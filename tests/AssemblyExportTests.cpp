#include "Assembler.hpp"
#include "AssemblyGenerator.hpp"
#include <iostream>
#include <stdexcept>

#include "CompilerError.hpp"
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint8_t> assembleCode(const std::string& statement) {
    return Assembler().assemble(".segment \"CODE\"\n    " + statement + "\n").code_section;
}

bool rejects(const std::string& statement) {
    try {
        assembleCode(statement);
    } catch (const CompilerError&) {
        return true;
    }
    return false;
}

// Every ALU mnemonic with register and immediate operands at the range edges.
// The bytes were produced once by the independent WLA-DX SuperFX assembler
// (wla-superfx 10.7) and agree with casfx.inc; an empty row is a form both
// reject. Regenerate only from that oracle, never from discas output.
struct ArithmeticCase {
    const char* mnemonic;
    const char* operand;
    std::vector<std::uint8_t> bytes;
};

void check(const ObjectFile& object);

void arithmeticEncodings() {
    const std::vector<ArithmeticCase> cases = {
        {"add", "#3", {0x3e, 0x53}},
        {"add", "#1", {0x3e, 0x51}},
        {"add", "#15", {0x3e, 0x5f}},
        {"add", "#0", {0x3e, 0x50}},
        {"add", "r3", {0x53}},
        {"add", "r1", {0x51}},
        {"add", "r0", {0x50}},
        {"adc", "#3", {0x3f, 0x53}},
        {"adc", "#1", {0x3f, 0x51}},
        {"adc", "#15", {0x3f, 0x5f}},
        {"adc", "#0", {0x3f, 0x50}},
        {"adc", "r3", {0x3d, 0x53}},
        {"adc", "r1", {0x3d, 0x51}},
        {"adc", "r0", {0x3d, 0x50}},
        {"sub", "#3", {0x3e, 0x63}},
        {"sub", "#1", {0x3e, 0x61}},
        {"sub", "#15", {0x3e, 0x6f}},
        {"sub", "#0", {0x3e, 0x60}},
        {"sub", "r3", {0x63}},
        {"sub", "r1", {0x61}},
        {"sub", "r0", {0x60}},
        {"sbc", "#3", {}},
        {"sbc", "#1", {}},
        {"sbc", "#15", {}},
        {"sbc", "#0", {}},
        {"sbc", "r3", {0x3d, 0x63}},
        {"sbc", "r1", {0x3d, 0x61}},
        {"sbc", "r0", {0x3d, 0x60}},
        {"cmp", "#3", {}},
        {"cmp", "#1", {}},
        {"cmp", "#15", {}},
        {"cmp", "#0", {}},
        {"cmp", "r3", {0x3f, 0x63}},
        {"cmp", "r1", {0x3f, 0x61}},
        {"cmp", "r0", {0x3f, 0x60}},
        {"and", "#3", {0x3e, 0x73}},
        {"and", "#1", {0x3e, 0x71}},
        {"and", "#15", {0x3e, 0x7f}},
        {"and", "#0", {}},
        {"and", "r3", {0x73}},
        {"and", "r1", {0x71}},
        {"and", "r0", {}},
        {"bic", "#3", {0x3f, 0x73}},
        {"bic", "#1", {0x3f, 0x71}},
        {"bic", "#15", {0x3f, 0x7f}},
        {"bic", "#0", {}},
        {"bic", "r3", {0x3d, 0x73}},
        {"bic", "r1", {0x3d, 0x71}},
        {"bic", "r0", {}},
        {"or", "#3", {0x3e, 0xc3}},
        {"or", "#1", {0x3e, 0xc1}},
        {"or", "#15", {0x3e, 0xcf}},
        {"or", "#0", {}},
        {"or", "r3", {0xc3}},
        {"or", "r1", {0xc1}},
        {"or", "r0", {}},
        {"xor", "#3", {0x3f, 0xc3}},
        {"xor", "#1", {0x3f, 0xc1}},
        {"xor", "#15", {0x3f, 0xcf}},
        {"xor", "#0", {}},
        {"xor", "r3", {0x3d, 0xc3}},
        {"xor", "r1", {0x3d, 0xc1}},
        {"xor", "r0", {}},
        {"mult", "#3", {0x3e, 0x83}},
        {"mult", "#1", {0x3e, 0x81}},
        {"mult", "#15", {0x3e, 0x8f}},
        {"mult", "#0", {0x3e, 0x80}},
        {"mult", "r3", {0x83}},
        {"mult", "r1", {0x81}},
        {"mult", "r0", {0x80}},
        {"umult", "#3", {0x3f, 0x83}},
        {"umult", "#1", {0x3f, 0x81}},
        {"umult", "#15", {0x3f, 0x8f}},
        {"umult", "#0", {0x3f, 0x80}},
        {"umult", "r3", {0x3d, 0x83}},
        {"umult", "r1", {0x3d, 0x81}},
        {"umult", "r0", {0x3d, 0x80}},
    };
    for (const auto& entry : cases) {
        const std::string op = entry.mnemonic, operand = entry.operand;
        // `op x`, `op r0, x`, `op r0, r0, x`: R0 is the default selector, so no WITH.
        // `op r5, x`, `op r5, r5, x`: WITH R5 ($25), then the same operation.
        const std::vector<std::string> plain = {op + " " + operand, op + " r0, " + operand, op + " r0, r0, " + operand};
        const std::vector<std::string> with5 = {op + " r5, " + operand, op + " r5, r5, " + operand};
        if (entry.bytes.empty()) {
            for (const auto& statement : plain) require(rejects(statement), "discas must reject: " + statement);
            for (const auto& statement : with5) require(rejects(statement), "discas must reject: " + statement);
            continue;
        }
        auto prefixed = entry.bytes;
        prefixed.insert(prefixed.begin(), 0x25);
        for (const auto& statement : plain)
            require(assembleCode(statement) == entry.bytes, "wrong encoding for: " + statement);
        for (const auto& statement : with5)
            require(assembleCode(statement) == prefixed, "wrong encoding for: " + statement);
        // The exporter either names the operation or keeps explicit bytes;
        // either way the listing must reassemble to the same encoding.
        ObjectFile object;
        object.code_section = entry.bytes;
        object.code_section.push_back(1);
        check(object);
    }
    require(rejects("add r1, r2, #3"), "three-operand form needs matching destination and left operand");
    require(rejects("add #16") && rejects("and #16") && rejects("add #-1"), "immediates are limited to 0..15");
}

