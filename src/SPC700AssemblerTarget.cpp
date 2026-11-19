#include "SPC700AssemblerTarget.hpp"

#include "SPC700Isa.hpp"

#include <cstdint>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace AsmSupport {
namespace {

using SPC700Isa::InstructionInfo;
using SPC700Isa::OperandLayout;
using SPC700Isa::OperandPattern;

// Syntactic shape of one operand, independent of the instruction. Pattern
// matching against the opcode map happens afterwards, so every encoding is
// chosen from the table rather than from ad-hoc string tests.
enum class Shape {
    Register,    // a, x, y, ya, sp, psw, c
    IndX,        // (x)
    IndXInc,     // (x)+
    IndY,        // (y)
    Immediate,   // #value, #<sym, #>sym
    Address,     // expr or !expr
    IndexedX,    // expr+x or !expr+x
    IndexedY,    // expr+y or !expr+y
    BracketX,    // [expr+x] or [!expr+x]
    BracketIndY, // [expr]+y
    Bit          // expr.bit or /expr.bit
};

enum class ImmediatePart { Value, Low, High };

struct Operand {
    Shape shape = Shape::Address;
    OperandPattern reg = OperandPattern::None; // Register shapes only.
    bool absolute = false;                     // Written with `!`.
    bool inverted = false;                     // `/mem.bit`.
    bool symbolic = false;
    std::int64_t value = 0;
    std::string symbol;
    int bit = -1;
    ImmediatePart part = ImmediatePart::Value;
    std::string text;                          // Original spelling for diagnostics.
};

std::string compact(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (const char c : text)
        if (c != ' ' && c != '\t') result.push_back(c);
    return result;
}

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Parses a numeric literal or a symbol name. A leading `!` has already been
// removed by the caller.
void parseExpression(const std::string& text, Operand& operand, int line) {
    if (text.empty()) fail("missing address in operand '" + operand.text + "'.", line);
    std::int64_t value = 0;
    if (parseNumber(text, value)) {
        operand.value = value;
        return;
    }
    if (text.front() == '#' || !isIdentifier(text)) fail("invalid operand '" + operand.text + "'.", line);
    operand.symbolic = true;
    operand.symbol = text;
}

void parseAddress(std::string text, Operand& operand, int line) {
    if (!text.empty() && text.front() == '!') {
        operand.absolute = true;
        text.erase(text.begin());
    }
    parseExpression(text, operand, line);
}

OperandPattern registerPattern(const std::string& lowered) {
    if (lowered == "a") return OperandPattern::A;
    if (lowered == "x") return OperandPattern::X;
    if (lowered == "y") return OperandPattern::Y;
    if (lowered == "ya") return OperandPattern::YA;
    if (lowered == "sp") return OperandPattern::SP;
    if (lowered == "psw") return OperandPattern::PSW;
    if (lowered == "c") return OperandPattern::C;
    return OperandPattern::None;
}

Operand parseOperand(const std::string& source, int line) {
    Operand operand;
    operand.text = trim(source);
    const auto text = compact(operand.text);
    const auto lowered = lower(text);
    if (text.empty()) fail("empty operand.", line);

    const auto reg = registerPattern(lowered);
    if (reg != OperandPattern::None) {
        operand.shape = Shape::Register;
        operand.reg = reg;
        return operand;
    }
    if (lowered == "(x)") { operand.shape = Shape::IndX; return operand; }
    if (lowered == "(x)+") { operand.shape = Shape::IndXInc; return operand; }
    if (lowered == "(y)") { operand.shape = Shape::IndY; return operand; }

    if (text.front() == '#') {
        operand.shape = Shape::Immediate;
        auto rest = text.substr(1);
        if (!rest.empty() && (rest.front() == '<' || rest.front() == '>')) {
            operand.part = rest.front() == '<' ? ImmediatePart::Low : ImmediatePart::High;
            rest.erase(rest.begin());
            parseExpression(rest, operand, line);
            if (!operand.symbolic) fail("'#<' and '#>' select bytes of a symbol address, got '" + operand.text + "'.", line);
            return operand;
        }
        parseExpression(rest, operand, line);
        if (operand.symbolic)
            fail("8-bit immediate cannot hold the symbol '" + operand.symbol + "'; use #<" + operand.symbol +
                 " or #>" + operand.symbol + ".", line);
        return operand;
    }

    if (text.front() == '[') {
        if (endsWith(lowered, "+x]")) {
            operand.shape = Shape::BracketX;
            parseAddress(text.substr(1, text.size() - 4), operand, line);
            return operand;
        }
        if (endsWith(lowered, "]+y")) {
            operand.shape = Shape::BracketIndY;
            parseAddress(text.substr(1, text.size() - 4), operand, line);
            return operand;
        }
        fail("invalid indirect operand '" + operand.text + "'.", line);
    }

    if (endsWith(lowered, "+x") || endsWith(lowered, "+y")) {
        operand.shape = lowered.back() == 'x' ? Shape::IndexedX : Shape::IndexedY;
        parseAddress(text.substr(0, text.size() - 2), operand, line);
        return operand;
    }

    // A trailing `.digits` always selects a bit; symbol names that end in
    // `.digits` cannot be used as plain addresses in SPC700 operands.
    auto body = text;
    if (!body.empty() && body.front() == '/') {
        operand.inverted = true;
        body.erase(body.begin());
    }
    const auto dot = body.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && dot + 1 < body.size() &&
        body.find_first_not_of("0123456789", dot + 1) == std::string::npos) {
        operand.shape = Shape::Bit;
        const auto bit_text = body.substr(dot + 1);
        if (bit_text.size() != 1 || bit_text[0] > '7') fail("bit index must be 0..7 in '" + operand.text + "'.", line);
        operand.bit = bit_text[0] - '0';
        parseAddress(body.substr(0, dot), operand, line);
        if (operand.absolute) fail("bit operands take an unmarked address, got '" + operand.text + "'.", line);
        return operand;
    }
    if (operand.inverted) fail("'/' only negates a mem.bit operand, got '" + operand.text + "'.", line);

