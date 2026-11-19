#include "GSUAssemblerTarget.hpp"

#include "ABI.hpp"
#include "GSUCodeLayout.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace AsmSupport {
namespace {

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

int requireRegister(const std::string& operand, const ParsedLine& line) {
    const int reg = parseRegister(operand);
    if (reg < 0) fail("invalid register '" + operand + "'.", line.line_number);
    return reg;
}

int requireMemoryRegister(const std::string& operand, const ParsedLine& line) {
    const int reg = parseMemoryRegister(operand);
    if (reg < 0 || reg > 11) fail("invalid indirect register '" + operand + "'.", line.line_number);
    return reg;
}

uint16_t requireWord(const std::string& operand, const ParsedLine& line) {
    std::int64_t value = 0;
    if (!parseNumber(operand, value) || value < -32768 || value > 65535) {
        fail("expected a 16-bit numeric value, got '" + operand + "'.", line.line_number);
    }
    return static_cast<uint16_t>(value);
}

uint8_t requireByte(const std::string& operand, const ParsedLine& line) {
    std::int64_t value = 0;
    if (!parseNumber(operand, value) || value < 0 || value > 255) {
        fail("expected an 8-bit numeric value, got '" + operand + "'.", line.line_number);
    }
    return static_cast<uint8_t>(value);
}

const std::map<std::string, uint8_t>& branchOpcodes() {
    static const std::map<std::string, uint8_t> branches = {
        {"bra", 0x05}, {"bge", 0x06}, {"blt", 0x07}, {"bne", 0x08}, {"beq", 0x09},
        {"bpl", 0x0a}, {"bmi", 0x0b}, {"bcc", 0x0c}, {"bcs", 0x0d}, {"bvc", 0x0e}, {"bvs", 0x0f}
    };
    return branches;
}

class GSUTargetEncoder final : public TargetEncoder {
public:
    TargetKind target() const override { return TargetKind::GSU; }

    void parseDefine(const std::string& rest, int line_number) override {
        std::istringstream definition(rest);
        std::string name, value, extra;
        if (!(definition >> name >> value) || (definition >> extra)) {
            fail(".define expects one reserved configuration name and value.", line_number);
        }
        if (name != "__DISCO_MEMORY_MAPPING" && name != "__DISCO_CODE_START_ADDRESS" &&
            name != "__DISCO_DATA_ALIGNMENT" && name != "__DISCO_RAM_ALIGNMENT" && name != "__DISCO_CODE_ALIGNMENT" &&
            name != "__DISCO_BITMAP_SCBR" && name != "__DISCO_BITMAP_SCMR" && name != "__DISCO_STATIC_RAM_NONNULL" &&
            name != "__DISCO_STATIC_STACK_BOUND") {
            fail("unsupported .define name '" + name + "'.", line_number);
        }
        if (!m_config_definitions.insert(name).second) {
            fail("duplicate configuration definition '" + name + "'.", line_number);
        }
        if (name == "__DISCO_CODE_ALIGNMENT") {
            if (value != "16") fail("GSU CODE alignment hint must be 16.", line_number);
            m_cache_alignment = true;
        } else if (name == "__DISCO_STATIC_RAM_NONNULL") {
            if (value != "1") fail("Static-RAM proof marker must be 1.", line_number);
            m_static_ram_proof = true;
        } else if (name == "__DISCO_STATIC_STACK_BOUND") {
            std::int64_t number = 0;
            if (!parseNumber(value, number) || number < 1 || number > 65535)
                fail("Static stack bound must be 1..65535 bytes.", line_number);
            m_static_stack_bound = static_cast<std::uint32_t>(number);
        } else if (name == "__DISCO_BITMAP_SCBR" || name == "__DISCO_BITMAP_SCMR") {
            std::int64_t number = 0;
            if (!parseNumber(value, number) || number < 0 || number > 255) fail("Invalid bitmap register value.", line_number);
            m_config.bitmap.enabled = true;
            if (name == "__DISCO_BITMAP_SCBR") m_config.bitmap.base = static_cast<std::uint32_t>(number) << 10;
            else {
                if ((number & ~0x27) || (number & 3) == 2) fail("Invalid bitmap SCMR mode bits (ownership bits are host-owned).", line_number);
                m_config.bitmap.depth = (number & 3) == 3 ? 8 : (number & 3) == 1 ? 4 : 2;
                m_config.bitmap.object_mode = (number & 0x24) == 0x24;
                m_config.bitmap.height = m_config.bitmap.object_mode ? 256 : number & 0x20 ? 192 : number & 4 ? 160 : 128;
            }
        } else if (name == "__DISCO_MEMORY_MAPPING") {
            if (value == "lorom") m_config.mapping = MemoryMapping::LoROM;
            else if (value == "hirom") m_config.mapping = MemoryMapping::HiROM;
            else fail("memory mapping must be lorom or hirom.", line_number);
            if (m_config_definitions.count("__DISCO_CODE_START_ADDRESS") == 0) {
                m_config.code_start_address = m_config.mapping == MemoryMapping::LoROM
                    ? 0x008000u : 0x408000u;
            }
        } else if (name == "__DISCO_DATA_ALIGNMENT" || name == "__DISCO_RAM_ALIGNMENT") {
            std::int64_t alignment = 0;
            if (!parseNumber(value, alignment) || alignment < 1 || alignment > 128 || (alignment & (alignment - 1)))
                fail("Section alignment must be a power of two in 1..128.", line_number);
            if (name == "__DISCO_DATA_ALIGNMENT") m_data_alignment = static_cast<std::uint8_t>(alignment);
            else m_ram_alignment = static_cast<std::uint8_t>(alignment);
        } else {
            std::int64_t address = 0;
            if (!parseNumber(value, address) || address < 0 || address > 0xFFFFFF) {
                fail("code start address must fit in 24 bits.", line_number);
            }
            m_config.code_start_address = static_cast<std::uint32_t>(address);
        }
    }

