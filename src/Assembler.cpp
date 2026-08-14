#include "Assembler.hpp"

#include "CompilerError.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

enum class Section { None, Code, Data };

struct ParsedLine {
    int line_number = 0;
    Section section = Section::None;
    std::string label;
    std::string operation;
    std::vector<std::string> operands;
};

struct SymbolDefinition {
    SymbolSection section;
    std::size_t offset = 0;
};

struct BranchFixup {
    std::size_t patch_offset = 0;
    std::string target;
    int line_number = 0;
};

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool isIdentifier(const std::string& value) {
    if (value.empty()) return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (!(std::isalpha(first) || value.front() == '_' || value.front() == '.' || value.front() == '$')) {
        return false;
    }
    for (std::size_t i = 1; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (!(std::isalnum(c) || value[i] == '_' || value[i] == '.' || value[i] == '$')) return false;
    }
    return true;
}

std::vector<std::string> splitOperands(const std::string& text) {
    std::vector<std::string> result;
    std::size_t start = 0;
    int parentheses = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '"') quoted = !quoted;
        if (quoted) continue;
        if (text[i] == '(') ++parentheses;
        if (text[i] == ')') --parentheses;
        if (text[i] == ',' && parentheses == 0) {
            result.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    const auto tail = trim(text.substr(start));
    if (!tail.empty()) result.push_back(tail);
    return result;
}

std::string withoutHash(std::string value) {
    value = trim(value);
    if (!value.empty() && value.front() == '#') value.erase(value.begin());
    return trim(value);
}

bool parseNumber(const std::string& text, std::int64_t& value) {
    std::string token = withoutHash(text);
    if (token.empty()) return false;

    bool negative = false;
    if (token.front() == '-') {
        negative = true;
        token.erase(token.begin());
    } else if (token.front() == '+') {
        token.erase(token.begin());
    }
    if (token.empty()) return false;

    int base = 0;
    if (token.front() == '$') {
        base = 16;
        token.erase(token.begin());
    } else if (token.size() > 1 && token.front() == '%') {
        base = 2;
        token.erase(token.begin());
    }
    if (token.empty()) return false;

    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoll(token, &consumed, base);
        if (consumed != token.size()) return false;
        value = negative ? -parsed : parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

int parseRegister(const std::string& text) {
    const auto name = lower(trim(text));
    if (name == "sp") return 10;
    if (name == "pc") return 15;
    if (name.size() < 2 || name.front() != 'r') return -1;
    std::int64_t value = 0;
    if (!parseNumber(name.substr(1), value) || value < 0 || value > 15) return -1;
    return static_cast<int>(value);
}

int parseMemoryRegister(const std::string& text) {
    const auto value = trim(text);
    if (value.size() < 4 || value.front() != '(' || value.back() != ')') return -1;
    return parseRegister(value.substr(1, value.size() - 2));
}

} // namespace

class AssemblerImpl {
public:
    ObjectFile assemble(const std::string& source) {
        m_lines = parse(source);
        firstPass();
        relaxBranches();
        secondPass();
        resolveBranches();
        addInternalSymbols();
        addExportedSymbols();
        return std::move(m_object);
    }

private:
    std::vector<ParsedLine> m_lines;
    std::map<std::string, SymbolDefinition> m_symbols;
    std::set<std::string> m_exports;
    std::vector<BranchFixup> m_branch_fixups;
    ObjectFile m_object;
    std::size_t m_code_size = 0;
    std::size_t m_data_size = 0;
    std::set<int> m_relaxed_branch_lines;
    std::map<int, std::size_t> m_code_offsets_by_line;

    static constexpr char InternalSymbolPrefix = '\x01';

    [[noreturn]] void fail(const std::string& message, int line) const {
        throw CompilerError("Assembler: " + message, line, 1);
    }

    static SymbolSection symbolSection(Section section) {
        return section == Section::Code ? SymbolSection::CODE : SymbolSection::DATA;
    }

    static std::size_t& offsetFor(Section section, std::size_t& code, std::size_t& data) {
        if (section == Section::Code) return code;
        return data;
    }