    operand.shape = Shape::Address;
    parseAddress(text, operand, line);
    return operand;
}

bool matches(OperandPattern pattern, int fixed, const Operand& operand) {
    switch (pattern) {
        case OperandPattern::None: return false;
        case OperandPattern::A:
        case OperandPattern::X:
        case OperandPattern::Y:
        case OperandPattern::YA:
        case OperandPattern::SP:
        case OperandPattern::PSW:
        case OperandPattern::C:
            return operand.shape == Shape::Register && operand.reg == pattern;
        case OperandPattern::Imm: return operand.shape == Shape::Immediate;
        case OperandPattern::Dp: return operand.shape == Shape::Address && !operand.absolute;
        case OperandPattern::DpX: return operand.shape == Shape::IndexedX && !operand.absolute;
        case OperandPattern::DpY: return operand.shape == Shape::IndexedY && !operand.absolute;
        case OperandPattern::Abs: return operand.shape == Shape::Address && operand.absolute;
        case OperandPattern::AbsX: return operand.shape == Shape::IndexedX && operand.absolute;
        case OperandPattern::AbsY: return operand.shape == Shape::IndexedY && operand.absolute;
        case OperandPattern::IndX: return operand.shape == Shape::IndX;
        case OperandPattern::IndXInc: return operand.shape == Shape::IndXInc;
        case OperandPattern::IndY: return operand.shape == Shape::IndY;
        case OperandPattern::DpXInd: return operand.shape == Shape::BracketX && !operand.absolute;
        case OperandPattern::DpIndY: return operand.shape == Shape::BracketIndY && !operand.absolute;
        case OperandPattern::AbsXInd: return operand.shape == Shape::BracketX && operand.absolute;
        case OperandPattern::DpBit: return operand.shape == Shape::Bit && !operand.inverted && operand.bit == fixed;
        case OperandPattern::MemBit: return operand.shape == Shape::Bit && !operand.inverted;
        case OperandPattern::NotMemBit: return operand.shape == Shape::Bit && operand.inverted;
        case OperandPattern::Rel: return operand.shape == Shape::Address && !operand.absolute && operand.symbolic;
        case OperandPattern::Upage: return operand.shape == Shape::Address && !operand.absolute && !operand.symbolic;
        case OperandPattern::TcallN:
            return operand.shape == Shape::Address && !operand.absolute && !operand.symbolic && operand.value == fixed;
    }
    return false;
}

std::size_t patternCount(const InstructionInfo& info) {
    return info.operands[0] == OperandPattern::None ? 0 : info.operands[1] == OperandPattern::None ? 1 : 2;
}

