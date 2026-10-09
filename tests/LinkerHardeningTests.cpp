#include "ABI.hpp"
#include "LinkerDriver.hpp"
#include "ObjectFile.hpp"
#include "GSUCodeLayout.hpp"
#include "GSUInstructionModel.hpp"
#include "RuntimeInitialization.hpp"
#include "TestTempDirectory.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
Bytes readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Cannot read test output: " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}
void writeBytes(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(static_cast<bool>(output), "Cannot write test fixture: " + path.string());
}
// Restore borrowed stream buffers even if a library assertion throws.
class Capture {
public:
    Capture() : out_(std::cout.rdbuf(buffer_.rdbuf())), err_(std::cerr.rdbuf(buffer_.rdbuf())) {}
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    ~Capture() { std::cout.rdbuf(out_); std::cerr.rdbuf(err_); }
    std::string text() const { return buffer_.str(); }
private:
    std::ostringstream buffer_;
    std::streambuf *out_, *err_;
};

ObjectFile entry() {
    ObjectFile object;
    object.code_section = {0x00, 0x01};
    object.symbol_table.push_back({"main", SymbolSection::CODE, 0});
    return object;
}
ObjectFile addressObject(SymbolSection target, RelocationType type, bool local = false, bool end = false) {
    auto object = entry();
    object.code_section = {0xff, 0, 0, 0, 1};
    object.data_section = {0x95, 0};
    object.ram_section = {0x42, 0};
    const auto name = local ? std::string(1, '\x01') + "target" : "target";
    const auto size = target == SymbolSection::CODE ? object.code_section.size() :
        target == SymbolSection::DATA ? object.data_section.size() : object.ram_section.size();
    const auto offset = end ? static_cast<std::uint32_t>(size) : target == SymbolSection::CODE ? 3u : 0u;
    object.symbol_table.push_back({name, target, offset});
    object.relocation_table.push_back({name, SymbolSection::CODE, 0, type});
    return object;
}

class Cases {
public:
    explicit Cases(const std::filesystem::path& root) : root_(root) {}
    Bytes link(const std::string& name, const std::vector<ObjectFile>& objects,
               const std::vector<std::string>& options = {}, const std::string& diagnostic = "") {
        const auto directory = root_ / name;
        std::filesystem::create_directory(directory);
        const auto binary = directory / "payload.bin", assembly = directory / "final.s";
        std::vector<std::string> arguments{"discld"};
        for (std::size_t index = 0; index < objects.size(); ++index) {
            const auto path = directory / (std::to_string(index) + ".o");
            auto object = objects[index]; object.write(path.string());
            arguments.push_back(path.string());
        }
        arguments.insert(arguments.end(), options.begin(), options.end());
        arguments.insert(arguments.end(), {"--emit-asm", assembly.string(), "-o", binary.string()});
        const Bytes sentinel{0x50, 0x52, 0x45, 0x53, 0x45, 0x52, 0x56, 0x45};
        if (!diagnostic.empty()) { writeBytes(binary, sentinel); writeBytes(assembly, sentinel); }
        int status; std::string output;
        { Capture capture; status = runLinker(std::move(arguments)); output = capture.text(); }
        if (diagnostic.empty()) {
            require(status == 0, name + ": unexpected link failure:\n" + output);
            require(!readBytes(assembly).empty(), name + ": missing final assembly");
            return readBytes(binary);
        }
        require(status == 1 && output.find(diagnostic) != std::string::npos,
            name + ": expected diagnostic '" + diagnostic + "':\n" + output);
        require(readBytes(binary) == sentinel && readBytes(assembly) == sentinel,
            name + ": invalid link configuration overwrote an output");
        return {};
    }
private:
    const std::filesystem::path& root_;
};