    static std::size_t currentOffset(Section section, std::size_t code, std::size_t data) {
        return section == Section::Code ? code : data;
    }

    std::vector<ParsedLine> parse(const std::string& source) {
        std::vector<ParsedLine> lines;
        Section current = Section::None;
        std::istringstream input(source);
        std::string raw;
        int line_number = 0;

        while (std::getline(input, raw)) {
            ++line_number;
            const auto comment = raw.find(';');
            if (comment != std::string::npos) raw.erase(comment);
            raw = trim(raw);
            if (raw.empty()) continue;

            ParsedLine line;
            line.line_number = line_number;
            line.section = current;

            const auto colon = raw.find(':');
            const auto whitespace = raw.find_first_of(" \t");
            if (colon != std::string::npos && (whitespace == std::string::npos || colon < whitespace)) {
                line.label = trim(raw.substr(0, colon));
                if (!isIdentifier(line.label)) fail("invalid label '" + line.label + "'.", line_number);
                raw = trim(raw.substr(colon + 1));
                if (raw.empty()) {
                    lines.push_back(line);
                    continue;
                }
            }

            std::istringstream statement(raw);
            statement >> line.operation;
            line.operation = lower(line.operation);
            std::string rest;
            std::getline(statement, rest);
            line.operands = splitOperands(rest);
            lines.push_back(line);

            if (line.operation == ".segment") {
                if (line.operands.size() != 1) fail(".segment expects one name.", line_number);
                const auto name = lower(line.operands.front());
                if (name == "\"code\"") current = Section::Code;
                else if (name == "\"data\"") current = Section::Data;
                else fail("unsupported segment " + line.operands.front() + ".", line_number);
            }
        }
        return lines;
    }

    void requireSection(const ParsedLine& line) const {
        if (line.section == Section::None) fail("statement appears before a .segment directive.", line.line_number);
    }

    void defineLabel(const ParsedLine& line, std::size_t code, std::size_t data) {
        if (line.label.empty()) return;
        requireSection(line);
        if (m_symbols.count(line.label)) fail("duplicate label '" + line.label + "'.", line.line_number);
        m_symbols.emplace(line.label, SymbolDefinition{symbolSection(line.section), currentOffset(line.section, code, data)});
    }

    void firstPass() {
        m_symbols.clear();
        m_code_offsets_by_line.clear();
        std::size_t code = 0;
        std::size_t data = 0;
        for (const auto& line : m_lines) {
            defineLabel(line, code, data);
            if (line.operation.empty() || line.operation == ".segment" || line.operation == ".export" ||
                line.operation == ".setcpu" || line.operation == ".include") {
                if (line.operation == ".export") {
                    for (const auto& operand : line.operands) {
                        if (!isIdentifier(operand)) fail("invalid export name '" + operand + "'.", line.line_number);
                        m_exports.insert(operand);
                    }
                }
                continue;
            }
            requireSection(line);
            if (line.section == Section::Code) {
                m_code_offsets_by_line[line.line_number] = code;
            }
            if (line.operation == ".byte" || line.operation == ".word") {
                if (line.operands.empty()) fail(line.operation + " expects at least one value.", line.line_number);
                auto& offset = offsetFor(line.section, code, data);
                offset += line.operands.size() * (line.operation == ".byte" ? 1u : 2u);
            } else {
                auto& offset = offsetFor(line.section, code, data);
                offset += encodeInstruction(line, nullptr, false);
            }
        }
        m_code_size = code;
        m_data_size = data;
    }