bool isMnemonic(const std::string& mnemonic) {
    for (int opcode = 0; opcode < 256; ++opcode)
        if (mnemonic == SPC700Isa::instruction(static_cast<std::uint8_t>(opcode)).mnemonic) return true;
    return false;
}

// Branch mnemonics whose last operand is an 8-bit displacement.
bool isBranchMnemonic(const std::string& mnemonic) {
    static const std::set<std::string> branches = {
        "bra", "beq", "bne", "bcs", "bcc", "bvs", "bvc", "bmi", "bpl", "cbne", "dbnz", "bbs", "bbc"};
    return branches.count(mnemonic) != 0;
}

struct Selected {
    const InstructionInfo* info = nullptr;
    std::vector<Operand> operands;
};

// Selects the unique opcode for `line`. The opcode map is unambiguous: two
// rows never accept the same operand shapes (checked by the encoding tests).
Selected select(const ParsedLine& line) {
    Selected selected;
    for (const auto& text : line.operands) selected.operands.push_back(parseOperand(text, line.line_number));
    for (int opcode = 0; opcode < 256; ++opcode) {
        const auto& info = SPC700Isa::instruction(static_cast<std::uint8_t>(opcode));
        if (line.operation != info.mnemonic || patternCount(info) != selected.operands.size()) continue;
        bool ok = true;
        for (std::size_t index = 0; index < selected.operands.size() && ok; ++index)
            ok = matches(info.operands[index], info.fixed, selected.operands[index]);
        if (ok) {
            selected.info = &info;
            return selected;
        }
    }
    if (!isMnemonic(line.operation)) fail("unknown instruction or directive '" + line.operation + "'.", line.line_number);
    if (line.operation == "tcall") fail("TCALL expects a vector number in 0..15.", line.line_number);
    if (line.operation == "pcall") fail("PCALL expects a numeric page-$FF offset in 0..255.", line.line_number);
    if (isBranchMnemonic(line.operation) && !selected.operands.empty() &&
        selected.operands.back().shape == Shape::Address && !selected.operands.back().symbolic)
        fail("branch target must be a label, got '" + selected.operands.back().text + "'.", line.line_number);
    std::string spelled;
    for (const auto& text : line.operands) spelled += (spelled.empty() ? "" : ",") + trim(text);
    fail("invalid operands for '" + line.operation + "': " + (spelled.empty() ? "(none)" : spelled) + ".", line.line_number);
}

class SPC700TargetEncoder final : public TargetEncoder {
public:
    TargetKind target() const override { return TargetKind::SPC700; }

    bool acceptsCpuName(const std::string& name) const override { return name == "spc700"; }

    void parseDefine(const std::string& rest, int line_number) override {
        std::istringstream definition(rest);
        std::string name, value, extra;
        if (!(definition >> name >> value) || (definition >> extra)) {
            fail(".define expects one reserved configuration name and value.", line_number);
        }
        if (name != "__DISCO_CODE_START_ADDRESS" && name != "__DISCO_DATA_ALIGNMENT" && name != "__DISCO_RAM_ALIGNMENT")
            fail("unsupported .define name '" + name + "' for SPC700.", line_number);
        if (!m_definitions.insert(name).second) fail("duplicate configuration definition '" + name + "'.", line_number);
        std::int64_t number = 0;
        if (name == "__DISCO_CODE_START_ADDRESS") {
            if (!parseNumber(value, number) || number < 0 || number > 0xFFFF)
                fail("SPC700 code start address must fit in 16 bits.", line_number);
            m_config.code_start_address = static_cast<std::uint32_t>(number);
            return;
        }
        if (!parseNumber(value, number) || number < 1 || number > 128 || (number & (number - 1)))
            fail("Section alignment must be a power of two in 1..128.", line_number);
        if (name == "__DISCO_DATA_ALIGNMENT") m_data_alignment = static_cast<std::uint8_t>(number);
        else m_ram_alignment = static_cast<std::uint8_t>(number);
    }

    void validateDefinitions() override {}

    bool supportsSection(Section section) const override {
        return section == Section::Code || section == Section::Data || section == Section::Ram;
    }

    void validateLayout(std::size_t) const override {}