void relocationCases(Cases& cases) {
    ObjectFile caller;
    caller.code_section = {0xff,0,0,1,0,1};
    caller.symbol_table = {{"main",SymbolSection::CODE,0}};
    caller.relocation_table = {{"hot",SymbolSection::CODE,0,RelocationType::ADDR16_JAL}};
    ObjectFile hot;
    hot.code_section.assign(15,1); hot.code_section.insert(hot.code_section.end(),{2,0xf0,149,0,0,1});
    hot.symbol_table = {{GSUCodeLayout::CacheAlignmentSymbol,SymbolSection::CODE,0},{"hot",SymbolSection::CODE,15},
        {"\x01" "local",SymbolSection::CODE,16}};
    hot.relocation_table = {{"\x01" "local",SymbolSection::CODE,16,RelocationType::ADDR16_IWT}};
    for (const auto origin : {0x700900u,0x700903u,0x70090fu}) {
        const auto payload = cases.link("cache-origin-" + std::to_string(origin),{caller,hot},
            {"--origin",std::to_string(origin),"--init-runtime","--stack-pointer","0x2000"});
        const auto caller_base = 11u;
        const auto hot_base = ((origin + caller_base + caller.code_section.size() + 15u) & ~15u) - origin;
        const auto entry = origin + hot_base + 15u;
        require(payload.at(caller_base + 1) == (entry & 255) && payload.at(caller_base + 2) == ((entry >> 8) & 255) &&
                payload.at(hot_base + 15) == 2 && ((entry + 1) & 15) == 0,
                "Final origin/startup/multifile CACHE alignment or public relocation is incorrect");
        require(payload.at(hot_base + 17) == ((entry + 1) & 255) &&
                payload.at(hot_base + 18) == (((entry + 1) >> 8) & 255),
                "Aligned object local relocation uses the wrong CODE base");
    }
    // Independent expected bytes cover CODE, per-object aligned DATA and RAM
    // bases. Operand offsets must not be based on the first object's DATA.
    ObjectFile first;
    first.code_section = {0xff, 0, 0, 0xa0, 0, 0xf0, 0, 0, 0xf1, 0, 0, 0, 1};
    first.data_section = {0xf2, 0, 0, 0x95}; first.data_alignment = 2;
    first.ram_section = {0x12, 0x34};
    first.symbol_table = {{"main", SymbolSection::CODE, 0}, {"a", SymbolSection::DATA, 0},
        {"ra", SymbolSection::RAM, 0}};
    first.relocation_table = {{"other", SymbolSection::CODE, 0, RelocationType::ADDR16_JAL},
        {"b", SymbolSection::CODE, 3, RelocationType::ADDR24_BANK},
        {"b", SymbolSection::CODE, 5, RelocationType::ADDR24_OFFSET},
        {"rb", SymbolSection::CODE, 8, RelocationType::ADDR16_RAM},
        {"b", SymbolSection::DATA, 0, RelocationType::ADDR16_IWT}};
    ObjectFile second;
    second.code_section = {0, 1}; second.data_alignment = 8; second.ram_alignment = 8;
    second.data_section = {0xf3, 0, 0}; second.ram_section = {0x56, 0x78};
    second.symbol_table = {{"other", SymbolSection::CODE, 0}, {"b", SymbolSection::DATA, 0},
        {"rb", SymbolSection::RAM, 0}};
    second.relocation_table = {{"a", SymbolSection::DATA, 0, RelocationType::ADDR16_IWT}};
    const auto payload = cases.link("multifile-bases", {first, second}, {"--host-initialized-globals"});
    const Bytes expected{0xff, 0x0d, 0x80, 0xa0, 0x00, 0xf0, 0x18, 0x80, 0xf1, 0x08, 0x04, 0, 1,
        0, 1, 1, 0xf2, 0x18, 0x80, 0x95, 0, 0, 0, 0, 0xf3, 0x10, 0x80};
    require(payload == expected, "multifile relocation/alignment bytes differ from the golden payload");

    for (const bool local : {false, true}) {
        const std::string suffix = local ? "local" : "global";
        for (const auto section : {SymbolSection::DATA, SymbolSection::RAM})
            cases.link("call-non-code-" + suffix + std::to_string(static_cast<unsigned>(section)),
                {addressObject(section, RelocationType::ADDR16_JAL, local)},
                {"--host-initialized-globals"}, "Call relocation must target an instruction in CODE");
        cases.link("call-end-" + suffix, {addressObject(SymbolSection::CODE, RelocationType::ADDR16_JAL, local, true)},
            {"--host-initialized-globals"}, "Call relocation must target an instruction in CODE");
        cases.link("ram-to-data-" + suffix, {addressObject(SymbolSection::DATA, RelocationType::ADDR16_RAM, local)},
            {"--host-initialized-globals"}, "RAM relocation must target static RAM");
    }
    auto missing = entry(); missing.code_section = {0xff, 0, 0};
    missing.relocation_table = {{"missing", SymbolSection::CODE, 0, RelocationType::ADDR16_JAL}};
    cases.link("undefined", {missing}, {}, "Undefined symbol");
    missing.relocation_table.front().target_symbol_name = std::string(1, '\x01') + "missing";
    cases.link("undefined-local", {missing}, {}, "Undefined local symbol");
    missing.relocation_table.front().target_symbol_name = GSUAbi::NearRamBankSymbol;
    cases.link("invalid-bank-context-type", {missing}, {}, "Bank-context symbol requires");

    auto rom = entry(); rom.code_section = {0xa0, 0, 0xf0, 0, 0, 0, 1};
    rom.data_section = {5}; rom.symbol_table.push_back({"palette", SymbolSection::DATA, 0});
    rom.relocation_table = {{GSUAbi::NearRomBankSymbol, SymbolSection::CODE, 0, RelocationType::ADDR24_BANK},
        {"palette", SymbolSection::CODE, 2, RelocationType::ADDR16_IWT}};
    require(cases.link("rom-context", {rom})[4] == 0x80, "near ROM reference lost its bank");
    cases.link("rom-bank-mismatch", {rom}, {"--rom-bank", "1"}, "Near ROM data relocation does not match");
    cases.link("rom-data-in-ram", {rom}, {"--origin", "0x706000"}, "Near ROM data relocation does not match");
}

