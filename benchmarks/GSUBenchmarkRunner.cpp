#include "../tests/GSUInstructionModel.hpp"
#include "ObjectFile.hpp"
#include <fstream>
#include <iostream>
#include <limits>

namespace {
using DiscoGSU::Machine;
constexpr std::uint32_t Origin = 0x008000;
constexpr std::uint32_t ResultAddress = 0x700120;
constexpr std::uint32_t StackBegin = 0x70e000, StackEnd = 0x710000;
constexpr std::uint32_t Framebuffer = 0x706000;
constexpr std::size_t FramebufferBytes = 256 * 192 / 2;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint8_t> readBytes(const std::string& path, std::size_t maximum) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(static_cast<bool>(input), "cannot open benchmark artifact: " + path);
    const auto size = input.tellg();
    require(size > 0 && static_cast<std::uint64_t>(size) <= maximum,
            "invalid benchmark artifact size: " + path);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    // SAFETY: a bounded byte vector is filled through the permitted char view.
    require(static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()),
                                        static_cast<std::streamsize>(bytes.size()))),
            "truncated benchmark artifact: " + path);
    return bytes;
}

bool graphicsCase(const std::string& name) {
    return name == "triangle_fill" || name == "horizontal_span" ||
           name == "rom_palette_plot" || name == "ram_palette_plot";
}

void seedWord(Machine& machine, std::uint32_t address, std::uint16_t value) {
    machine.seed(address, static_cast<std::uint8_t>(value), false);
    machine.seed(address + 1, static_cast<std::uint8_t>(value >> 8), false);
}

std::uint16_t arithmeticResult() {
    std::uint16_t value = 0x1234;
    for (unsigned i = 0; i < 32; ++i) {
        const auto product = static_cast<std::uint16_t>(static_cast<std::uint32_t>(value) * 13 + 17);
        value = static_cast<std::uint16_t>(product ^ (value >> 3));
        value = static_cast<std::uint16_t>((static_cast<std::uint32_t>(value) << 1) | (value >> 15));
        const auto quotient = value / 7, remainder = value % 7;
        value = static_cast<std::uint16_t>((quotient + (remainder << 4)) ^
                                          (static_cast<std::uint32_t>(value) << 1));
    }
    return value;
}

std::uint16_t prepare(Machine& machine, const std::string& name) {
    if (name == "triangle_fill") return 9409;
    if (name == "horizontal_span" || name == "rom_palette_plot" || name == "ram_palette_plot") return 160;
    if (name == "memcpy") {
        for (unsigned i = 0; i < 128; ++i) {
            machine.seed(0x700400 + i, static_cast<std::uint8_t>(i ^ 0x5a), false);
            machine.seed(0x700500 + i, 0xa5, false);
        }
        machine.seed(0x7004ff, 0x5a, false);
        machine.seed(0x700580, 0xa5, false);
        return 128;
    }
    if (name == "function_call") {
        seedWord(machine, 0x700100, 3);
        return 2019; // 3 + 0 + 1 + ... + 63.
    }
    if (name == "switch_dense") {
        for (unsigned i = 0; i < 8; ++i) machine.seed(0x700400 + i, static_cast<std::uint8_t>(i), false);
        machine.seed(0x700408, 127, false);
        return 239;
    }
    if (name == "switch_sparse") {
        const std::uint16_t keys[] = {1, 17, 257, 1024, 2048, 8192, 16384, 30000, 77};
        for (std::size_t i = 0; i < 9; ++i) seedWord(machine, 0x700400 + static_cast<std::uint32_t>(i) * 2, keys[i]);
        return 135;
    }
    if (name == "arithmetic") {
        seedWord(machine, 0x700100, 0x1234);
        seedWord(machine, 0x700102, 7);
        return arithmeticResult();
    }
    throw std::runtime_error("unknown benchmark: " + name);
}