    bool shortBranch(const ParsedLine& line, std::string& target, std::size_t& short_size) const override {
        if (!isBranchMnemonic(line.operation) || line.operands.empty()) return false;
        const auto selected = select(line);
        if (selected.info->operands[patternCount(*selected.info) - 1] != OperandPattern::Rel) return false;
        target = selected.operands.back().symbol;
        short_size = selected.info->size;
        return true;
    }

    void configureObject(ObjectFile& object) const override {
        object.config = m_config;
        object.data_alignment = m_data_alignment;
        object.ram_alignment = m_ram_alignment;
    }

    std::size_t encode(const ParsedLine& line, std::vector<std::uint8_t>* out, EncodeContext& context) override {
        const auto selected = select(line);
        const auto& info = *selected.info;
        const auto count = patternCount(info);
        const bool branch = count != 0 && info.operands[count - 1] == OperandPattern::Rel;
        if (branch && line.section != Section::Code)
            fail("branches are only valid in .segment \"CODE\".", line.line_number);
        if (branch && context.isRelaxed(line.line_number)) return encodeRelaxed(line, selected, out, context);

        const auto start = out ? out->size() : 0;
        emitByte(out, info.opcode);
        if (info.layout == OperandLayout::Reversed) {
            emitOperand(line, info.operands[1], selected.operands[1], out, context);
            emitOperand(line, info.operands[0], selected.operands[0], out, context);
        } else {
            for (std::size_t index = 0; index < count; ++index)
                emitOperand(line, info.operands[index], selected.operands[index], out, context);
        }
        if (out && out->size() - start != info.size)
            fail("internal error: SPC700 encoding size mismatch.", line.line_number);
        return info.size;
    }

private:
    CompilerConfig m_config = []() {
        CompilerConfig config;
        config.target = TargetKind::SPC700;
        return config;
    }();
    std::uint8_t m_data_alignment = 1;
    std::uint8_t m_ram_alignment = 2;
    std::set<std::string> m_definitions;

    static void relocate(const ParsedLine& line, std::vector<std::uint8_t>* out, EncodeContext& context,
                         const std::string& symbol, RelocationType type) {
        if (out) context.addRelocation({symbol, symbolSection(line.section), static_cast<std::uint32_t>(out->size()), type});
    }

    static void emitDirectPage(const ParsedLine& line, const Operand& operand, std::vector<std::uint8_t>* out,
                               EncodeContext& context) {
        if (operand.symbolic) {
            relocate(line, out, context, operand.symbol, RelocationType::SPC_DP8);
            emitByte(out, 0);
            return;
        }
        if (operand.value < 0 || operand.value > 0xFF)
            fail("direct-page operand '" + operand.text + "' exceeds $FF; write !address for an absolute operand.", line.line_number);
        emitByte(out, static_cast<std::uint8_t>(operand.value));
    }

    static void emitAbsolute(const ParsedLine& line, const Operand& operand, std::vector<std::uint8_t>* out,
                             EncodeContext& context) {
        if (operand.symbolic) {
            relocate(line, out, context, operand.symbol, RelocationType::SPC_ABS16);
            emitWord(out, 0);
            return;
        }
        if (operand.value < 0 || operand.value > 0xFFFF)
            fail("absolute operand '" + operand.text + "' exceeds $FFFF.", line.line_number);
        emitWord(out, static_cast<std::uint16_t>(operand.value));
    }