void runtimeCases(Cases& cases) {
    const auto main = entry();
    for (const auto bank : {0u, 1u}) {
        const auto bytes = cases.link("startup-bank-" + std::to_string(bank), {main},
            {"--origin", "0x700900", "--init-runtime", "--ram-bank", std::to_string(bank), "--stack-pointer", "0x3000"});
        const Bytes expected{0xa0, static_cast<std::uint8_t>(bank), 0x3e, 0xdf, 0xfa, 0, 0x30,
            0xff, 0x0b, 0x09, 1, 0, 1};
        require(bytes == expected, "runtime startup differs from independent opcode fixture");
        DiscoGSU::Machine runtime(bytes,0x700900,0x7777,static_cast<std::uint8_t>(1-bank)); runtime.run();
        require(runtime.ramBank() == bank && runtime.reg(10) == 0x3000, "Compact startup did not restore poisoned RAMBR/R10");
        const auto short_bytes = cases.link("startup-signed-stack-" + std::to_string(bank), {main},
            {"--origin", "0x700900", "--init-runtime", "--ram-bank", std::to_string(bank), "--stack-pointer", "0xfffe"});
        const Bytes short_expected{0xa0,static_cast<std::uint8_t>(bank),0x3e,0xdf,0xaa,0xfe,0xff,0x0a,0x09,1,0,1};
        require(short_bytes == short_expected, "Sign-extended stack startup has wrong bytes/entry relocation");
        DiscoGSU::Machine short_runtime(short_bytes,0x700900,0x7777,static_cast<std::uint8_t>(1-bank)); short_runtime.run();
        require(short_runtime.ramBank() == bank && short_runtime.reg(10) == 0xfffe,
            "IBT stack startup lost sign extension or bank initialization");
    }
    for (const auto stack : {0x7eu,0x80u,0xfeu,0xff80u,0xfffeu}) {
        const auto bytes = cases.link("startup-literal-boundary-" + std::to_string(stack),{main},
            {"--init-runtime","--stack-pointer",std::to_string(stack)});
        const bool small = stack <= 0x7f || stack >= 0xff80;
        require(bytes.at(4) == (small ? 0xaa : 0xfa) && bytes.size() == (small ? 12u : 13u),
            "Runtime materialization confused a zero-extended byte with a signed byte");
        DiscoGSU::Machine runtime(bytes,0x008000); runtime.run();
        require(runtime.reg(10) == stack, "Runtime literal boundary produced a different 16-bit value");
    }
    for (const auto& invalid : std::vector<std::pair<std::string, std::string>>{
            {"0", "Initial stack pointer must be even"}, {"7", "Initial stack pointer must be even"},
            {"9", "Initial stack pointer must be even"}, {"0xffff", "out of range"},
            {"0x10000", "out of range"}})
        cases.link("bad-stack-" + invalid.first, {main}, {"--init-runtime", "--stack-pointer", invalid.first}, invalid.second);
    cases.link("runtime-options-without-startup", {main}, {"--stack-pointer", "0x2000"}, "require --init-runtime");
    cases.link("bad-ram-bank", {main}, {"--ram-bank", "2"}, "out of range");
    cases.link("bad-rom-bank", {main}, {"--rom-bank", "0x60"}, "out of range");
    cases.link("bad-origin", {main}, {"--origin", "0x7e8000"}, "outside supported ROM/RAM");
    cases.link("wide-origin", {main}, {"--origin", "0x1000000"}, "out of range");
    cases.link("wrong-execution-memory", {main}, {"--execution-memory", "ram"}, "does not match selected execution memory");
    cases.link("stack-in-payload", {main}, {"--init-runtime", "--origin", "0x701ff8"}, "Initial stack word overlaps");
    cases.link("no-entry", {main}, {"--init-runtime", "--entry", "absent"}, "Runtime entry must name");
    auto end = main; end.symbol_table.front().offset = 2;
    cases.link("end-entry", {end}, {"--init-runtime"}, "Runtime entry must name");
    auto ram_entry = main; ram_entry.ram_section = {0, 0}; ram_entry.symbol_table.front().section = SymbolSection::RAM;
    cases.link("ram-entry", {ram_entry}, {"--init-runtime"}, "Runtime entry must name");

    auto guarded = entry(); guarded.code_section = {0xf3, 0, 0, 0, 1};
    guarded.relocation_table = {{std::string(GSUAbi::StackLimitPrefix) + "4", SymbolSection::CODE, 0, RelocationType::ADDR16_RAM}};
    const auto floor = cases.link("ram-code-stack-floor", {guarded}, {"--origin", "0x701000", "--init-runtime"});
    require(floor[12] == 0x14 && floor[13] == 0x10, "stack guard must reserve RAM payload below R10");
    cases.link("ram-code-no-frame-space", {guarded},
        {"--origin", "0x701000", "--init-runtime", "--stack-pointer", "0x1012"}, "no room for the requested stack frame");
    guarded.relocation_table.front().target_symbol_name = std::string(GSUAbi::StackLimitPrefix) + "65535";
    cases.link("stack-threshold-overflow", {guarded}, {"--origin", "0x701000", "--init-runtime"}, "no room for the requested stack frame");
    for (const auto& value : std::vector<std::string>{"", "bad", "-1", "9999999999"}) {
        guarded.relocation_table.front().target_symbol_name = std::string(GSUAbi::StackLimitPrefix) + value;
        cases.link("malformed-stack-context-" + value, {guarded}, {}, "option value");
    }

    auto reserved = main; reserved.symbol_table.front().name = GSUAbi::NearRamBankSymbol;
    cases.link("defined-reserved-bank", {reserved}, {}, "reserved for the pointer ABI");
    reserved.symbol_table.front().name = std::string(GSUAbi::StackLimitPrefix) + "4";
    cases.link("defined-reserved-stack", {reserved}, {}, "reserved for the pointer ABI");

    auto globals = main; globals.ram_section = {0x95, 0};
    globals.symbol_table.push_back({"result", SymbolSection::RAM, 0});
    cases.link("globals-need-contract", {globals}, {}, "require --init-runtime");
    cases.link("double-init-contract", {globals}, {"--init-runtime", "--host-initialized-globals"}, "not both");
    cases.link("odd-static-origin", {globals}, {"--init-runtime", "--ram-origin", "0x401"}, "word-aligned");
    cases.link("static-stack-overlap", {globals}, {"--init-runtime", "--ram-origin", "0x2000"}, "below the initial stack pointer");
    cases.link("static-payload-overlap", {globals}, {"--origin", "0x700400", "--init-runtime"}, "Static RAM overlaps");
    cases.link("static-bank-crossing", {globals, globals},
        {"--ram-origin", "0xfffe", "--init-runtime"}, "crosses a RAM bank boundary");
}