    void relaxBranches() {
        static const std::set<std::string> branches = {
            "bra", "bge", "blt", "bne", "beq", "bpl", "bmi",
            "bcc", "bcs", "bvc", "bvs"};
        for (;;) {
            bool changed = false;
            for (const auto& line : m_lines) {
                if (line.section != Section::Code || line.operation.empty() ||
                    branches.count(line.operation) == 0 || line.operands.size() != 1 ||
                    m_relaxed_branch_lines.count(line.line_number) != 0) {
                    continue;
                }
                const auto symbol = m_symbols.find(line.operands.front());
                if (symbol == m_symbols.end()) {
                    fail("undefined branch label '" + line.operands.front() + "'.", line.line_number);
                }
                if (symbol->second.section != SymbolSection::CODE) {
                    fail("branch target must be in the code section.", line.line_number);
                }
                const auto source = m_code_offsets_by_line.at(line.line_number);
                const auto distance = static_cast<std::int64_t>(symbol->second.offset) -
                                      static_cast<std::int64_t>(source + 2);
                if (distance < -128 || distance > 127) {
                    changed = m_relaxed_branch_lines.insert(line.line_number).second || changed;
                }
            }
            if (!changed) break;
            // Recompute labels after every expansion. Earlier expansions can
            // move a later branch target outside the short-branch range.
            firstPass();
        }
    }

    void secondPass() {
        m_object = ObjectFile();
        m_object.code_section.reserve(m_code_size);
        m_object.data_section.reserve(m_data_size);

        for (const auto& line : m_lines) {
            if (line.operation.empty() || line.operation == ".segment" || line.operation == ".export" ||
                line.operation == ".setcpu" || line.operation == ".include") continue;
            requireSection(line);
            if (line.operation == ".byte" || line.operation == ".word") {
                encodeData(line);
            } else {
                encodeInstruction(line, output(line.section), true);
            }
        }
    }

    std::vector<uint8_t>* output(Section section) {
        return section == Section::Code ? &m_object.code_section : &m_object.data_section;
    }

    static void emitByte(std::vector<uint8_t>* output, uint8_t value) {
        if (output) output->push_back(value);
    }

    static void emitWord(std::vector<uint8_t>* output, uint16_t value) {
        emitByte(output, static_cast<uint8_t>(value & 0xff));
        emitByte(output, static_cast<uint8_t>((value >> 8) & 0xff));
    }

    void encodeData(const ParsedLine& line) {
        auto* out = output(line.section);
        const bool byte = line.operation == ".byte";
        for (const auto& operand : line.operands) {
            std::int64_t value = 0;
            if (!parseNumber(operand, value)) fail("data values must be numeric.", line.line_number);
            if (byte) {
                if (value < 0 || value > 255) fail(".byte value is outside 0..255.", line.line_number);
                emitByte(out, static_cast<uint8_t>(value));
            } else {
                if (value < -32768 || value > 65535) fail(".word value is outside 16-bit range.", line.line_number);
                emitWord(out, static_cast<uint16_t>(value));
            }
        }
    }

    void requireOperands(const ParsedLine& line, std::size_t count) const {
        if (line.operands.size() != count) {
            fail(line.operation + " expects " + std::to_string(count) + " operand(s).", line.line_number);
        }
    }

    int requireRegister(const std::string& operand, const ParsedLine& line) const {
        const int reg = parseRegister(operand);
        if (reg < 0) fail("invalid register '" + operand + "'.", line.line_number);
        return reg;
    }

    int requireMemoryRegister(const std::string& operand, const ParsedLine& line) const {
        const int reg = parseMemoryRegister(operand);
        if (reg < 0 || reg > 11) fail("invalid indirect register '" + operand + "'.", line.line_number);
        return reg;
    }

    uint16_t requireWord(const std::string& operand, const ParsedLine& line) const {
        std::int64_t value = 0;
        if (!parseNumber(operand, value) || value < -32768 || value > 65535) {
            fail("expected a 16-bit numeric value, got '" + operand + "'.", line.line_number);
        }
        return static_cast<uint16_t>(value);
    }

    uint8_t requireByte(const std::string& operand, const ParsedLine& line) const {
        std::int64_t value = 0;
        if (!parseNumber(operand, value) || value < 0 || value > 255) {
            fail("expected an 8-bit numeric value, got '" + operand + "'.", line.line_number);
        }
        return static_cast<uint8_t>(value);
    }