template<typename Snapshot>
void checkFramebuffer(const Snapshot& machine, const std::string& name) {
    // Build expected bitplanes directly from the geometric/palette contract,
    // independently of the graphics model's pixel caches and read operation.
    std::vector<std::uint8_t> expected(FramebufferBytes, 0);
    for (unsigned y = 0; y < 192; ++y) for (unsigned x = 0; x < 256; ++x) {
        unsigned color = 0;
        if (name == "triangle_fill") {
            if (y >= 48 && y <= 144 && x >= 128 - (y - 48) && x <= 128 + (y - 48))
                color = 1 + ((y - 48) >> 3);
        } else if (y == 40 && x >= 32 && x < 160) {
            color = name == "horizontal_span" ? 5 : 1 + ((x - 32) & 7);
        }
        const auto tile = static_cast<std::size_t>(x / 8) * 24 + y / 8;
        for (unsigned plane = 0; plane < 4; ++plane) {
            const auto offset = tile * 32 + (y & 7) * 2 + (plane / 2) * 16 + plane % 2;
            if (color & (1u << plane)) expected.at(offset) |= static_cast<std::uint8_t>(1u << (7 - x % 8));
        }
    }
    for (std::size_t i = 0; i < expected.size(); ++i)
        require(machine.byte(Framebuffer + static_cast<std::uint32_t>(i)) == expected[i],
                "framebuffer mismatch at byte " + std::to_string(i));
}

template<typename Snapshot>
void checkResult(const Snapshot& machine, const std::string& name, std::uint16_t expected) {
    require(machine.reg(0) == expected && machine.word(ResultAddress) == expected,
            "incorrect result in R0 or host-visible result word: expected " + std::to_string(expected));
    require(machine.ramBank() == 0, "benchmark did not initialize/restore RAMBR");
    if (graphicsCase(name)) checkFramebuffer(machine, name);
    if (name == "memcpy") {
        for (unsigned i = 0; i < 128; ++i) {
            require(machine.byte(0x700500 + i) == (i ^ 0x5a), "memcpy destination mismatch");
            require(machine.byte(0x700400 + i) == (i ^ 0x5a), "memcpy changed source bytes");
        }
        require(machine.byte(0x7004ff) == 0x5a && machine.byte(0x700580) == 0xa5,
                "memcpy wrote past the destination boundaries");
    }
}

// Owned snapshots from an external emulator, not a second execution model.
class MesenSnapshot {
public:
    MesenSnapshot(std::vector<std::uint8_t> ram, std::vector<std::uint8_t> registers)
        : ram_(std::move(ram)), registers_(std::move(registers)) {
        require(ram_.size() == 131072 && registers_.size() == 34, "invalid Mesen snapshot size");
        require(registers_[33] == 0, "Mesen program bank differs from the ROM benchmark profile");
    }
    std::uint8_t byte(std::uint32_t address) const {
        require(address >= 0x700000 && address < 0x720000, "snapshot address outside GSU RAM");
        return ram_.at(address - 0x700000);
    }
    std::uint16_t word(std::uint32_t address) const {
        return static_cast<std::uint16_t>(byte(address) | (static_cast<unsigned>(byte(address + 1)) << 8));
    }
    std::uint16_t reg(unsigned index) const {
        require(index < 16, "invalid snapshot register");
        return static_cast<std::uint16_t>(registers_.at(index * 2) |
                                         (static_cast<unsigned>(registers_.at(index * 2 + 1)) << 8));
    }
    unsigned ramBank() const { return registers_[32]; }
private:
    std::vector<std::uint8_t> ram_, registers_;
};

void writeSeed(const std::string& name, const std::string& path) {
    Machine fixture({0, 1}, Origin);
    prepare(fixture, name);
    std::vector<std::uint8_t> bytes(131072);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = fixture.byte(0x700000 + static_cast<std::uint32_t>(i));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "cannot write Mesen input fixture");
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(static_cast<bool>(output), "failed to finish Mesen input fixture");
}