void runtimeEncodingCases() {
    const std::vector<std::pair<std::uint16_t, Bytes>> stack_encodings{
        {std::uint16_t{0x007f}, {0xaa,0x7f}}, {std::uint16_t{0x0080}, {0xfa,0x80,0x00}},
        {std::uint16_t{0xff7f}, {0xfa,0x7f,0xff}}, {std::uint16_t{0xff80}, {0xaa,0x80}},
        {std::uint16_t{0xfffe}, {0xaa,0xfe}}
    };
    for (const auto bank : {0u,1u}) for (const auto& encoding : stack_encodings) {
        auto expected = Bytes{0xa0,static_cast<std::uint8_t>(bank),0x3e,0xdf};
        expected.insert(expected.end(),encoding.second.begin(),encoding.second.end());
        const auto patch = expected.size() + 1;
        expected.insert(expected.end(),{0xff,0,0,1});
        const auto startup = makeRuntimeInitialization(static_cast<std::uint8_t>(bank),encoding.first,0,{});
        require(startup.bytes == expected && startup.entry_patch == patch,
            "Runtime startup bytes/entry patch changed at an IBT/IWT boundary");
    }
    const auto check = [](std::uint16_t origin, const Bytes& image, const Bytes& expected) {
        const auto startup = makeRuntimeInitialization(0,0xfffe,origin,image);
        require(startup.bytes == expected && startup.entry_patch == expected.size()-3,
            "Runtime RAM initializer differs from its golden encoding");
    };
    check(0,{0},{0xa0,0,0x3e,0xdf,0xaa,0xfe,0xa0,0,0xa1,0,0xa2,1,
        0x3d,0x31,0xd1,0xe2,0x08,0xfa,1,0xff,0,0,1});
    check(0,{0x80,0,0x95},{0xa0,0,0x3e,0xdf,0xaa,0xfe,0xa0,0,0xa1,0,0xa2,3,
        0x3d,0x31,0xd1,0xe2,0x08,0xfa,1,
        0xa1,0,0xf0,0x80,0,0x31,0xa1,2,0xf0,0x95,0,0x3d,0x31,0xff,0,0,1});
    check(0xfffe,{0xfe,0xff},{0xa0,0,0x3e,0xdf,0xaa,0xfe,0xa0,0,0xa1,0xfe,0xa2,2,
        0x3d,0x31,0xd1,0xe2,0x08,0xfa,1,0xa1,0xfe,0xa0,0xfe,0x31,0xff,0,0,1});

    for (const auto size : {65535u,65536u}) {
        const auto startup = makeRuntimeInitialization(1,0xfffe,0,Bytes(size,0x12));
        const auto bound = 27u + 7u * ((static_cast<std::size_t>(size) + 1u) / 2u);
        require(startup.bytes.size() <= bound && startup.entry_patch == startup.bytes.size()-3,
            "Runtime initializer exceeded its validated emission bound");
        const auto tail = size == 65535u ? Bytes{0xa1,0xfe,0xa0,0x12,0x3d,0x31,0xff,0,0,1}
                                       : Bytes{0xa1,0xfe,0xf0,0x12,0x12,0x31,0xff,0,0,1};
        require(startup.bytes.size() >= tail.size(),"Runtime initializer was truncated before the final store");
        require(Bytes(startup.bytes.end()-static_cast<std::ptrdiff_t>(tail.size()),startup.bytes.end()) == tail,
            "Runtime initializer misencoded the final byte/word of a full RAM bank");
    }
    const auto reject = [](std::uint8_t bank, std::uint16_t origin, const Bytes& image) {
        bool rejected = false;
        try { (void)makeRuntimeInitialization(bank,0x2000,origin,image); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected,"Runtime initializer accepted an invalid bank/allocation");
    };
    reject(2,0,{}); reject(0,1,{}); reject(0,0xfffe,{0,0,0}); reject(0,0,Bytes(65537,0));
}