    void emitImmediateWord(
        const ParsedLine& line,
        std::vector<uint8_t>* out,
        int reg,
        const std::string& operand,
        RelocationType relocation) {
        const std::size_t patch = out ? out->size() : 0;
        emitByte(out, static_cast<uint8_t>(0xf0 | reg));
        std::int64_t value = 0;
        if (parseNumber(operand, value)) {
            if (value < -32768 || value > 65535) fail("word immediate is out of range.", line.line_number);
            emitWord(out, static_cast<uint16_t>(value));
        } else {
            if (!isIdentifier(withoutHash(operand))) fail("invalid symbolic immediate '" + operand + "'.", line.line_number);
            emitWord(out, 0);
            if (out) m_object.relocation_table.push_back({withoutHash(operand), symbolSection(line.section), static_cast<uint32_t>(patch), relocation});
        }
    }

    std::size_t encodeBranch(const ParsedLine& line, std::vector<uint8_t>* out, uint8_t opcode) {
        requireOperands(line, 1);
        if (line.section != Section::Code) fail("branches are only valid in .segment \"CODE\".", line.line_number);
        const auto patch = out ? out->size() : 0;
        if (m_relaxed_branch_lines.count(line.line_number) != 0) {
            if (line.operation == "bra") {
                emitByte(out, 0xff); // IWT R15, absolute target
                emitWord(out, 0);
                if (out) m_object.relocation_table.push_back({
                    std::string(1, InternalSymbolPrefix) + line.operands.front(),
                    SymbolSection::CODE, static_cast<std::uint32_t>(patch),
                    RelocationType::ADDR16_IWT});
                emitByte(out, 0x9f); // JMP R15
                return 4;
            }

            static const std::map<uint8_t, uint8_t> inverse = {
                {0x06, 0x07}, {0x07, 0x06}, {0x08, 0x09}, {0x09, 0x08},
                {0x0a, 0x0b}, {0x0b, 0x0a}, {0x0c, 0x0d}, {0x0d, 0x0c},
                {0x0e, 0x0f}, {0x0f, 0x0e}};
            emitByte(out, inverse.at(opcode));
            emitByte(out, 4); // Skip the following IWT/JMP sequence.
            emitByte(out, 0xff);
            emitWord(out, 0);
            if (out) m_object.relocation_table.push_back({
                std::string(1, InternalSymbolPrefix) + line.operands.front(),
                SymbolSection::CODE, static_cast<std::uint32_t>(patch + 2),
                RelocationType::ADDR16_IWT});
            emitByte(out, 0x9f);
            return 6;
        }
        emitByte(out, opcode);
        emitByte(out, 0);
        if (out) m_branch_fixups.push_back({patch + 1, line.operands.front(), line.line_number});
        return 2;
    }

