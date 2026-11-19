#include "Assembler.hpp"
#include "AssemblyGenerator.hpp"
#include "CompilerError.hpp"
#include "ObjectFile.hpp"
#include "SPC700Isa.hpp"
#include "TestTempDirectory.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string hexBytes(const Bytes& bytes) {
    static const char* digits = "0123456789ABCDEF";
    std::string text;
    for (const auto byte : bytes) {
        if (!text.empty()) text += ' ';
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

struct GoldenInstruction {
    const char* text; // Canonical source; branch operands target label `t` at the instruction.
    Bytes bytes;
};

// Independent reference: every row was assembled once with WLA SPC700 Macro
// Assembler v10.7 (`t: <text>` at the start of a section). Regenerate only
// from that external oracle, never from discas output.
const std::vector<GoldenInstruction>& golden() {
    static const std::vector<GoldenInstruction> rows = {
    {"nop", {0x00}},
    {"tcall 0", {0x01}},
    {"set1 $12.0", {0x02, 0x12}},
    {"bbs $12.0,t", {0x03, 0x12, 0xFD}},
    {"or a,$12", {0x04, 0x12}},
    {"or a,!$1234", {0x05, 0x34, 0x12}},
    {"or a,(x)", {0x06}},
    {"or a,[$12+x]", {0x07, 0x12}},
    {"or a,#$5A", {0x08, 0x5A}},
    {"or $34,$12", {0x09, 0x12, 0x34}},
    {"or1 c,$1234.5", {0x0A, 0x34, 0xB2}},
    {"asl $12", {0x0B, 0x12}},
    {"asl !$1234", {0x0C, 0x34, 0x12}},
    {"push psw", {0x0D}},
    {"tset1 !$1234", {0x0E, 0x34, 0x12}},
    {"brk", {0x0F}},
    {"bpl t", {0x10, 0xFE}},
    {"tcall 1", {0x11}},
    {"clr1 $12.0", {0x12, 0x12}},
    {"bbc $12.0,t", {0x13, 0x12, 0xFD}},
    {"or a,$12+x", {0x14, 0x12}},
    {"or a,!$1234+x", {0x15, 0x34, 0x12}},
    {"or a,!$1234+y", {0x16, 0x34, 0x12}},
    {"or a,[$12]+y", {0x17, 0x12}},
    {"or $12,#$5A", {0x18, 0x5A, 0x12}},
    {"or (x),(y)", {0x19}},
    {"decw $12", {0x1A, 0x12}},
    {"asl $12+x", {0x1B, 0x12}},
    {"asl a", {0x1C}},
    {"dec x", {0x1D}},
    {"cmp x,!$1234", {0x1E, 0x34, 0x12}},
    {"jmp [!$1234+x]", {0x1F, 0x34, 0x12}},
    {"clrp", {0x20}},
    {"tcall 2", {0x21}},
    {"set1 $12.1", {0x22, 0x12}},
    {"bbs $12.1,t", {0x23, 0x12, 0xFD}},
    {"and a,$12", {0x24, 0x12}},
    {"and a,!$1234", {0x25, 0x34, 0x12}},
    {"and a,(x)", {0x26}},
    {"and a,[$12+x]", {0x27, 0x12}},
    {"and a,#$5A", {0x28, 0x5A}},
    {"and $34,$12", {0x29, 0x12, 0x34}},
    {"or1 c,/$1234.5", {0x2A, 0x34, 0xB2}},
    {"rol $12", {0x2B, 0x12}},
    {"rol !$1234", {0x2C, 0x34, 0x12}},
    {"push a", {0x2D}},
    {"cbne $12,t", {0x2E, 0x12, 0xFD}},
    {"bra t", {0x2F, 0xFE}},
    {"bmi t", {0x30, 0xFE}},
    {"tcall 3", {0x31}},
    {"clr1 $12.1", {0x32, 0x12}},
    {"bbc $12.1,t", {0x33, 0x12, 0xFD}},
    {"and a,$12+x", {0x34, 0x12}},
    {"and a,!$1234+x", {0x35, 0x34, 0x12}},
    {"and a,!$1234+y", {0x36, 0x34, 0x12}},
    {"and a,[$12]+y", {0x37, 0x12}},
    {"and $12,#$5A", {0x38, 0x5A, 0x12}},
    {"and (x),(y)", {0x39}},
    {"incw $12", {0x3A, 0x12}},
    {"rol $12+x", {0x3B, 0x12}},
    {"rol a", {0x3C}},
    {"inc x", {0x3D}},
    {"cmp x,$12", {0x3E, 0x12}},
    {"call !$1234", {0x3F, 0x34, 0x12}},
    {"setp", {0x40}},
    {"tcall 4", {0x41}},
    {"set1 $12.2", {0x42, 0x12}},
    {"bbs $12.2,t", {0x43, 0x12, 0xFD}},
    {"eor a,$12", {0x44, 0x12}},
    {"eor a,!$1234", {0x45, 0x34, 0x12}},
    {"eor a,(x)", {0x46}},
    {"eor a,[$12+x]", {0x47, 0x12}},
    {"eor a,#$5A", {0x48, 0x5A}},
    {"eor $34,$12", {0x49, 0x12, 0x34}},
    {"and1 c,$1234.5", {0x4A, 0x34, 0xB2}},
    {"lsr $12", {0x4B, 0x12}},
    {"lsr !$1234", {0x4C, 0x34, 0x12}},
    {"push x", {0x4D}},
    {"tclr1 !$1234", {0x4E, 0x34, 0x12}},
    {"pcall $34", {0x4F, 0x34}},
    {"bvc t", {0x50, 0xFE}},
    {"tcall 5", {0x51}},
    {"clr1 $12.2", {0x52, 0x12}},
    {"bbc $12.2,t", {0x53, 0x12, 0xFD}},
    {"eor a,$12+x", {0x54, 0x12}},
    {"eor a,!$1234+x", {0x55, 0x34, 0x12}},
    {"eor a,!$1234+y", {0x56, 0x34, 0x12}},
    {"eor a,[$12]+y", {0x57, 0x12}},
    {"eor $12,#$5A", {0x58, 0x5A, 0x12}},
    {"eor (x),(y)", {0x59}},
    {"cmpw ya,$12", {0x5A, 0x12}},
    {"lsr $12+x", {0x5B, 0x12}},
    {"lsr a", {0x5C}},
    {"mov x,a", {0x5D}},
    {"cmp y,!$1234", {0x5E, 0x34, 0x12}},
    {"jmp !$1234", {0x5F, 0x34, 0x12}},
    {"clrc", {0x60}},
    {"tcall 6", {0x61}},
    {"set1 $12.3", {0x62, 0x12}},
    {"bbs $12.3,t", {0x63, 0x12, 0xFD}},
    {"cmp a,$12", {0x64, 0x12}},
    {"cmp a,!$1234", {0x65, 0x34, 0x12}},
    {"cmp a,(x)", {0x66}},
    {"cmp a,[$12+x]", {0x67, 0x12}},
    {"cmp a,#$5A", {0x68, 0x5A}},
    {"cmp $34,$12", {0x69, 0x12, 0x34}},
    {"and1 c,/$1234.5", {0x6A, 0x34, 0xB2}},
    {"ror $12", {0x6B, 0x12}},
    {"ror !$1234", {0x6C, 0x34, 0x12}},
    {"push y", {0x6D}},
    {"dbnz $12,t", {0x6E, 0x12, 0xFD}},
    {"ret", {0x6F}},
    {"bvs t", {0x70, 0xFE}},
    {"tcall 7", {0x71}},
    {"clr1 $12.3", {0x72, 0x12}},
    {"bbc $12.3,t", {0x73, 0x12, 0xFD}},
    {"cmp a,$12+x", {0x74, 0x12}},
    {"cmp a,!$1234+x", {0x75, 0x34, 0x12}},
    {"cmp a,!$1234+y", {0x76, 0x34, 0x12}},
    {"cmp a,[$12]+y", {0x77, 0x12}},
    {"cmp $12,#$5A", {0x78, 0x5A, 0x12}},
    {"cmp (x),(y)", {0x79}},
    {"addw ya,$12", {0x7A, 0x12}},
    {"ror $12+x", {0x7B, 0x12}},
    {"ror a", {0x7C}},
    {"mov a,x", {0x7D}},
    {"cmp y,$12", {0x7E, 0x12}},
    {"reti", {0x7F}},
    {"setc", {0x80}},
    {"tcall 8", {0x81}},
    {"set1 $12.4", {0x82, 0x12}},
    {"bbs $12.4,t", {0x83, 0x12, 0xFD}},
    {"adc a,$12", {0x84, 0x12}},
    {"adc a,!$1234", {0x85, 0x34, 0x12}},
    {"adc a,(x)", {0x86}},
    {"adc a,[$12+x]", {0x87, 0x12}},
    {"adc a,#$5A", {0x88, 0x5A}},
    {"adc $34,$12", {0x89, 0x12, 0x34}},
    {"eor1 c,$1234.5", {0x8A, 0x34, 0xB2}},
    {"dec $12", {0x8B, 0x12}},
    {"dec !$1234", {0x8C, 0x34, 0x12}},
    {"mov y,#$5A", {0x8D, 0x5A}},
    {"pop psw", {0x8E}},
    {"mov $12,#$5A", {0x8F, 0x5A, 0x12}},
    {"bcc t", {0x90, 0xFE}},
    {"tcall 9", {0x91}},
    {"clr1 $12.4", {0x92, 0x12}},
    {"bbc $12.4,t", {0x93, 0x12, 0xFD}},
    {"adc a,$12+x", {0x94, 0x12}},
    {"adc a,!$1234+x", {0x95, 0x34, 0x12}},
    {"adc a,!$1234+y", {0x96, 0x34, 0x12}},
    {"adc a,[$12]+y", {0x97, 0x12}},
    {"adc $12,#$5A", {0x98, 0x5A, 0x12}},
    {"adc (x),(y)", {0x99}},
    {"subw ya,$12", {0x9A, 0x12}},
    {"dec $12+x", {0x9B, 0x12}},
    {"dec a", {0x9C}},
    {"mov x,sp", {0x9D}},
    {"div ya,x", {0x9E}},
    {"xcn a", {0x9F}},
    {"ei", {0xA0}},
    {"tcall 10", {0xA1}},
    {"set1 $12.5", {0xA2, 0x12}},
    {"bbs $12.5,t", {0xA3, 0x12, 0xFD}},
    {"sbc a,$12", {0xA4, 0x12}},
    {"sbc a,!$1234", {0xA5, 0x34, 0x12}},
    {"sbc a,(x)", {0xA6}},
    {"sbc a,[$12+x]", {0xA7, 0x12}},
    {"sbc a,#$5A", {0xA8, 0x5A}},
    {"sbc $34,$12", {0xA9, 0x12, 0x34}},
    {"mov1 c,$1234.5", {0xAA, 0x34, 0xB2}},
    {"inc $12", {0xAB, 0x12}},
    {"inc !$1234", {0xAC, 0x34, 0x12}},
    {"cmp y,#$5A", {0xAD, 0x5A}},
    {"pop a", {0xAE}},
    {"mov (x)+,a", {0xAF}},
    {"bcs t", {0xB0, 0xFE}},
    {"tcall 11", {0xB1}},
    {"clr1 $12.5", {0xB2, 0x12}},
    {"bbc $12.5,t", {0xB3, 0x12, 0xFD}},
    {"sbc a,$12+x", {0xB4, 0x12}},
    {"sbc a,!$1234+x", {0xB5, 0x34, 0x12}},
    {"sbc a,!$1234+y", {0xB6, 0x34, 0x12}},
    {"sbc a,[$12]+y", {0xB7, 0x12}},
    {"sbc $12,#$5A", {0xB8, 0x5A, 0x12}},
    {"sbc (x),(y)", {0xB9}},
    {"movw ya,$12", {0xBA, 0x12}},
    {"inc $12+x", {0xBB, 0x12}},
    {"inc a", {0xBC}},
    {"mov sp,x", {0xBD}},
    {"das a", {0xBE}},
    {"mov a,(x)+", {0xBF}},
    {"di", {0xC0}},
    {"tcall 12", {0xC1}},
    {"set1 $12.6", {0xC2, 0x12}},
    {"bbs $12.6,t", {0xC3, 0x12, 0xFD}},
    {"mov $12,a", {0xC4, 0x12}},
    {"mov !$1234,a", {0xC5, 0x34, 0x12}},
    {"mov (x),a", {0xC6}},
    {"mov [$12+x],a", {0xC7, 0x12}},
    {"cmp x,#$5A", {0xC8, 0x5A}},
    {"mov !$1234,x", {0xC9, 0x34, 0x12}},
    {"mov1 $1234.5,c", {0xCA, 0x34, 0xB2}},
    {"mov $12,y", {0xCB, 0x12}},
    {"mov !$1234,y", {0xCC, 0x34, 0x12}},
    {"mov x,#$5A", {0xCD, 0x5A}},
    {"pop x", {0xCE}},
    {"mul ya", {0xCF}},
    {"bne t", {0xD0, 0xFE}},
    {"tcall 13", {0xD1}},
    {"clr1 $12.6", {0xD2, 0x12}},
    {"bbc $12.6,t", {0xD3, 0x12, 0xFD}},
    {"mov $12+x,a", {0xD4, 0x12}},
    {"mov !$1234+x,a", {0xD5, 0x34, 0x12}},
    {"mov !$1234+y,a", {0xD6, 0x34, 0x12}},
    {"mov [$12]+y,a", {0xD7, 0x12}},
    {"mov $12,x", {0xD8, 0x12}},
    {"mov $12+y,x", {0xD9, 0x12}},
    {"movw $12,ya", {0xDA, 0x12}},
    {"mov $12+x,y", {0xDB, 0x12}},
    {"dec y", {0xDC}},
    {"mov a,y", {0xDD}},
    {"cbne $12+x,t", {0xDE, 0x12, 0xFD}},
    {"daa a", {0xDF}},
    {"clrv", {0xE0}},
    {"tcall 14", {0xE1}},
    {"set1 $12.7", {0xE2, 0x12}},
    {"bbs $12.7,t", {0xE3, 0x12, 0xFD}},
    {"mov a,$12", {0xE4, 0x12}},
    {"mov a,!$1234", {0xE5, 0x34, 0x12}},
    {"mov a,(x)", {0xE6}},
    {"mov a,[$12+x]", {0xE7, 0x12}},
    {"mov a,#$5A", {0xE8, 0x5A}},
    {"mov x,!$1234", {0xE9, 0x34, 0x12}},
    {"not1 $1234.5", {0xEA, 0x34, 0xB2}},
    {"mov y,$12", {0xEB, 0x12}},
    {"mov y,!$1234", {0xEC, 0x34, 0x12}},
    {"notc", {0xED}},
    {"pop y", {0xEE}},
    {"sleep", {0xEF}},
    {"beq t", {0xF0, 0xFE}},
    {"tcall 15", {0xF1}},
    {"clr1 $12.7", {0xF2, 0x12}},
    {"bbc $12.7,t", {0xF3, 0x12, 0xFD}},
    {"mov a,$12+x", {0xF4, 0x12}},
    {"mov a,!$1234+x", {0xF5, 0x34, 0x12}},
    {"mov a,!$1234+y", {0xF6, 0x34, 0x12}},
    {"mov a,[$12]+y", {0xF7, 0x12}},
    {"mov x,$12", {0xF8, 0x12}},
    {"mov x,$12+y", {0xF9, 0x12}},
    {"mov $34,$12", {0xFA, 0x12, 0x34}},
    {"mov y,$12+x", {0xFB, 0x12}},
    {"inc y", {0xFC}},
    {"mov y,a", {0xFD}},
    {"dbnz y,t", {0xFE, 0xFE}},
    {"stop", {0xFF}},
    };
    return rows;
}

ObjectFile assembleSpc(const std::string& body) {
    return Assembler(TargetKind::SPC700).assemble(".setcpu \"SPC700\"\n.segment \"CODE\"\n" + body);
}

void expectFailure(TargetKind target, const std::string& source, const std::string& expected) {
    try {
        Assembler(target).assemble(source);
    } catch (const CompilerError& error) {
        require(error.getMessage().find(expected) != std::string::npos,
            "unexpected diagnostic for:\n" + source + "\nexpected '" + expected + "', got '" + error.getMessage() + "'");
        return;
    }
    throw std::runtime_error("assembly unexpectedly succeeded:\n" + source);
}

void expectSpcFailure(const std::string& body, const std::string& expected) {
    expectFailure(TargetKind::SPC700, ".segment \"CODE\"\n" + body, expected);
}

std::string withLabel(const std::string& text, const std::string& label) {
    // Golden branch rows end in the label operand `t`.
    if (text.size() >= 2 && text.compare(text.size() - 2, 2, ",t") == 0) return text.substr(0, text.size() - 1) + label;
    if (text.size() >= 2 && text.compare(text.size() - 2, 2, " t") == 0) return text.substr(0, text.size() - 1) + label;
    return text;
}

void opcodeTableIsComplete() {
    std::set<std::tuple<std::string, int, int, int>> forms;
    for (int opcode = 0; opcode < 256; ++opcode) {
        const auto& info = SPC700Isa::instruction(static_cast<std::uint8_t>(opcode));
        require(info.opcode == opcode, "opcode table is not indexed by opcode");
        const auto size = 1 + SPC700Isa::operandBytes(info.operands[0]) + SPC700Isa::operandBytes(info.operands[1]);
        require(size == info.size, "opcode table size mismatch for opcode " + std::to_string(opcode));
        require(forms.insert(std::make_tuple(std::string(info.mnemonic), static_cast<int>(info.operands[0]),
            static_cast<int>(info.operands[1]), static_cast<int>(info.fixed))).second,
            "two opcodes share one source form: " + std::string(info.mnemonic));
    }
}

void goldenEncodings() {
    require(golden().size() == 256, "golden table must cover every opcode");
    std::set<std::uint8_t> opcodes;
    for (const auto& row : golden()) {
        const auto object = assembleSpc(std::string("t: ") + row.text + "\n");
        require(object.code_section == row.bytes, std::string("encoding mismatch for '") + row.text + "': got " +
            hexBytes(object.code_section) + ", expected " + hexBytes(row.bytes));
        require(object.relocation_table.empty(), std::string("numeric operand produced a relocation: ") + row.text);
        opcodes.insert(row.bytes.front());
    }
    require(opcodes.size() == 256, "golden table must contain each opcode once");
}

void exportRoundTrip() {
    std::string body;
    for (std::size_t index = 0; index < golden().size(); ++index) {
        const auto label = "t" + std::to_string(index);
        body += label + ": " + withLabel(golden()[index].text, label) + "\n";
    }
    const auto first = assembleSpc(body);
    const auto listing = AssemblyGenerator(first).generate();
    const auto second = Assembler(Assembler::detectTarget(listing, TargetKind::GSU)).assemble(listing);
    require(second.config.target == TargetKind::SPC700, "exported listing must select SPC700");
    require(second.code_section == first.code_section, "SPC700 assembly export did not round-trip");
    require(AssemblyGenerator(second).generate() == listing, "SPC700 assembly export is not a fixed point");
}

const RelocationEntry& relocationAt(const ObjectFile& object, std::uint32_t offset) {
    for (const auto& relocation : object.relocation_table)
        if (relocation.patch_offset == offset) return relocation;
    throw std::runtime_error("missing relocation at offset " + std::to_string(offset));
}

void symbolicOperands() {
    const auto object = assembleSpc(
        ".export entry\n"
        "entry:\n"
        "    mov a,ext_dp\n"        // 0: E4 dp           DP8 @1
        "    mov a,!ext_abs\n"      // 2: E5 lo hi        ABS16 @3
        "    mov1 c,ext_bit.5\n"    // 5: AA lo hi|bit    MEMBIT13 @6
        "    mov a,#<ext_abs\n"     // 8: E8 lo           IMM8_LO @9
        "    mov y,#>ext_abs\n"     // 10: 8D hi          IMM8_HI @11
        "    mov ext_dp,#$12\n"     // 12: 8F 12 dp       DP8 @14 (operands reversed)
        "    mov ext_dp,other\n"    // 15: FA src dst     DP8 @16 (other), @17 (ext_dp)
        "    call !helper\n"        // 18: 3F lo hi       ABS16 @19 to private helper
        "    bbs ext_dp.3,entry\n"  // 21: 63 dp rel      DP8 @22
        "helper:\n"
        "    ret\n");               // 24
    require(object.code_section == Bytes({0xE4, 0x00, 0xE5, 0x00, 0x00, 0xAA, 0x00, 0xA0, 0xE8, 0x00, 0x8D, 0x00,
        0x8F, 0x12, 0x00, 0xFA, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x63, 0x00, 0xE8, 0x6F}),
        "symbolic operand placeholders: " + hexBytes(object.code_section));
    const std::vector<std::tuple<std::uint32_t, RelocationType, std::string>> expected = {
        {1, RelocationType::SPC_DP8, "ext_dp"}, {3, RelocationType::SPC_ABS16, "ext_abs"},
        {6, RelocationType::SPC_MEMBIT13, "ext_bit"}, {9, RelocationType::SPC_IMM8_LO, "ext_abs"},
        {11, RelocationType::SPC_IMM8_HI, "ext_abs"}, {14, RelocationType::SPC_DP8, "ext_dp"},
        {16, RelocationType::SPC_DP8, "other"}, {17, RelocationType::SPC_DP8, "ext_dp"},
        {19, RelocationType::SPC_ABS16, "\x01helper"}, {22, RelocationType::SPC_DP8, "ext_dp"}};
    require(object.relocation_table.size() == expected.size(), "unexpected relocation count");
    for (const auto& entry : expected) {
        const auto& relocation = relocationAt(object, std::get<0>(entry));
        require(relocation.type == std::get<1>(entry) && relocation.target_symbol_name == std::get<2>(entry) &&
            relocation.section_to_patch == SymbolSection::CODE,
            "relocation mismatch at offset " + std::to_string(std::get<0>(entry)));
    }
    // The symbolic listing must also round-trip, relocations included.
    const auto listing = AssemblyGenerator(object).generate();
    const auto again = Assembler(TargetKind::SPC700).assemble(listing);
    require(again.code_section == object.code_section && again.relocation_table.size() == object.relocation_table.size(),
        "symbolic SPC700 listing did not round-trip");
}

std::string fill(std::size_t count) {
    std::string text;
    for (std::size_t index = 0; index < count; ++index) text += "    nop\n";
    return text;
}

void branchRanges() {
    // Forward: distance = target - (branch + size).
    auto near = assembleSpc("    bne far\n" + fill(127) + "far:\n    nop\n");
    require(Bytes(near.code_section.begin(), near.code_section.begin() + 2) == Bytes({0xD0, 0x7F}), "+127 must stay short");
    require(near.relocation_table.empty(), "short branch must not relocate");
    auto relaxed = assembleSpc("    bne far\n" + fill(128) + "far:\n    nop\n");
    require(Bytes(relaxed.code_section.begin(), relaxed.code_section.begin() + 5) == Bytes({0xF0, 0x03, 0x5F, 0x00, 0x00}),
        "+128 must relax BNE to BEQ +3; JMP: " + hexBytes(Bytes(relaxed.code_section.begin(), relaxed.code_section.begin() + 5)));
    require(relaxed.relocation_table.size() == 1 && relaxed.relocation_table[0].patch_offset == 3 &&
        relaxed.relocation_table[0].type == RelocationType::SPC_ABS16 &&
        relaxed.relocation_table[0].target_symbol_name == "\x01" "far", "relaxed branch relocation");

    // Backward: -128 is the last short distance.
    auto back = assembleSpc("far:\n" + fill(126) + "    bra far\n");
    require(Bytes(back.code_section.end() - 2, back.code_section.end()) == Bytes({0x2F, 0x80}), "-128 must stay short");
    auto back_far = assembleSpc("far:\n" + fill(127) + "    bra far\n");
    require(Bytes(back_far.code_section.end() - 3, back_far.code_section.end()) == Bytes({0x5F, 0x00, 0x00}),
        "-129 must relax BRA to JMP");

    // Every long form, measured from a target beyond the short range.
    const std::vector<std::pair<std::string, Bytes>> forms = {
        {"bra far", {0x5F, 0x00, 0x00}},
        {"bcs far", {0x90, 0x03, 0x5F, 0x00, 0x00}},
        {"bbs $12.6,far", {0xD3, 0x12, 0x03, 0x5F, 0x00, 0x00}},
        {"bbc $12.1,far", {0x23, 0x12, 0x03, 0x5F, 0x00, 0x00}},
        {"cbne $34,far", {0x2E, 0x34, 0x02, 0x2F, 0x03, 0x5F, 0x00, 0x00}},
        {"cbne $34+x,far", {0xDE, 0x34, 0x02, 0x2F, 0x03, 0x5F, 0x00, 0x00}},
        {"dbnz $34,far", {0x6E, 0x34, 0x02, 0x2F, 0x03, 0x5F, 0x00, 0x00}},
        {"dbnz y,far", {0xFE, 0x02, 0x2F, 0x03, 0x5F, 0x00, 0x00}}};
    for (const auto& form : forms) {
        const auto object = assembleSpc("    " + form.first + "\n" + fill(200) + "far:\n    nop\n");
        const Bytes head(object.code_section.begin(), object.code_section.begin() + static_cast<std::ptrdiff_t>(form.second.size()));
        require(head == form.second, "long form of '" + form.first + "': " + hexBytes(head));
        require(object.relocation_table.size() == 1 &&
            object.relocation_table[0].patch_offset == form.second.size() - 2, "long form relocation of '" + form.first + "'");
        // The long form must also survive an export/assemble round trip.
        const auto again = Assembler(TargetKind::SPC700).assemble(AssemblyGenerator(object).generate());
        require(again.code_section == object.code_section, "long form round trip of '" + form.first + "'");
    }

    // Relaxing the first branch pushes the second out of range: the layout
    // fixpoint must relax both.
    const auto chain = assembleSpc("    beq far\n    beq other\n" + fill(123) + "far:\n    nop\n" + fill(200) +
        "other:\n    nop\n");
    require(chain.code_section[0] == 0xD0 && chain.code_section[5] == 0xD0, "chained relaxation did not converge");
}

void diagnostics() {
    expectSpcFailure("    mov a,$123\n", "exceeds $FF");
    expectSpcFailure("    mov a,!$10000\n", "exceeds $FFFF");
    expectSpcFailure("    set1 $12.8\n", "bit index must be 0..7");
    expectSpcFailure("    set1 !$12.1\n", "unmarked address");
    expectSpcFailure("    or1 c,$2000.1\n", "$0000-$1FFF");
    expectSpcFailure("    tcall 16\n", "TCALL expects");
    expectSpcFailure("    pcall $100\n", "PCALL expects");
    expectSpcFailure("    mov a,#$100\n", "outside 0..255");
    expectSpcFailure("    mov a,#value\n", "use #<value");
    expectSpcFailure("    mov a,#<$12\n", "select bytes of a symbol");
    expectSpcFailure("    plot\n", "unknown instruction or directive 'plot'");
    expectSpcFailure("    mov x,y\n", "invalid operands for 'mov'");
    expectSpcFailure("    mov a,[$12]\n", "invalid indirect operand");
    expectSpcFailure("    mov a,/$12\n", "only negates a mem.bit");
    expectSpcFailure("    bra $10\n", "branch target must be a label");
    expectSpcFailure("    bra missing\n", "undefined branch label 'missing'");
    expectSpcFailure("here:\n    nop\n.segment \"DATA\"\n    bra here\n", "branches are only valid");
    expectFailure(TargetKind::SPC700, ".segment \"SHORTRAM\"\n", "unsupported segment");
    expectFailure(TargetKind::SPC700, ".define __DISCO_MEMORY_MAPPING lorom\n", "for SPC700");
    expectFailure(TargetKind::SPC700, ".define __DISCO_CODE_START_ADDRESS $10000\n", "fit in 16 bits");
    expectFailure(TargetKind::SPC700, ".setcpu \"GSU\"\n", "does not match the selected SPC700 target");
    expectFailure(TargetKind::GSU, ".setcpu \"SPC700\"\n", "does not match the selected GSU target");
    expectFailure(TargetKind::SPC700, ".segment \"CODE\"\nstart:\n.setcpu \"SPC700\"\n", "must precede");
    // Each instruction set rejects the other's mnemonics.
    expectFailure(TargetKind::GSU, ".segment \"CODE\"\n    mov a,#$12\n", "unknown instruction or directive 'mov'");
    expectSpcFailure("    iwt r1, #$1234\n", "unknown instruction or directive 'iwt'");

    require(Assembler::detectTarget("; comment\n.setcpu \"SPC700\"\n", TargetKind::GSU) == TargetKind::SPC700, "detect SPC700");
    require(Assembler::detectTarget(".setcpu \"GSU\"\n", TargetKind::SPC700) == TargetKind::GSU, "detect GSU");
    require(Assembler::detectTarget("nop\n", TargetKind::SPC700) == TargetKind::SPC700, "detect fallback");
}

Bytes readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void expectObjectRejected(ObjectFile object, const std::filesystem::path& path, const std::string& expected) {
    try {
        object.write(path.string());
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(expected) != std::string::npos,
            "unexpected object diagnostic: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("object write unexpectedly succeeded: " + expected);
}

void expectBytesRejected(const Bytes& bytes, const std::string& expected) {
    try {
        ObjectFile::readBytes(bytes);
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(expected) != std::string::npos,
            "unexpected object read diagnostic: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("object read unexpectedly succeeded: " + expected);
}

void objectFormat() {
    const TestTempDirectory directory("spc700-object");
    const auto object = assembleSpc(".export entry\nentry:\n    jmp !entry\n");
    const auto path = directory.path() / "spc.o";
    auto writable = object;
    writable.write(path.string());
    const auto bytes = readFile(path);
    require(bytes.size() > 7 && bytes[5] == ObjectFile::SPC700RelocationFormatVersion && bytes[6] == 1,
        "SPC700 relocations must select object format 11");
    const auto loaded = ObjectFile::read(path.string());
    require(loaded.code_section == object.code_section && loaded.relocation_table.size() == 1 &&
        loaded.relocation_table[0].type == RelocationType::SPC_ABS16 && loaded.relocation_table[0].patch_offset == 1,
        "SPC700 object did not round-trip");

    // The relocation type is the final byte of the single relocation record.
    auto version10 = bytes;
    version10[5] = 10;
    expectBytesRejected(version10, "invalid relocation type");
    auto gsu_type = bytes;
    gsu_type.back() = static_cast<std::uint8_t>(RelocationType::ADDR16_IWT);
    expectBytesRejected(gsu_type, "does not match the object target");

    auto in_gsu = object;
    in_gsu.config.target = TargetKind::GSU;
    expectObjectRejected(in_gsu, directory.path() / "gsu.o", "does not match the object target");
    auto gsu_in_spc = object;
    gsu_in_spc.relocation_table[0].type = RelocationType::ADDR16_IWT;
    gsu_in_spc.relocation_table[0].patch_offset = 0;
    expectObjectRejected(gsu_in_spc, directory.path() / "mixed.o", "does not match the object target");
    auto out_of_section = object;
    out_of_section.relocation_table[0].patch_offset = 2;
    expectObjectRejected(out_of_section, directory.path() / "range.o", "extends beyond its section");

    // Without relocations an SPC700 object keeps the baseline format.
    auto plain = assembleSpc("    nop\n");
    const auto plain_path = directory.path() / "plain.o";
    plain.write(plain_path.string());
    require(readFile(plain_path).at(5) == ObjectFile::BaselineFormatVersion, "plain SPC700 object version");
}

int printCorpus() {
    for (const auto& row : golden()) std::cout << row.text << '\n';
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--print-corpus") return printCorpus();
        if (argc != 1) throw std::runtime_error("Usage: disco_spc700_assembler_tests [--print-corpus]");
        opcodeTableIsComplete();
        goldenEncodings();
        exportRoundTrip();
        symbolicOperands();
        branchRanges();
        diagnostics();
        objectFormat();
    } catch (const std::exception& error) {
        std::cerr << "SPC700 assembler test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