void snapshotTests() {
    std::vector<std::uint8_t> ram(131072), registers(34);
    registers[0] = 128;
    ram[0x120] = 128;
    for (unsigned i = 0; i < 128; ++i) ram[0x400 + i] = ram[0x500 + i] = static_cast<std::uint8_t>(i ^ 0x5a);
    ram[0x4ff] = 0x5a; ram[0x580] = 0xa5;
    checkResult(MesenSnapshot(ram, registers), "memcpy", 128);
    auto rejects = [](const std::vector<std::uint8_t>& memory, const std::vector<std::uint8_t>& regs) {
        try { checkResult(MesenSnapshot(memory, regs), "memcpy", 128); }
        catch (const std::runtime_error&) { return true; }
        return false;
    };
    ram[0x580] = 0;
    require(rejects(ram, registers), "external snapshot accepted a boundary overwrite");
    ram[0x580] = 0xa5; registers[0] = 127;
    require(rejects(ram, registers), "external snapshot accepted an incorrect R0");
    registers[0] = 128; registers[32] = 1;
    require(rejects(ram, registers), "external snapshot accepted an incorrect RAMBR");
    registers[32] = 0; registers[33] = 0x70;
    require(rejects(ram, registers), "external snapshot accepted an incorrect PBR");
    registers[33] = 0; ram.pop_back();
    require(rejects(ram, registers), "external snapshot accepted truncated RAM");
    ram.push_back(0); registers.push_back(0);
    require(rejects(ram, registers), "external snapshot accepted trailing register bytes");
}

void counterTests() {
    Machine operands({0xf0, 0x4c, 0x4e, 0, 1}, Origin);
    operands.run();
    require(operands.metrics().instructions == 2 && operands.graphicsState("--plots") == 0 &&
            operands.graphicsState("--color") == 0, "operands or synthetic NOP counted as instructions");

    Machine memory({0xf0, 0, 0xe0, 0xf1, 3, 0, 0x21, 0x30, 0x19, 0x40, 0, 1}, Origin);
    memory.configureStackWindow(StackBegin, StackEnd);
    memory.run();
    const auto& metrics = memory.metrics();
    require(metrics.instructions == 7 && metrics.ram_loads == 1 && metrics.stores == 1 &&
            metrics.stack_loads == 1 && metrics.stack_stores == 1 && memory.reg(9) == 3,
            "word/frame memory counters are incorrect");
    memory.word(0x70e000); memory.byte(0x70e000);
    require(memory.metrics().ram_loads == 1, "host validation counted as GSU memory access");

    Machine otherBank({0xf0, 0, 0xe0, 0xf1, 3, 0, 0x21, 0x3d, 0x30, 0x3d, 0x40, 0, 1}, Origin, 0xfffe, 1);
    otherBank.configureStackWindow(StackBegin, StackEnd);
    otherBank.run();
    require(otherBank.metrics().ram_loads == 1 && otherBank.metrics().stores == 1 &&
            otherBank.metrics().stack_loads == 0 && otherBank.metrics().stack_stores == 0,
            "RAM bank alias or byte-load/store classification is incorrect");

    Machine rom({0xa0, 1, 0x3e, 0xdf, 0xa0, 2, 0x3f, 0xdf,
                 0xfe, 0, 0x90, 0xdf, 0xef, 0, 1}, Origin);
    rom.seed(0x029000, 3, true); rom.run();
    require(rom.metrics().instructions == 10 && rom.metrics().rom_reads == 2 &&
            rom.graphicsState("--getc") == 1 && rom.graphicsState("--color") == 0 &&
            rom.metrics().ram_loads == 0, "GETC/GETB confused with RAMB/ROMB");

    Machine pixel({0xa0, 3, 0x4e, 0x4c, 0x3d, 0x4c, 0, 1}, Origin);
    pixel.run();
    require(pixel.metrics().instructions == 6 && pixel.graphicsState("--plots") == 1 &&
            pixel.graphicsState("--color") == 1 && pixel.graphicsState("--rpix") == 1 &&
            pixel.metrics().ram_loads == 0 && pixel.metrics().stores == 0,
            "graphics operations counted as ordinary memory instructions");

    Machine branch({0xf0, 0, 0, 0x05, 3, 0xd0, 0xd0, 0xd0, 0, 1}, Origin);
    branch.run();
    require(branch.metrics().instructions == 4 && branch.metrics().branches == 1 &&
            branch.metrics().taken_branches == 1 && branch.metrics().jumps == 0 && branch.reg(0) == 1,
            "branch delay-slot or taken-branch counts are incorrect");

    Machine notTaken({0xf0, 1, 0, 0xd0, 0x09, 2, 1, 0, 1}, Origin);
    notTaken.run();
    require(notTaken.metrics().branches == 1 && notTaken.metrics().taken_branches == 0,
            "untaken conditional branch counted as taken");

    Machine call({0x94, 0xff, 8, 0x80, 1, 0, 1, 1, 0xf0, 149, 0, 0x9b, 1}, Origin);
    call.run();
    require(call.metrics().instructions == 7 && call.metrics().calls == 1 && call.metrics().jumps == 2,
            "LINK/call/return counters are incorrect");

    bool rejected = false;
    try { call.configureStackWindow(StackEnd, StackBegin); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "invalid stack windows were accepted");
}