    std::size_t encodeArithmetic(const ParsedLine& line, std::vector<uint8_t>* out) {
        const auto& op = line.operation;
        uint8_t base = 0;
        bool immediate_alt1 = false;
        bool immediate_alt3 = false;
        bool register_alt1 = false;
        bool register_alt3 = false;
        if (op == "add" || op == "adc") { base = 0x50; register_alt1 = op == "adc"; immediate_alt3 = op == "adc"; }
        else if (op == "sub" || op == "sbc") { base = 0x60; register_alt1 = op == "sbc"; }
        else if (op == "mult" || op == "umult") { base = 0x80; register_alt1 = op == "umult"; immediate_alt3 = op == "umult"; }
        else if (op == "cmp") { base = 0x60; register_alt3 = true; }
        else if (op == "and" || op == "or") { base = op == "and" ? 0x71 : 0xc1; immediate_alt1 = false; }
        else if (op == "bic" || op == "xor") { base = op == "bic" ? 0x71 : 0xc1; register_alt1 = true; immediate_alt1 = true; }
        else return 0;

        if (line.operands.size() == 1) {
            std::int64_t value = 0;
            if (parseNumber(line.operands[0], value)) {
                if (op == "cmp" || op == "sbc") fail("this arithmetic operation has no immediate form.", line.line_number);
                if (value < (op == "and" || op == "or" || op == "bic" || op == "xor" ? 1 : 0) || value > 15) fail("immediate arithmetic value is out of range.", line.line_number);
                if (immediate_alt1) emitByte(out, 0x3d);
                else if (immediate_alt3) emitByte(out, 0x3f);
                else emitByte(out, 0x3e);
                emitByte(out, static_cast<uint8_t>(base + value - ((op == "and" || op == "or" || op == "bic" || op == "xor") ? 1 : 0)));
                return immediate_alt1 || immediate_alt3 ? 2 : 2;
            }
            const int reg = requireRegister(line.operands[0], line);
            if (reg == 0 && (op == "and" || op == "or" || op == "bic" || op == "xor")) fail("register R0 is not valid for this operation.", line.line_number);
            if (register_alt1) emitByte(out, 0x3d);
            if (register_alt3) emitByte(out, 0x3f);
            emitByte(out, static_cast<uint8_t>(base + (op == "and" || op == "or" || op == "bic" || op == "xor" ? reg - 1 : reg)));
            return 1 + (register_alt1 ? 1 : 0) + (register_alt3 ? 1 : 0);
        }

        if (line.operands.size() == 2) {
            const int dest = requireRegister(line.operands[0], line);
            std::int64_t parsed_immediate = 0;
            if (parseNumber(line.operands[1], parsed_immediate)) {
                if (dest != 0 && dest != 10) emitByte(out, static_cast<uint8_t>(0x20 + dest));
                if (parsed_immediate < 0 || parsed_immediate > 15 || op == "cmp") fail("invalid two-operand immediate arithmetic form.", line.line_number);
                emitByte(out, 0x3e);
                emitByte(out, static_cast<uint8_t>(base + parsed_immediate));
                return 2 + ((dest != 0 && dest != 10) ? 1 : 0);
            }
            const int source = requireRegister(line.operands[1], line);
            if (dest != 0) emitByte(out, static_cast<uint8_t>(0x20 + dest));
            if (register_alt1) emitByte(out, 0x3d);
            if (register_alt3) emitByte(out, 0x3f);
            emitByte(out, static_cast<uint8_t>(base + (op == "and" || op == "or" || op == "bic" || op == "xor" ? source - 1 : source)));
            return 1 + (dest != 0 ? 1 : 0) + (register_alt1 ? 1 : 0) + (register_alt3 ? 1 : 0);
        }

        requireOperands(line, 3);
        const int dest = requireRegister(line.operands[0], line);
        const int left = requireRegister(line.operands[1], line);
        if (dest != left) fail("three-operand arithmetic requires destination and left operand to match.", line.line_number);
        if (dest != 0) emitByte(out, static_cast<uint8_t>(0x20 + dest));
        std::int64_t immediate = 0;
        if (parseNumber(line.operands[2], immediate)) {
            if (op == "cmp" || op == "sbc" || immediate < 0 || immediate > 15) fail("immediate arithmetic value is out of range.", line.line_number);
            emitByte(out, 0x3e);
            emitByte(out, static_cast<uint8_t>(base + immediate));
            return 2 + (dest != 0 ? 1 : 0);
        }
        const int source = requireRegister(line.operands[2], line);
        if (register_alt1) emitByte(out, 0x3d);
        if (register_alt3) emitByte(out, 0x3f);
        emitByte(out, static_cast<uint8_t>(base + (op == "and" || op == "or" || op == "bic" || op == "xor" ? source - 1 : source)));
        return 1 + (dest != 0 ? 1 : 0) + (register_alt1 ? 1 : 0) + (register_alt3 ? 1 : 0);
    }