    void validateDefinitions() override {
        if (m_config.bitmap.enabled && (m_config_definitions.count("__DISCO_BITMAP_SCBR") == 0 || m_config_definitions.count("__DISCO_BITMAP_SCMR") == 0))
            fail("Bitmap metadata requires both SCBR and SCMR definitions.", 1);
        m_config.bitmap.validate();
    }

    bool supportsSection(Section section) const override {
        return section == Section::Code || section == Section::Data || section == Section::Ram ||
               section == Section::ShortRam;
    }

    void validateLayout(std::size_t short_ram_size) const override {
        if (short_ram_size > GSUAbi::ShortRamCapacity) fail("short RAM image exceeds the LMS/SMS window.", 1);
    }

    bool shortBranch(const ParsedLine& line, std::string& target, std::size_t& short_size) const override {
        if (branchOpcodes().count(line.operation) == 0 || line.operands.size() != 1) return false;
        target = line.operands.front();
        short_size = 2;
        return true;
    }

    void configureObject(ObjectFile& object) const override {
        object.config = m_config;
        object.data_alignment = m_data_alignment;
        object.ram_alignment = m_ram_alignment;
        if (m_cache_alignment) object.symbol_table.push_back({GSUCodeLayout::CacheAlignmentSymbol, SymbolSection::CODE, 0});
        if (m_static_ram_proof) object.symbol_table.push_back({GSUAbi::StaticRamProofSymbol, SymbolSection::RAM, 0});
        if (m_static_stack_bound) object.symbol_table.push_back({GSUAbi::StaticStackBoundPrefix +
            std::to_string(m_static_stack_bound), SymbolSection::RAM, 0});
    }