void runtimeImageCases() {
    const Bytes image{0x7f,0,0x80,0,0xfe,0,0x80,0xff,0xfe,0xff,0x95};
    for (const auto bank : {0u,1u}) for (const auto origin : {0x7eu,0x80u,0xfff4u}) {
        auto startup = makeRuntimeInitialization(static_cast<std::uint8_t>(bank),0xfffe,
            static_cast<std::uint16_t>(origin),image);
        const auto destination = 0x8000u + startup.bytes.size();
        require(startup.bytes.at(startup.entry_patch-1) == 0xff, "Runtime entry patch was shortened to IBT");
        startup.bytes.at(startup.entry_patch) = static_cast<std::uint8_t>(destination);
        startup.bytes.at(startup.entry_patch+1) = static_cast<std::uint8_t>(destination >> 8);
        startup.bytes.insert(startup.bytes.end(),{0,1});
        DiscoGSU::Machine runtime(startup.bytes,0x008000,0x7777,static_cast<std::uint8_t>(1-bank));
        for (unsigned offset = 0; offset < 65536; ++offset) runtime.seed(0x700000 + bank*65536 + offset,0xa5,false);
        runtime.run();
        require(runtime.reg(10) == 0xfffe && runtime.ramBank() == bank, "Global initialization changed stack/bank setup");
        for (unsigned offset = 0; offset < 65536; ++offset) {
            const auto expected = offset >= origin && offset-origin < image.size() ? image[offset-origin] : 0xa5;
            require(runtime.byte(0x700000+bank*65536+offset) == expected,
                "Compact global initializer changed a value or a byte outside the allocation");
            require(runtime.byte(0x700000+(1-bank)*65536+offset) == 0, "Global initializer wrote the other RAM bank");
        }
    }
    // Count 0 represents exactly 64 KiB: it must wrap once, not skip clearing.
    auto startup = makeRuntimeInitialization(0,0x2000,0,Bytes(65536,0));
    const auto destination = 0x8000u + startup.bytes.size();
    startup.bytes.at(startup.entry_patch) = static_cast<std::uint8_t>(destination);
    startup.bytes.at(startup.entry_patch+1) = static_cast<std::uint8_t>(destination >> 8);
    startup.bytes.insert(startup.bytes.end(),{0,1});
    DiscoGSU::Machine runtime(startup.bytes,0x008000);
    for (unsigned offset = 0; offset < 65536; ++offset) runtime.seed(0x700000+offset,0xa5,false);
    runtime.run();
    for (unsigned offset = 0; offset < 65536; ++offset)
        require(runtime.byte(0x700000+offset) == 0, "Compact 64 KiB clearing loop ended early");
}