    std::size_t encodeInstruction(const ParsedLine& line, std::vector<uint8_t>* out, bool record_relocations) {
        (void)record_relocations;
        const auto& op = line.operation;
        static const std::map<std::string, uint8_t> branches = {
            {"bra", 0x05}, {"bge", 0x06}, {"blt", 0x07}, {"bne", 0x08}, {"beq", 0x09},
            {"bpl", 0x0a}, {"bmi", 0x0b}, {"bcc", 0x0c}, {"bcs", 0x0d}, {"bvc", 0x0e}, {"bvs", 0x0f}
        };
        const auto branch = branches.find(op);
        if (branch != branches.end()) return encodeBranch(line, out, branch->second);

        static const std::map<std::string, uint8_t> implied = {
            {"stop", 0x00}, {"nop", 0x01}, {"cache", 0x02}, {"lsr", 0x03}, {"rol", 0x04}, {"loop", 0x3c},
            {"plot", 0x4c}, {"color", 0x4e}, {"swap", 0x4d}, {"not", 0x4f}, {"merge", 0x70},
            {"msbk", 0x90}, {"sex", 0x95}, {"asr", 0x96}, {"ror", 0x97}, {"lob", 0x9e},
            {"getc", 0xdf}, {"getb", 0xef}
        };
        const auto implied_op = implied.find(op);
        if (implied_op != implied.end()) {
            requireOperands(line, 0);
            emitByte(out, implied_op->second);
            return 1;
        }
        if (op == "alt1" || op == "alt2" || op == "alt3") {
            requireOperands(line, 0);
            emitByte(out, static_cast<uint8_t>(op == "alt1" ? 0x3d : op == "alt2" ? 0x3e : 0x3f));
            return 1;
        }
        if (op == "rpix" || op == "cmode" || op == "div2" || op == "ramb" || op == "romb" || op == "getbh" || op == "getbl" || op == "getbs") {
            requireOperands(line, 0);
            emitByte(out, static_cast<uint8_t>((op == "rpix" || op == "cmode" || op == "div2") ? 0x3d : op == "ramb" ? 0x3e : 0x3f));
            emitByte(out, static_cast<uint8_t>(op == "rpix" ? 0x4c : op == "cmode" ? 0x4e : op == "div2" ? 0x96 : op == "ramb" || op == "romb" ? 0xdf : 0xef));
            return 2;
        }
        if (op == "to" || op == "with" || op == "from") {
            requireOperands(line, 1);
            const int reg = requireRegister(line.operands[0], line);
            emitByte(out, static_cast<uint8_t>((op == "to" ? 0x10 : op == "with" ? 0x20 : 0xb0) + reg));
            return 1;
        }
        if (op == "inc" || op == "dec") {
            requireOperands(line, 1);
            const int reg = requireRegister(line.operands[0], line);
            if (reg == 15) fail("increment/decrement cannot target R15.", line.line_number);
            emitByte(out, static_cast<uint8_t>((op == "inc" ? 0xd0 : 0xe0) + reg));
            return 1;
        }
        if (op == "jmp" || op == "ljmp") {
            requireOperands(line, 1);
            const int reg = requireRegister(line.operands[0], line);
            if (reg < 8 || reg > 13) fail("JMP requires a register from R8 to R13.", line.line_number);
            if (op == "ljmp") emitByte(out, 0x3d);
            emitByte(out, static_cast<uint8_t>(0x98 + reg - 8));
            return op == "ljmp" ? 2 : 1;
        }
        if (op == "link") {
            requireOperands(line, 1);
            const auto operand = withoutHash(line.operands[0]);
            const auto value = requireByte(operand, line);
            if (value < 1 || value > 4) fail("LINK immediate must be in 1..4.", line.line_number);
            emitByte(out, static_cast<uint8_t>(0x91 + value - 1));
            return 1;
        }
        if (op == "jal") {
            requireOperands(line, 1);
            emitByte(out, 0x94); // LINK #4
            emitImmediateWord(line, out, 15, "#" + line.operands[0], RelocationType::ADDR16_JAL);
            return 4;
        }
        if (op == "ret") {
            requireOperands(line, 0);
            emitByte(out, 0x9b);
            return 1;
        }
        if (op == "ibt" || op == "iwt" || op == "lea") {
            requireOperands(line, 2);
            const int reg = requireRegister(line.operands[0], line);
            if (op == "ibt") {
                emitByte(out, static_cast<uint8_t>(0xa0 + reg));
                emitByte(out, requireByte(withoutHash(line.operands[1]), line));
                return 2;
            }
            emitImmediateWord(line, out, reg, line.operands[1], RelocationType::ADDR16_IWT);
            return 3;
        }
        if (op == "move" || op == "moveb" || op == "movew") {
            requireOperands(line, 2);
            const bool byte = op == "moveb";
            const int destination = parseRegister(line.operands[0]);
            const int source = parseRegister(line.operands[1]);
            const int destination_memory = parseMemoryRegister(line.operands[0]);
            const int source_memory = parseMemoryRegister(line.operands[1]);
            if (destination >= 0 && source >= 0) {
                if (op != "move") fail("MOVEB/MOVEW register-to-register form is unsupported.", line.line_number);
                if (source != 0) emitByte(out, static_cast<uint8_t>(0x20 + source));
                emitByte(out, static_cast<uint8_t>(0x10 + destination));
                return source == 0 ? 1 : 2;
            }
            if (destination >= 0 && source_memory >= 0) {
                if (!byte && op == "move") {
                    if (destination != 0) emitByte(out, static_cast<uint8_t>(0x20 + destination));
                    emitByte(out, static_cast<uint8_t>(0x40 + source_memory));
                    return destination == 0 ? 1 : 2;
                }
                if (destination != 0) emitByte(out, static_cast<uint8_t>(0x20 + destination));
                if (byte) emitByte(out, 0x3d);
                emitByte(out, static_cast<uint8_t>(0x40 + source_memory));
                return (destination != 0 ? 1 : 0) + (byte ? 1 : 0) + 1;
            }
            if (destination_memory >= 0 && source >= 0) {
                if (source != 0) emitByte(out, static_cast<uint8_t>(0x20 + source));
                if (byte) emitByte(out, 0x3d);
                emitByte(out, static_cast<uint8_t>(0x30 + destination_memory));
                return (source != 0 ? 1 : 0) + (byte ? 1 : 0) + 1;
            }
            if (destination >= 0 && line.operands[1].front() == '#') {
                if (op != "move") fail("MOVEB/MOVEW immediate form is unsupported.", line.line_number);
                std::int64_t value = 0;
                if (!parseNumber(line.operands[1], value)) {
                    emitImmediateWord(line, out, destination, line.operands[1], RelocationType::ADDR16_IWT);
                    return 3;
                }
                if (value >= 0 && value <= 255) {
                    emitByte(out, static_cast<uint8_t>(0xa0 + destination));
                    emitByte(out, static_cast<uint8_t>(value));
                    return 2;
                }
                emitByte(out, static_cast<uint8_t>(0xf0 + destination));
                emitWord(out, static_cast<uint16_t>(value));
                return 3;
            }
            fail("unsupported MOVE operand form.", line.line_number);
        }
        if (op == "stw" || op == "stb" || op == "ldw" || op == "ldb") {
            requireOperands(line, 2);
            const bool store = op == "stw" || op == "stb";
            const bool byte = op == "stb" || op == "ldb";
            const int memory = store ? requireMemoryRegister(line.operands[0], line) : requireMemoryRegister(line.operands[1], line);
            const int reg = store ? requireRegister(line.operands[1], line) : requireRegister(line.operands[0], line);
            if (reg != 0) emitByte(out, static_cast<uint8_t>(0x20 + reg));
            if (byte) emitByte(out, 0x3d);
            emitByte(out, static_cast<uint8_t>((store ? 0x30 : 0x40) + memory));
            return (reg != 0 ? 1 : 0) + (byte ? 1 : 0) + 1;
        }
        if (op == "push" || op == "pop") {
            requireOperands(line, 1);
            const int reg = requireRegister(line.operands[0], line);
            if (op == "push") {
                if (reg != 0) emitByte(out, static_cast<uint8_t>(0x20 + reg));
                emitByte(out, 0x3a);
                emitByte(out, 0xea); emitByte(out, 0xea);
            } else {
                emitByte(out, 0xda); emitByte(out, 0xda);
                if (reg != 0) emitByte(out, static_cast<uint8_t>(0x20 + reg));
                emitByte(out, 0x4a);
            }
            return (reg != 0 ? 4 : 3);
        }
        if (op == "pushb" || op == "popb") {
            requireOperands(line, 1);
            const int reg = requireRegister(line.operands[0], line);
            if (op == "pushb") {
                if (reg != 0) emitByte(out, static_cast<uint8_t>(0x20 + reg));
                emitByte(out, 0x3d); emitByte(out, 0x3a); emitByte(out, 0xe0);
            } else {
                emitByte(out, 0xd0);
                if (reg != 0) emitByte(out, static_cast<uint8_t>(0x20 + reg));
                emitByte(out, 0x3d); emitByte(out, 0x4a);
            }
            return (reg != 0 ? 4 : 3);
        }
        if (op == "add" || op == "adc" || op == "sub" || op == "sbc" || op == "mult" || op == "umult" ||
            op == "cmp" || op == "and" || op == "bic" || op == "or" || op == "xor") {
            return encodeArithmetic(line, out);
        }
        if (op == "lm" || op == "lms" || op == "sm" || op == "sms") {
            requireOperands(line, 2);
            const bool load = op == "lm" || op == "lms";
            const bool short_address = op == "lms" || op == "sms";
            const int reg = load ? requireRegister(line.operands[0], line) : requireRegister(line.operands[1], line);
            const auto address_token = load ? line.operands[1] : line.operands[0];
            const auto address = requireWord(address_token, line);
            if (short_address) {
                if ((address & 1u) != 0 || address > 510) fail("short memory address must be even and <= 510.", line.line_number);
                emitByte(out, static_cast<uint8_t>(load ? 0x3d : 0x3e));
                emitByte(out, static_cast<uint8_t>((load ? 0xa0 : 0xa0) + reg));
                emitByte(out, static_cast<uint8_t>(address >> 1));
                return 3;
            }
            emitByte(out, static_cast<uint8_t>(load ? 0x3d : 0x3e));
            emitByte(out, static_cast<uint8_t>(0xf0 + reg));
            emitWord(out, address);
            return 4;
        }

        fail("unknown instruction or directive '" + op + "'.", line.line_number);
    }