    std::size_t encode(const ParsedLine& line, std::vector<uint8_t>* out, EncodeContext& context) override {
        const auto& op = line.operation;
        const auto branch = branchOpcodes().find(op);
        if (branch != branchOpcodes().end()) return encodeBranch(line, out, branch->second, context);

        static const std::map<std::string, uint8_t> implied = {
            {"stop", 0x00}, {"nop", 0x01}, {"cache", 0x02}, {"lsr", 0x03}, {"rol", 0x04}, {"loop", 0x3c},
            {"plot", 0x4c}, {"color", 0x4e}, {"swap", 0x4d}, {"not", 0x4f}, {"merge", 0x70},
            {"msbk", 0x90}, {"sex", 0x95}, {"asr", 0x96}, {"ror", 0x97}, {"lob", 0x9e}, {"hib", 0xc0},
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
            emitImmediateWord(line, out, 15, "#" + line.operands[0], RelocationType::ADDR16_JAL, context);
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
                const auto patch = out ? out->size() : 0;
                emitByte(out, static_cast<uint8_t>(0xa0 + reg));
                const auto operand = withoutHash(line.operands[1]);
                if (!operand.empty() && operand.front() == '^') {
                    const auto name = operand.substr(1);
                    if (!isIdentifier(name)) fail("invalid bank relocation symbol.", line.line_number);
                    emitByte(out, 0);
                    if (out) context.addRelocation({name, symbolSection(line.section),
                        static_cast<uint32_t>(patch), RelocationType::ADDR24_BANK});
                } else emitByte(out, requireByte(operand, line));
                return 2;
            }
            emitImmediateWord(line, out, reg, line.operands[1], RelocationType::ADDR16_IWT, context);
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
                emitByte(out, static_cast<uint8_t>(0x20 + source));
                emitByte(out, static_cast<uint8_t>(0x10 + destination));
                return 2;
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
                    emitImmediateWord(line, out, destination, line.operands[1], RelocationType::ADDR16_IWT, context);
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
            const bool store = op == "stw" || op == "stb";
            const bool byte = op == "stb" || op == "ldb";
            if (line.operands.size() == 1) {
                const int memory = requireMemoryRegister(line.operands.front(), line);
                if (byte) emitByte(out, 0x3d);
                emitByte(out, static_cast<uint8_t>((store ? 0x30 : 0x40) + memory));
                return byte ? 2 : 1;
            }
            requireOperands(line, 2);
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
            // `(short(name))`: the linker writes name's word index (address / 2).
            auto symbolic = address_token;
            if (symbolic.size() > 2 && symbolic.front() == '(' && symbolic.back() == ')')
                symbolic = symbolic.substr(1, symbolic.size() - 2);
            if (short_address && symbolic.size() > 7 && symbolic.compare(0, 6, "short(") == 0 && symbolic.back() == ')') {
                const auto name = symbolic.substr(6, symbolic.size() - 7);
                if (!isIdentifier(name)) fail("invalid short RAM relocation symbol.", line.line_number);
                if (line.section != Section::Code) fail("short RAM relocations are only valid in .segment \"CODE\".", line.line_number);
                emitByte(out, static_cast<uint8_t>(load ? 0x3d : 0x3e));
                const auto patch = out ? out->size() : 0;
                emitByte(out, static_cast<uint8_t>(0xa0 + reg));
                emitByte(out, 0);
                if (out) context.addRelocation({name, SymbolSection::CODE, static_cast<uint32_t>(patch),
                    RelocationType::ADDR8_SHORT_RAM});
                return 3;
            }
            const auto address = requireWord(symbolic, line);
            if (short_address) {
                if ((address & 1u) != 0 || address > 510) fail("short memory address must be even and <= 510.", line.line_number);
                emitByte(out, static_cast<uint8_t>(load ? 0x3d : 0x3e));
                emitByte(out, static_cast<uint8_t>(0xa0 + reg));
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

private:
    CompilerConfig m_config;
    std::uint8_t m_data_alignment = 1;
    std::uint8_t m_ram_alignment = 2;
    bool m_cache_alignment = false;
    bool m_static_ram_proof = false;
    std::uint32_t m_static_stack_bound = 0;
    std::set<std::string> m_config_definitions;

    static void emitImmediateWord(
        const ParsedLine& line,
        std::vector<uint8_t>* out,
        int reg,
        const std::string& operand,
        RelocationType relocation,
        EncodeContext& context) {
        const std::size_t patch = out ? out->size() : 0;
        emitByte(out, static_cast<uint8_t>(0xf0 | reg));
        std::int64_t value = 0;
        if (parseNumber(operand, value)) {
            if (value < -32768 || value > 65535) fail("word immediate is out of range.", line.line_number);
            emitWord(out, static_cast<uint16_t>(value));
        } else {
            auto name = withoutHash(operand);
            if (name.size() > 6 && name.compare(0, 5, "lo24(") == 0 && name.back() == ')') {
                name = name.substr(5, name.size() - 6);
                relocation = RelocationType::ADDR24_OFFSET;
            }
            if (name.size() > 5 && name.compare(0, 4, "ram(") == 0 && name.back() == ')') {
                name = name.substr(4, name.size() - 5);
                relocation = RelocationType::ADDR16_RAM;
            }
            if (!isIdentifier(name)) fail("invalid symbolic immediate '" + operand + "'.", line.line_number);
            emitWord(out, 0);
            if (out) context.addRelocation({name, symbolSection(line.section), static_cast<uint32_t>(patch), relocation});
        }
    }

    static std::size_t encodeBranch(const ParsedLine& line, std::vector<uint8_t>* out, uint8_t opcode, EncodeContext& context) {
        requireOperands(line, 1);
        if (line.section != Section::Code) fail("branches are only valid in .segment \"CODE\".", line.line_number);
        const auto patch = out ? out->size() : 0;
        if (context.isRelaxed(line.line_number)) {
            if (line.operation == "bra") {
                emitByte(out, 0xff); // IWT R15, absolute target
                emitWord(out, 0);
                if (out) context.addRelocation({
                    std::string(1, InternalSymbolPrefix) + line.operands.front(),
                    SymbolSection::CODE, static_cast<std::uint32_t>(patch),
                    RelocationType::ADDR16_IWT});
                emitByte(out, 0x01); // IWT R15 delay slot (0x9f is FMULT, not JMP).
                return 4;
            }

            static const std::map<uint8_t, uint8_t> inverse = {
                {0x06, 0x07}, {0x07, 0x06}, {0x08, 0x09}, {0x09, 0x08},
                {0x0a, 0x0b}, {0x0b, 0x0a}, {0x0c, 0x0d}, {0x0d, 0x0c},
                {0x0e, 0x0f}, {0x0f, 0x0e}};
            emitByte(out, inverse.at(opcode));
            emitByte(out, 5); // Skip NOP + IWT + its NOP delay slot.
            emitByte(out, 0x01); // Branch delay slot must not contain IWT.
            emitByte(out, 0xff);
            emitWord(out, 0);
            if (out) context.addRelocation({
                std::string(1, InternalSymbolPrefix) + line.operands.front(),
                SymbolSection::CODE, static_cast<std::uint32_t>(patch + 3),
                RelocationType::ADDR16_IWT});
            emitByte(out, 0x01);
            return 7;
        }
        emitByte(out, opcode);
        emitByte(out, 0);
        if (out) context.addBranchFixup(patch + 1, line.operands.front(), line.line_number);
        return 2;
    }

    static std::size_t encodeArithmetic(const ParsedLine& line, std::vector<uint8_t>* out) {
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
                return 2;
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
                if (dest != 0) emitByte(out, static_cast<uint8_t>(0x20 + dest));
                if (parsed_immediate < 0 || parsed_immediate > 15 || op == "cmp") fail("invalid two-operand immediate arithmetic form.", line.line_number);
                emitByte(out, 0x3e);
                emitByte(out, static_cast<uint8_t>(base + parsed_immediate));
                return 2 + (dest != 0 ? 1 : 0);
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
};

} // namespace

std::unique_ptr<TargetEncoder> makeGSUTargetEncoder() {
    return std::make_unique<GSUTargetEncoder>();
}

} // namespace AsmSupport