void metadataCases(Cases& cases) {
    auto first = entry(); ObjectFile second; second.code_section = {0, 1};
    second.symbol_table.push_back({"helper", SymbolSection::CODE, 0});
    auto incompatible = second; incompatible.config.target = TargetKind::SPC700;
    cases.link("mixed-targets", {first, incompatible}, {}, "incompatible target configurations");
    incompatible = second; incompatible.config.mapping = MemoryMapping::HiROM;
    cases.link("mixed-mappings", {first, incompatible}, {}, "incompatible target configurations");
    incompatible = second; incompatible.config.code_start_address = 0x9000;
    cases.link("mixed-origins", {first, incompatible}, {}, "incompatible target configurations");
    require(cases.link("override-origins", {first, incompatible}, {"--origin", "0x706000"}).size() == 4,
        "explicit CLI origin must override all input origin defaults");
    cases.link("wrong-target", {first}, {"--target", "spc700"}, "does not match input objects");
    cases.link("target-before-origin", {first}, {"--target", "spc700", "--origin", "0x200"}, "target does not match input objects");
    cases.link("wrong-mapping", {first}, {"--memory-mapping", "hirom"}, "does not match input objects");
    auto spc = first; spc.config.target = TargetKind::SPC700;
    cases.link("spc-startup", {spc}, {"--init-runtime"}, "only supported for GSU");
    for (const auto section : {SymbolSection::CODE, SymbolSection::DATA, SymbolSection::RAM}) {
        auto duplicate = second; duplicate.symbol_table.front() = {"main", section, 0};
        if (section == SymbolSection::DATA) duplicate.data_section = {0};
        if (section == SymbolSection::RAM) duplicate.ram_section = {0, 0};
        cases.link("duplicate-" + std::to_string(static_cast<unsigned>(section)), {first, duplicate},
            {"--host-initialized-globals"}, "defined multiple times");
    }
    // Local names have object scope, unlike exported/global names.
    first.symbol_table.push_back({std::string(1, '\x01') + "private", SymbolSection::CODE, 0});
    second.symbol_table.push_back(first.symbol_table.back());
    require(cases.link("private-symbols", {first, second}).size() == 4, "private symbols collided across objects");
    first.config.bitmap.enabled = true; first.config.bitmap.depth = 4;
    first.config.bitmap.height = 192; first.config.bitmap.base = 0;
    second.config.bitmap = first.config.bitmap;
    cases.link("same-bitmap", {first, second}, {"--init-runtime", "--stack-pointer", "0x8000"});
    cases.link("one-bitmap", {entry(), second}, {"--init-runtime", "--stack-pointer", "0x8000"});
    second.config.bitmap.depth = 2;
    cases.link("different-bitmaps", {first, second}, {}, "conflicting host bitmap configurations");
    cases.link("bitmap-payload-overlap", {first}, {"--origin", "0x704000"}, "Bitmap framebuffer overlaps the linked RAM payload");
    auto static_bitmap = first; static_bitmap.ram_section = {0};
    cases.link("bitmap-static-overlap", {static_bitmap}, {"--init-runtime", "--stack-pointer", "0x8000"}, "Bitmap framebuffer overlaps static RAM");
    cases.link("bitmap-stack-overlap", {first}, {"--init-runtime"}, "Bitmap framebuffer overlaps the initial stack word");
    // Alignment padding counts toward the actual program-bank size.
    auto aligned = entry(); aligned.data_section = {0}; aligned.data_alignment = 8;
    cases.link("alignment-crosses-bank", {aligned}, {"--origin", "0x00fffd"}, "program-bank boundary");
}