    void resolveBranches() {
        for (const auto& fixup : m_branch_fixups) {
            const auto symbol = m_symbols.find(fixup.target);
            if (symbol == m_symbols.end()) fail("undefined branch label '" + fixup.target + "'.", fixup.line_number);
            if (symbol->second.section != SymbolSection::CODE) fail("branch target must be in the code section.", fixup.line_number);
            const auto distance = static_cast<std::int64_t>(symbol->second.offset) -
                                  (static_cast<std::int64_t>(fixup.patch_offset) + 1);
            if (distance < -128 || distance > 127) fail("branch target is out of range [-128, 127].", fixup.line_number);
            m_object.code_section.at(fixup.patch_offset) = static_cast<uint8_t>(static_cast<std::int8_t>(distance));
        }
    }

    void addExportedSymbols() {
        for (const auto& name : m_exports) {
            const auto symbol = m_symbols.find(name);
            if (symbol == m_symbols.end()) fail("exported symbol '" + name + "' is not defined.", 1);
            m_object.symbol_table.push_back({name, symbol->second.section, static_cast<uint32_t>(symbol->second.offset)});
        }
    }

    void addInternalSymbols() {
        std::set<std::string> added;
        for (const auto& line : m_lines) {
            if (m_relaxed_branch_lines.count(line.line_number) == 0) continue;
            const auto symbol = m_symbols.find(line.operands.front());
            if (symbol == m_symbols.end() || !added.insert(line.operands.front()).second) continue;
            m_object.symbol_table.push_back({
                std::string(1, InternalSymbolPrefix) + line.operands.front(),
                SymbolSection::CODE, static_cast<std::uint32_t>(symbol->second.offset)});
        }
    }
};

ObjectFile Assembler::assemble(const std::string& source) {
    return AssemblerImpl().assemble(source);
}