void check(const ObjectFile& object) {
    const auto reconstructed = Assembler().assemble(AssemblyGenerator(object).generate());
    if (object.code_section != reconstructed.code_section ||
        object.data_section != reconstructed.data_section || object.config != reconstructed.config ||
        object.data_alignment != reconstructed.data_alignment)
        throw std::runtime_error("Assembly round trip changed bytes or placement");
}
}
int main() {
    try {
        arithmeticEncodings();
        // Independent hand-encoded coverage of every raw opcode and ALT form.
        // These are encoding tests, not claims that every sequence is runnable.
        for (unsigned op = 0; op < 256; ++op) {
            for (const unsigned prefix : {0u, 0x3du, 0x3eu, 0x3fu}) {
                ObjectFile object;
                const std::size_t offset = prefix ? 1 : 0;
                object.code_section.resize(5 + offset, 0);
                if (prefix) object.code_section[0] = static_cast<std::uint8_t>(prefix);
                object.code_section[offset] = static_cast<std::uint8_t>(op);
                object.code_section.back() = 1;
                check(object);
            }
        }
        ObjectFile object;
        object.config.code_start_address = 0x706000;
        object.data_alignment = 2;
        object.code_section = {0xa1, 0, 0xf2, 0, 0, 0xff, 0, 0, 1, 0, 1};
        object.data_section = {0xff, 0x00, 0x80};
        object.symbol_table = {{"main", SymbolSection::CODE, 0},
            {std::string(1, '\x01') + "private.block", SymbolSection::CODE, 9},
            {"value", SymbolSection::DATA, 0}};
        object.relocation_table = {{"value", SymbolSection::CODE, 0, RelocationType::ADDR24_BANK},
            {"value", SymbolSection::CODE, 2, RelocationType::ADDR24_OFFSET},
            {std::string(1, '\x01') + "private.block", SymbolSection::CODE, 5, RelocationType::ADDR16_IWT}};
        check(object);
        const auto rebuilt = Assembler().assemble(AssemblyGenerator(object).generate());
        if (rebuilt.relocation_table.size() != 3 ||
            rebuilt.relocation_table[0].type != RelocationType::ADDR24_BANK ||
            rebuilt.relocation_table[1].type != RelocationType::ADDR24_OFFSET ||
            rebuilt.relocation_table[2].target_symbol_name.front() != '\x01')
            throw std::runtime_error("Assembly export lost far/private relocation semantics");
        // Short RAM: LMS/SMS with linker-written word indexes and the SHORTRAM
        // image survive export and reassembly, private and public names alike.
        ObjectFile shorted;
        shorted.code_section = {0x3d, 0xa5, 0x00, 0x3e, 0xa7, 0x00, 0x3d, 0xa1, 0x7f, 0x00, 0x01};
        shorted.short_ram_section = {0x00, 0x00, 0x34, 0x12};
        shorted.symbol_table = {{"main", SymbolSection::CODE, 0},
            {std::string(1, '\x01') + "slot", SymbolSection::SHORT_RAM, 0}, {"window", SymbolSection::SHORT_RAM, 2}};
        shorted.relocation_table = {{std::string(1, '\x01') + "slot", SymbolSection::CODE, 1, RelocationType::ADDR8_SHORT_RAM},
            {"window", SymbolSection::CODE, 4, RelocationType::ADDR8_SHORT_RAM}};
        check(shorted);
        const auto text = AssemblyGenerator(shorted).generate();
        const auto short_rebuilt = Assembler().assemble(text);
        if (text.find("sms (short(window)), r7") == std::string::npos || text.find("lms r1, ($0FE)") == std::string::npos ||
            short_rebuilt.short_ram_section != shorted.short_ram_section || short_rebuilt.relocation_table.size() != 2 ||
            short_rebuilt.relocation_table[0].type != RelocationType::ADDR8_SHORT_RAM ||
            short_rebuilt.relocation_table[0].patch_offset != 1 || short_rebuilt.relocation_table[1].patch_offset != 4 ||
            short_rebuilt.relocation_table[0].target_symbol_name.front() != '\x01')
            throw std::runtime_error("Assembly export lost short RAM accesses or their image");
        for (const auto& bytes : {std::vector<std::uint8_t>{0xf0}, {0xa0}, {0x05, 0x7f}}) {
            ObjectFile malformed;
            malformed.code_section = bytes;
            bool rejected = false;
            try { (void)AssemblyGenerator(malformed).generate(); }
            catch (const std::runtime_error&) { rejected = true; }
            if (!rejected) throw std::runtime_error("Malformed assembly export input was accepted");
        }
        std::cout << "Assembly export round trips passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