void report(const std::string& name, const ObjectFile& object, const Machine& machine,
            std::size_t payload_bytes, std::uint16_t result) {
    const auto& m = machine.metrics();
    std::cout << "{\"name\":\"" << name << "\",\"result\":" << result
        << ",\"code_bytes\":" << object.code_section.size()
        << ",\"data_bytes\":" << object.data_section.size()
        << ",\"payload_bytes\":" << payload_bytes
        << ",\"instructions_executed\":" << m.instructions
        << ",\"loads\":" << m.ram_loads + m.rom_reads
        << ",\"ram_loads\":" << m.ram_loads << ",\"rom_reads\":" << m.rom_reads
        << ",\"stores\":" << m.stores
        << ",\"stack_accesses\":" << m.stack_loads + m.stack_stores
        << ",\"stack_loads\":" << m.stack_loads << ",\"stack_stores\":" << m.stack_stores
        << ",\"plot\":" << machine.graphicsState("--plots")
        << ",\"color\":" << machine.graphicsState("--color")
        << ",\"getc\":" << machine.graphicsState("--getc")
        << ",\"rpix\":" << machine.graphicsState("--rpix")
        << ",\"branches\":" << m.branches << ",\"taken_branches\":" << m.taken_branches
        << ",\"jumps\":" << m.jumps << ",\"calls\":" << m.calls
        << ",\"cache\":" << machine.cacheRequests() << "}\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") {
            counterTests();
            snapshotTests();
            std::cout << "GSU benchmark counter checks passed\n";
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--mesen-fixture") {
            writeSeed(argv[2], argv[3]);
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--verify-mesen") {
            const std::string name(argv[2]);
            Machine fixture({0, 1}, Origin);
            const auto expected = prepare(fixture, name);
            const MesenSnapshot snapshot(readBytes(argv[3], 131072), readBytes(argv[4], 34));
            checkResult(snapshot, name, expected);
            require(snapshot.reg(10) == 0xfffa, "Mesen entry frame/stack mismatch after STOP");
            std::cout << "Mesen result, memory and stack checks passed\n";
            return 0;
        }
        require(argc == 4, "usage: disco_gsu_benchmarks case payload.bin linked.o | --self-test");
        const std::string name(argv[1]);
        const auto payload = readBytes(argv[2], 32768);
        const auto object = ObjectFile::readBytes(readBytes(argv[3], 1024 * 1024));
        require(object.config.target == TargetKind::GSU && object.config.mapping == MemoryMapping::LoROM &&
                object.config.code_start_address == Origin, "benchmark placement/profile mismatch");
        require(object.code_section.size() + object.data_section.size() == payload.size() &&
                std::equal(object.code_section.begin(), object.code_section.end(), payload.begin()) &&
                std::equal(object.data_section.begin(), object.data_section.end(),
                           payload.begin() + object.code_section.size()), "linked assembly does not match payload bytes");
        const auto& bitmap = object.config.bitmap;
        require(bitmap.enabled == graphicsCase(name), "benchmark bitmap metadata mismatch");
        if (bitmap.enabled)
            require(bitmap.base == 0x6000 && bitmap.scmr() == 0x21, "benchmark bitmap layout mismatch");
        Machine machine(payload, Origin, 0x1234, 1);
        machine.configureStackWindow(StackBegin, StackEnd);
        if (bitmap.enabled) machine.configureScreen(bitmap.scmr(), bitmap.scbr());
        const auto expected = prepare(machine, name);
        machine.run();
        checkResult(machine, name, expected);
        report(name, object, machine, payload.size(), expected);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