    static void emitOperand(const ParsedLine& line, OperandPattern pattern, const Operand& operand,
                            std::vector<std::uint8_t>* out, EncodeContext& context) {
        switch (pattern) {
            case OperandPattern::Imm:
                if (operand.symbolic) {
                    relocate(line, out, context, operand.symbol, operand.part == ImmediatePart::Low
                        ? RelocationType::SPC_IMM8_LO : RelocationType::SPC_IMM8_HI);
                    emitByte(out, 0);
                    return;
                }
                if (operand.value < 0 || operand.value > 0xFF)
                    fail("8-bit immediate '" + operand.text + "' is outside 0..255.", line.line_number);
                emitByte(out, static_cast<std::uint8_t>(operand.value));
                return;
            case OperandPattern::Dp:
            case OperandPattern::DpX:
            case OperandPattern::DpY:
            case OperandPattern::DpXInd:
            case OperandPattern::DpIndY:
            case OperandPattern::DpBit:
                emitDirectPage(line, operand, out, context);
                return;
            case OperandPattern::Abs:
            case OperandPattern::AbsX:
            case OperandPattern::AbsY:
            case OperandPattern::AbsXInd:
                emitAbsolute(line, operand, out, context);
                return;
            case OperandPattern::MemBit:
            case OperandPattern::NotMemBit: {
                const auto bit_bits = static_cast<std::uint16_t>(operand.bit << 13);
                if (operand.symbolic) {
                    relocate(line, out, context, operand.symbol, RelocationType::SPC_MEMBIT13);
                    emitWord(out, bit_bits);
                    return;
                }
                if (operand.value < 0 || operand.value > 0x1FFF)
                    fail("mem.bit address in '" + operand.text + "' must be in $0000-$1FFF.", line.line_number);
                emitWord(out, static_cast<std::uint16_t>(bit_bits | operand.value));
                return;
            }
            case OperandPattern::Rel:
                if (out) context.addBranchFixup(out->size(), operand.symbol, line.line_number);
                emitByte(out, 0);
                return;
            case OperandPattern::Upage:
                if (operand.value < 0 || operand.value > 0xFF)
                    fail("PCALL expects a numeric page-$FF offset in 0..255.", line.line_number);
                emitByte(out, static_cast<std::uint8_t>(operand.value));
                return;
            case OperandPattern::None:
            case OperandPattern::A:
            case OperandPattern::X:
            case OperandPattern::Y:
            case OperandPattern::YA:
            case OperandPattern::SP:
            case OperandPattern::PSW:
            case OperandPattern::C:
            case OperandPattern::IndX:
            case OperandPattern::IndXInc:
            case OperandPattern::IndY:
            case OperandPattern::TcallN:
                return;
        }
    }

    // Long forms keep the branch semantics with a 16-bit JMP to the private
    // copy of the target label:
    //   BRA L            -> JMP !L
    //   Bcc L            -> B!cc +3; JMP !L
    //   BBS/BBC d.b,L    -> BBC/BBS d.b,+3; JMP !L
    //   CBNE/DBNZ ...,L  -> CBNE/DBNZ ...,+2; BRA +3; JMP !L
    static std::size_t encodeRelaxed(const ParsedLine& line, const Selected& selected, std::vector<std::uint8_t>* out,
                                     EncodeContext& context) {
        const auto& info = *selected.info;
        const auto start = out ? out->size() : 0;
        const auto& mnemonic = line.operation;
        const auto jumpTo = [&]() {
            emitByte(out, 0x5F); // JMP !abs
            if (out) context.addRelocation({std::string(1, InternalSymbolPrefix) + selected.operands.back().symbol,
                SymbolSection::CODE, static_cast<std::uint32_t>(out->size()), RelocationType::SPC_ABS16});
            emitWord(out, 0);
        };
        if (mnemonic == "bra") {
            jumpTo();
        } else if (mnemonic == "bbs" || mnemonic == "bbc") {
            emitByte(out, static_cast<std::uint8_t>(info.opcode ^ 0x10)); // BBS <-> BBC, same bit.
            emitDirectPage(line, selected.operands[0], out, context);
            emitByte(out, 3);
            jumpTo();
        } else if (mnemonic == "cbne" || mnemonic == "dbnz") {
            emitByte(out, info.opcode);
            if (info.operands[0] != OperandPattern::Y) emitDirectPage(line, selected.operands[0], out, context);
            emitByte(out, 2);
            emitByte(out, 0x2F); // BRA +3 over the JMP
            emitByte(out, 3);
            jumpTo();
        } else {
            // Conditional branches pair as (opcode, opcode ^ 0x20): BPL/BMI,
            // BVC/BVS, BCC/BCS, BNE/BEQ.
            emitByte(out, static_cast<std::uint8_t>(info.opcode ^ 0x20));
            emitByte(out, 3);
            jumpTo();
        }
        const std::size_t size = mnemonic == "bra" ? 3 :
            (mnemonic == "bbs" || mnemonic == "bbc") ? 6 :
            (mnemonic == "cbne" || mnemonic == "dbnz") ? (info.operands[0] == OperandPattern::Y ? 7u : 8u) : 5;
        if (out && out->size() - start != size)
            fail("internal error: SPC700 relaxed branch size mismatch.", line.line_number);
        return size;
    }
};

} // namespace

std::unique_ptr<TargetEncoder> makeSPC700TargetEncoder() {
    return std::make_unique<SPC700TargetEncoder>();
}

} // namespace AsmSupport