void malformedCases(const std::filesystem::path& root) {
    const auto object = root / "malformed.o", output = root / "malformed.bin";
    const Bytes sentinel{0x95, 0x42}; writeBytes(output, sentinel);
    for (const auto& bytes : std::vector<Bytes>{{}, {'D', 'I', 'S', 'C', 'O', 255}, {1, 2, 3}}) {
        writeBytes(object, bytes);
        int result; { Capture capture; result = runLinker({"discld", object.string(), "-o", output.string()}); }
        require(result == 1 && readBytes(output) == sentinel, "malformed object link must preserve existing output");
    }
    auto valid = entry(); valid.write(object.string());
    const auto input = readBytes(object);
    int result; std::string message;
    { Capture capture; result = runLinker({"discld", object.string(), "-o", object.string()}); message = capture.text(); }
    require(result == 1 && message.find("overwrite an input object") != std::string::npos && readBytes(object) == input,
        "link output must not overwrite its input");
    { Capture capture; result = runLinker({"discld", object.string(), "-o", output.string(), "--emit-asm", output.string()}); }
    require(result == 1 && readBytes(output) == sentinel, "binary/assembly destinations must differ");
}

void aggregateBudgetCase(const std::filesystem::path& root) {
    const auto path = root / "large.o", output = root / "large.bin";
    {
        auto object = entry(); object.config.target = TargetKind::SPC700;
        // SPC700 container fixtures bypass the GSU one-bank limit, isolating
        // the aggregate record budget. Repeated inputs would otherwise reach
        // duplicate-symbol validation; that is not the expected rejection.
        object.code_section.assign(ObjectFile::MaxSectionBytes, 1);
        object.write(path.string());
    }
    const Bytes sentinel{0x95, 0x42}; writeBytes(output, sentinel);
    int result; std::string message;
    { Capture capture; result = runLinker({"discld", path.string(), path.string(), "-o", output.string()});
      message = capture.text(); }
    require(result == 1 && message.find("Combined link inputs exceed") != std::string::npos && readBytes(output) == sentinel,
        "aggregate link budget did not reject input before layout/output");
}
} // namespace

int main() {
    try {
        TestTempDirectory temporary("linker-tests"); Cases cases(temporary.path());
        relocationCases(cases); runtimeCases(cases); runtimeEncodingCases(); runtimeImageCases(); metadataCases(cases); malformedCases(temporary.path());
        aggregateBudgetCase(temporary.path());
        temporary.cleanup();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
