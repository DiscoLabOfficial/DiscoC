#include "AssemblyGenerator.hpp"
#include "GSUCodeLayout.hpp"
#include "ABI.hpp"
#include "SPC700Isa.hpp"
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
std::string hex(std::uint32_t value, int width = 2) {
    std::ostringstream out;
    out << '$' << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << value;
    return out.str();
}
std::string reg(unsigned value) { return "r" + std::to_string(value); }
struct Instruction { std::size_t size = 1; std::string text; };

// ALT changes the operation but not the operand length of IBT/LMS/SMS or
// IWT/LM/SM. Unknown encodings retain literal bytes rather than guessing.
Instruction decode(const std::vector<std::uint8_t>& code, std::size_t offset) {
    const auto op = code.at(offset);
    const unsigned n = op & 15u;
    Instruction result;
    if ((op >= 5 && op <= 15) || (op >= 0xa0 && op <= 0xaf)) result.size = 2;
    else if (op >= 0xf0) result.size = 3;
    if (result.size > code.size() - offset) throw std::runtime_error("Assembly export: truncated instruction.");
    static const std::map<unsigned, std::string> implied = {
        {0,"stop"},{1,"nop"},{2,"cache"},{3,"lsr"},{4,"rol"},
        {0x3c,"loop"},{0x3d,"alt1"},{0x3e,"alt2"},{0x3f,"alt3"},
        {0x4c,"plot"},{0x4d,"swap"},{0x4e,"color"},{0x4f,"not"},{0x70,"merge"},
        {0x90,"msbk"},{0x95,"sex"},{0x96,"asr"},{0x97,"ror"},{0x9e,"lob"},{0xc0,"hib"},
        {0xdf,"getc"},{0xef,"getb"}};
    const auto found = implied.find(op);
    if (found != implied.end()) result.text = found->second;
    else if (op >= 0x10 && op <= 0x1f) result.text = "to " + reg(n);
    else if (op >= 0x20 && op <= 0x2f) result.text = "with " + reg(n);
    else if (op >= 0x30 && op <= 0x3b) result.text = "stw (" + reg(n) + ")";
    else if (op >= 0x40 && op <= 0x4b) result.text = "ldw (" + reg(n) + ")";
    else if (op >= 0x50 && op <= 0x5f) result.text = "add " + reg(n);
    else if (op >= 0x60 && op <= 0x6f) result.text = "sub " + reg(n);
    else if (op >= 0x71 && op <= 0x7f) result.text = "and " + reg(n);
    else if (op >= 0x80 && op <= 0x8f) result.text = "mult " + reg(n);
    else if (op >= 0x91 && op <= 0x94) result.text = "link #" + std::to_string(op - 0x90);
    else if (op >= 0x98 && op <= 0x9d) result.text = "jmp " + reg(op - 0x98 + 8);
    else if (op >= 0xa0 && op <= 0xaf) result.text = "ibt " + reg(n) + ", #" + hex(code[offset + 1]);
    else if (op >= 0xb0 && op <= 0xbf) result.text = "from " + reg(n);
    else if (op >= 0xc1 && op <= 0xcf) result.text = "or " + reg(n);
    else if (op >= 0xd0 && op <= 0xde) result.text = "inc " + reg(n);
    else if (op >= 0xe0 && op <= 0xee) result.text = "dec " + reg(n);
    else if (op >= 0xf0) result.text = "iwt " + reg(n) + ", #" +
        hex(code[offset + 1] | (static_cast<std::uint32_t>(code[offset + 2]) << 8), 4);
    return result;
}

// Fuse only pairs with exact assembler expansions. Intervening selectors or
// unusual prefix combinations stay explicit to preserve the encoded stream.
Instruction decodeAlt(const std::vector<std::uint8_t>& code, std::size_t offset) {
    Instruction result;
    result.size = 2;
    if (offset + 1 >= code.size()) return result;
    const auto prefix = code[offset], op = code[offset + 1];
    const unsigned n = op & 15u;
    const bool a1 = prefix != 0x3e, a2 = prefix != 0x3d;
    if (op >= 0x50 && op <= 0x5f) result.text = std::string(a1 ? "adc " : "add ") +
        (a2 ? "#" + std::to_string(n) : reg(n));
    else if (op >= 0x60 && op <= 0x6f) result.text = a1 && a2 ? "cmp " + reg(n) :
        (a1 ? "sbc " + reg(n) : "sub #" + std::to_string(n));
    else if (op >= 0x80 && op <= 0x8f) result.text = std::string(a1 ? "umult " : "mult ") +
        (a2 ? "#" + std::to_string(n) : reg(n));
    else if (prefix == 0x3d && op >= 0x30 && op <= 0x3b) result.text = "stb (" + reg(n) + ")";
    else if (prefix == 0x3d && op >= 0x40 && op <= 0x4b) result.text = "ldb (" + reg(n) + ")";
    else if (op == 0xdf && prefix != 0x3d) result.text = a1 ? "romb" : "ramb";
    else if (prefix == 0x3d && op == 0x4c) result.text = "rpix";
    else if (prefix == 0x3d && op == 0x4e) result.text = "cmode";
    else if (prefix == 0x3d && op == 0x96) result.text = "div2";
    else if (op >= 0xa0 && op <= 0xaf && prefix != 0x3f && offset + 2 < code.size()) {
        result.size = 3;
        const auto address = hex(static_cast<std::uint32_t>(code[offset + 2]) * 2u, 3);
        result.text = prefix == 0x3d ? "lms " + reg(n) + ", (" + address + ")" : "sms (" + address + "), " + reg(n);
    }
    return result;
}
} // namespace

std::string AssemblyGenerator::generate() const {
    if (m_object.config.target == TargetKind::SPC700) return generateSPC700();
    if (m_object.config.target != TargetKind::GSU) throw std::runtime_error("Assembly export: unsupported target.");
    std::ostringstream out;
    out << "; Canonical GSU backend: byte-exact assembly export.\n"
           "; Relocatable symbols require discld; numeric linked addresses are fixed.\n"
           ".setcpu \"GSU\"\n.define __DISCO_MEMORY_MAPPING "
        << (m_object.config.mapping == MemoryMapping::LoROM ? "lorom" : "hirom")
        << "\n.define __DISCO_CODE_START_ADDRESS " << hex(m_object.config.code_start_address, 6)
        << "\n.define __DISCO_DATA_ALIGNMENT " << static_cast<unsigned>(m_object.data_alignment)
        << "\n.define __DISCO_RAM_ALIGNMENT " << static_cast<unsigned>(m_object.ram_alignment) << '\n';
    if (GSUCodeLayout::alignment(m_object) == 16) out << ".define __DISCO_CODE_ALIGNMENT 16\n";
    if (m_object.hasStaticRamProof()) out << ".define __DISCO_STATIC_RAM_NONNULL 1\n";
    const auto stack_bound = m_object.staticStackBound();
    if (stack_bound) out << ".define __DISCO_STATIC_STACK_BOUND " << stack_bound << '\n';
    m_object.config.bitmap.validate();
    if (m_object.config.bitmap.enabled) {
        out << "; SNES host writes SCBR and SCMR before starting GSU; OR SCMR with bus ownership bits.\n"
            << ".define __DISCO_BITMAP_SCBR " << hex(m_object.config.bitmap.scbr())
            << "\n.define __DISCO_BITMAP_SCMR " << hex(m_object.config.bitmap.scmr()) << '\n';
    }
    std::set<std::string> names;
    for (const auto& symbol : m_object.symbol_table) names.insert(symbol.name);
    std::map<std::string, std::string> renamed;
    std::map<std::size_t, std::vector<std::string>> code_labels, data_labels, ram_labels, short_labels;
    std::size_t serial = 0;
    const auto uniqueName = [&]() {
        std::string name;
        do { name = "__disco_local_" + std::to_string(serial++); } while (names.count(name));
        names.insert(name);
        return name;
    };
    for (const auto& symbol : m_object.symbol_table) {
        if (symbol.name.empty()) throw std::runtime_error("Assembly export: empty symbol name.");
        if (symbol.name == GSUCodeLayout::CacheAlignmentSymbol || symbol.name == GSUAbi::StaticRamProofSymbol) continue;
        if (stack_bound && symbol.name == GSUAbi::StaticStackBoundPrefix + std::to_string(stack_bound)) continue;
        const bool local = symbol.name.front() == '\x01';
        auto name = local ? uniqueName() : symbol.name;
        if (local && symbol.name.compare(0, std::string(GSUAbi::ImmutablePoolPrefix).size(), GSUAbi::ImmutablePoolPrefix) == 0) {
            // Keep the guarded relocation namespace through .s round trips.
            // The assembler reinstates the private marker for nonexports.
            name = symbol.name.substr(1);
            while (names.count(name)) name += "_";
            names.insert(name);
        }
        renamed.emplace(symbol.name, name);
        auto& labels = symbol.section == SymbolSection::CODE ? code_labels : symbol.section == SymbolSection::DATA ? data_labels :
            symbol.section == SymbolSection::RAM ? ram_labels : short_labels;
        const auto size = symbol.section == SymbolSection::CODE ? m_object.code_section.size() :
            symbol.section == SymbolSection::DATA ? m_object.data_section.size() :
            symbol.section == SymbolSection::RAM ? m_object.ram_section.size() : m_object.short_ram_section.size();
        if (symbol.offset > size) throw std::runtime_error("Assembly export: symbol outside section.");
        labels[symbol.offset].push_back(name);
        if (!local) out << ".export " << name << '\n';
    }
    std::map<std::size_t, const RelocationEntry*> relocations;
    for (const auto& relocation : m_object.relocation_table) {
        const std::size_t span = relocationSpan(relocation.type).extent();
        if (relocation.section_to_patch != SymbolSection::CODE || relocation.patch_offset > m_object.code_section.size() ||
            span > m_object.code_section.size() - relocation.patch_offset ||
            !relocations.emplace(relocation.patch_offset, &relocation).second) {
            throw std::runtime_error("Assembly export: unsupported or invalid relocation.");
        }
    }
    const auto& code = m_object.code_section;
    std::map<std::size_t, std::string> branches;
    for (std::size_t offset = 0; offset < code.size();) {
        const auto instruction = decode(code, offset);
        if (code[offset] >= 5 && code[offset] <= 15) {
            const int displacement = code[offset + 1] < 128 ? code[offset + 1] : static_cast<int>(code[offset + 1]) - 256;
            const auto target = static_cast<std::int64_t>(offset) + 2 + displacement;
            if (target < 0 || static_cast<std::uint64_t>(target) > code.size()) throw std::runtime_error("Assembly export: branch outside code.");
            const auto name = uniqueName();
            code_labels[static_cast<std::size_t>(target)].push_back(name);
            branches.emplace(offset, name);
        }
        offset += instruction.size;
    }
    const auto labelsAt = [&](const std::map<std::size_t, std::vector<std::string>>& labels, std::size_t offset) {
        const auto found = labels.find(offset);
        if (found != labels.end()) for (const auto& name : found->second) out << name << ":\n";
    };
    out << "\n.segment \"CODE\"\n";
    bool explicit_alt = false;
    for (std::size_t offset = 0; offset < code.size();) {
        labelsAt(code_labels, offset);
        auto instruction = decode(code, offset);
        const auto op = code[offset];
        auto relocation = relocations.find(offset);
        const auto short_relocation = op >= 0x3d && op <= 0x3e ? relocations.find(offset + 1) : relocations.end();
        if (!explicit_alt && short_relocation != relocations.end() &&
            short_relocation->second->type == RelocationType::ADDR8_SHORT_RAM && !code_labels.count(offset + 1)) {
            // LMS/SMS with a linker-written word index: one 3-byte statement.
            const auto found = renamed.find(short_relocation->second->target_symbol_name);
            const auto& name = found == renamed.end() ? short_relocation->second->target_symbol_name : found->second;
            const auto n = static_cast<unsigned>(code.at(offset + 1) & 15u);
            instruction.size = 3;
            instruction.text = op == 0x3d ? "lms " + reg(n) + ", (short(" + name + "))" : "sms (short(" + name + ")), " + reg(n);
            relocation = short_relocation;
        } else if (relocation != relocations.end()) {
            const auto& entry = *relocation->second;
            const auto found = renamed.find(entry.target_symbol_name);
            const auto& name = found == renamed.end() ? entry.target_symbol_name : found->second;
            if (entry.type == RelocationType::ADDR24_BANK && op >= 0xa0 && op <= 0xaf)
                instruction.text = "ibt " + reg(op & 15u) + ", #^" + name;
            else if (entry.type != RelocationType::ADDR24_BANK && op >= 0xf0)
                instruction.text = "iwt " + reg(op & 15u) + ", #" +
                    (entry.type == RelocationType::ADDR24_OFFSET ? "lo24(" + name + ")" : entry.type == RelocationType::ADDR16_RAM ? "ram(" + name + ")" : name);
            else throw std::runtime_error("Assembly export: relocation opcode mismatch.");
        } else if (branches.count(offset)) {
            static const char* const branch_names[] = {"bra","bge","blt","bne","beq","bpl","bmi","bcc","bcs","bvc","bvs"};
            instruction.text = std::string(branch_names[op - 5]) + " " + branches.at(offset);
        } else if (!explicit_alt && op >= 0x3d && op <= 0x3f && offset + 1 < code.size() &&
                   !code_labels.count(offset + 1) && !relocations.count(offset + 1)) {
            const auto combined = decodeAlt(code, offset);
            if (!combined.text.empty() && !(combined.size == 3 && code_labels.count(offset + 2))) instruction = combined;
        }
        const auto next_label = code_labels.upper_bound(offset);
        const auto next_relocation = relocations.upper_bound(relocation != relocations.end() ? relocation->first : offset);
        if ((next_label != code_labels.end() && next_label->first < offset + instruction.size) ||
            (next_relocation != relocations.end() && next_relocation->first < offset + instruction.size))
            throw std::runtime_error("Assembly export: label/relocation inside instruction.");
        out << "    ";
        // An explicit ALT must not be inserted a second time by reassembly.
        if (instruction.text.empty() || (explicit_alt && relocation == relocations.end() &&
            !(op >= 0x10 && op <= 0x2f) && !(op >= 0xb0 && op <= 0xbf) && !(op >= 5 && op <= 15))) {
            out << ".byte ";
            for (std::size_t i = 0; i < instruction.size; ++i) {
                if (i) out << ", ";
                out << hex(code[offset + i]);
            }
            out << " ; explicit encoding";
        } else out << instruction.text;
        out << " ; CODE+" << hex(static_cast<std::uint32_t>(offset), 4) << '\n';
        if (op >= 0x3d && op <= 0x3f && instruction.size == 1) explicit_alt = true;
        else if (!(op >= 0x10 && op <= 0x2f) && !(op >= 0xb0 && op <= 0xbf) && !(op >= 5 && op <= 15)) explicit_alt = false;
        offset += instruction.size;
    }
    labelsAt(code_labels, code.size());
    out << "\n.segment \"DATA\"\n";
    for (std::size_t offset = 0; offset < m_object.data_section.size(); ++offset) {
        labelsAt(data_labels, offset);
        out << "    .byte " << hex(m_object.data_section[offset]) << '\n';
    }
    labelsAt(data_labels, m_object.data_section.size());
    if (!m_object.ram_section.empty()) {
        out << "\n.segment \"RAM\"\n";
        for (std::size_t offset = 0; offset < m_object.ram_section.size(); ++offset) {
            labelsAt(ram_labels, offset);
            out << "    .byte " << hex(m_object.ram_section[offset]) << '\n';
        }
        labelsAt(ram_labels, m_object.ram_section.size());
    }
    if (!m_object.short_ram_section.empty() || !short_labels.empty()) {
        out << "\n.segment \"SHORTRAM\"\n";
        for (std::size_t offset = 0; offset < m_object.short_ram_section.size(); ++offset) {
            labelsAt(short_labels, offset);
            out << "    .byte " << hex(m_object.short_ram_section[offset]) << '\n';
        }
        labelsAt(short_labels, m_object.short_ram_section.size());
    }
    return out.str();
}

namespace {

using SPC700Isa::OperandPattern;

const char* registerName(OperandPattern pattern) {
    switch (pattern) {
        case OperandPattern::A: return "a";
        case OperandPattern::X: return "x";
        case OperandPattern::Y: return "y";
        case OperandPattern::YA: return "ya";
        case OperandPattern::SP: return "sp";
        case OperandPattern::PSW: return "psw";
        case OperandPattern::C: return "c";
        default: return nullptr;
    }
}

bool relocationFits(OperandPattern pattern, RelocationType type) {
    switch (pattern) {
        case OperandPattern::Imm: return type == RelocationType::SPC_IMM8_LO || type == RelocationType::SPC_IMM8_HI;
        case OperandPattern::Dp:
        case OperandPattern::DpX:
        case OperandPattern::DpY:
        case OperandPattern::DpXInd:
        case OperandPattern::DpIndY:
        case OperandPattern::DpBit: return type == RelocationType::SPC_DP8;
        case OperandPattern::Abs:
        case OperandPattern::AbsX:
        case OperandPattern::AbsY:
        case OperandPattern::AbsXInd: return type == RelocationType::SPC_ABS16;
        case OperandPattern::MemBit:
        case OperandPattern::NotMemBit: return type == RelocationType::SPC_MEMBIT13;
        default: return false;
    }
}

} // namespace

// SPC700 export uses the same opcode map as the encoder. Every listing
// reassembles to identical bytes: operands with relocations print the symbol,
// branch displacements print generated labels, and anything the map cannot
// express inside the section falls back to explicit .byte statements.
std::string AssemblyGenerator::generateSPC700() const {
    std::ostringstream out;
    out << "; SPC700 byte-exact assembly export.\n"
           "; Relocatable symbols require discld; numeric linked addresses are fixed.\n"
           ".setcpu \"SPC700\"\n"
        << ".define __DISCO_CODE_START_ADDRESS " << hex(m_object.config.code_start_address, 4)
        << "\n.define __DISCO_DATA_ALIGNMENT " << static_cast<unsigned>(m_object.data_alignment)
        << "\n.define __DISCO_RAM_ALIGNMENT " << static_cast<unsigned>(m_object.ram_alignment) << '\n';

    std::set<std::string> names;
    for (const auto& symbol : m_object.symbol_table) names.insert(symbol.name);
    std::map<std::string, std::string> renamed;
    std::map<std::size_t, std::vector<std::string>> code_labels, data_labels, ram_labels;
    std::size_t serial = 0;
    const auto uniqueName = [&]() {
        std::string name;
        do { name = "__disco_local_" + std::to_string(serial++); } while (names.count(name));
        names.insert(name);
        return name;
    };
    for (const auto& symbol : m_object.symbol_table) {
        if (symbol.name.empty()) throw std::runtime_error("Assembly export: empty symbol name.");
        if (symbol.section == SymbolSection::SHORT_RAM) throw std::runtime_error("Assembly export: short RAM requires GSU.");
        const bool local = symbol.name.front() == '\x01';
        const auto name = local ? uniqueName() : symbol.name;
        renamed.emplace(symbol.name, name);
        auto& labels = symbol.section == SymbolSection::CODE ? code_labels :
            symbol.section == SymbolSection::DATA ? data_labels : ram_labels;
        const auto size = symbol.section == SymbolSection::CODE ? m_object.code_section.size() :
            symbol.section == SymbolSection::DATA ? m_object.data_section.size() : m_object.ram_section.size();
        if (symbol.offset > size) throw std::runtime_error("Assembly export: symbol outside section.");
        labels[symbol.offset].push_back(name);
        if (!local) out << ".export " << name << '\n';
    }
    const auto& code = m_object.code_section;
    std::map<std::size_t, const RelocationEntry*> relocations;
    for (const auto& relocation : m_object.relocation_table) {
        const auto span = relocationSpan(relocation.type);
        if (relocation.section_to_patch != SymbolSection::CODE || !isSPC700Relocation(relocation.type) ||
            relocation.patch_offset > code.size() || span.extent() > code.size() - relocation.patch_offset ||
            !relocations.emplace(relocation.patch_offset, &relocation).second) {
            throw std::runtime_error("Assembly export: unsupported or invalid relocation.");
        }
    }
    const auto symbolName = [&](const RelocationEntry& entry) {
        const auto found = renamed.find(entry.target_symbol_name);
        return found == renamed.end() ? entry.target_symbol_name : found->second;
    };

    // Pass 1: instruction boundaries and branch-target labels. A trailing
    // partial instruction is emitted byte by byte.
    std::map<std::size_t, std::string> branch_labels;
    for (std::size_t offset = 0; offset < code.size();) {
        const auto& info = SPC700Isa::instruction(code[offset]);
        if (info.size > code.size() - offset) break;
        const auto count = info.operands[0] == OperandPattern::None ? 0u : info.operands[1] == OperandPattern::None ? 1u : 2u;
        if (count && info.operands[count - 1] == OperandPattern::Rel && !relocations.count(offset + info.size - 1)) {
            const auto byte = code[offset + info.size - 1];
            const int displacement = byte < 128 ? byte : static_cast<int>(byte) - 256;
            const auto target = static_cast<std::int64_t>(offset + info.size) + displacement;
            if (target < 0 || static_cast<std::uint64_t>(target) > code.size())
                throw std::runtime_error("Assembly export: branch outside code.");
            const auto name = uniqueName();
            code_labels[static_cast<std::size_t>(target)].push_back(name);
            branch_labels.emplace(offset, name);
        }
        offset += info.size;
    }

    const auto labelsAt = [&](const std::map<std::size_t, std::vector<std::string>>& labels, std::size_t offset) {
        const auto found = labels.find(offset);
        if (found != labels.end()) for (const auto& name : found->second) out << name << ":\n";
    };

    out << "\n.segment \"CODE\"\n";
    for (std::size_t offset = 0; offset < code.size();) {
        labelsAt(code_labels, offset);
        const auto& info = SPC700Isa::instruction(code[offset]);
        if (info.size > code.size() - offset) {
            if (relocations.count(offset)) throw std::runtime_error("Assembly export: relocation inside instruction.");
            out << "    .byte " << hex(code[offset]) << " ; explicit encoding ; CODE+" << hex(static_cast<std::uint32_t>(offset), 4) << '\n';
            ++offset;
            continue;
        }
        const auto next_label = code_labels.upper_bound(offset);
        if (next_label != code_labels.end() && next_label->first < offset + info.size)
            throw std::runtime_error("Assembly export: label inside instruction.");

        const auto count = info.operands[0] == OperandPattern::None ? 0u : info.operands[1] == OperandPattern::None ? 1u : 2u;
        // Byte position of each source operand after the opcode.
        std::size_t position[2] = {offset + 1, offset + 1};
        if (count == 2) {
            const auto first = SPC700Isa::operandBytes(info.operands[0]);
            const auto second = SPC700Isa::operandBytes(info.operands[1]);
            if (info.layout == SPC700Isa::OperandLayout::Reversed) position[0] = offset + 1 + second;
            else position[1] = offset + 1 + first;
        }
        std::size_t used_relocations = 0;
        std::string text = info.mnemonic;
        for (std::size_t index = 0; index < count; ++index) {
            const auto pattern = info.operands[index];
            const auto at = position[index];
            const RelocationEntry* relocation = nullptr;
            if (SPC700Isa::operandBytes(pattern) != 0) {
                const auto found = relocations.find(at);
                if (found != relocations.end()) {
                    if (!relocationFits(pattern, found->second->type))
                        throw std::runtime_error("Assembly export: relocation does not match its operand.");
                    relocation = found->second;
                    ++used_relocations;
                }
            }
            const auto byte = [&]() { return relocation ? symbolName(*relocation) : hex(code[at]); };
            const auto word = [&]() {
                return relocation ? "!" + symbolName(*relocation) :
                    "!" + hex(code[at] | (static_cast<std::uint32_t>(code[at + 1]) << 8), 4);
            };
            std::string operand;
            if (const char* name = registerName(pattern)) operand = name;
            else switch (pattern) {
                case OperandPattern::Imm:
                    operand = relocation ? std::string(relocation->type == RelocationType::SPC_IMM8_LO ? "#<" : "#>") +
                        symbolName(*relocation) : "#" + hex(code[at]);
                    break;
                case OperandPattern::Dp: operand = byte(); break;
                case OperandPattern::DpX: operand = byte() + "+x"; break;
                case OperandPattern::DpY: operand = byte() + "+y"; break;
                case OperandPattern::Abs: operand = word(); break;
                case OperandPattern::AbsX: operand = word() + "+x"; break;
                case OperandPattern::AbsY: operand = word() + "+y"; break;
                case OperandPattern::IndX: operand = "(x)"; break;
                case OperandPattern::IndXInc: operand = "(x)+"; break;
                case OperandPattern::IndY: operand = "(y)"; break;
                case OperandPattern::DpXInd: operand = "[" + byte() + "+x]"; break;
                case OperandPattern::DpIndY: operand = "[" + byte() + "]+y"; break;
                case OperandPattern::AbsXInd: operand = "[" + word() + "+x]"; break;
                case OperandPattern::DpBit: operand = byte() + "." + std::to_string(info.fixed); break;
                case OperandPattern::MemBit:
                case OperandPattern::NotMemBit: {
                    const auto value = code[at] | (static_cast<std::uint32_t>(code[at + 1]) << 8);
                    if (relocation && (value & 0x1fffu) != 0)
                        throw std::runtime_error("Assembly export: mem.bit relocation has a nonzero address addend.");
                    operand = (pattern == OperandPattern::NotMemBit ? "/" : "") +
                        (relocation ? symbolName(*relocation) : hex(value & 0x1fffu, 4)) + "." + std::to_string(value >> 13);
                    break;
                }
                case OperandPattern::Rel: operand = branch_labels.at(offset); break;
                case OperandPattern::Upage: operand = hex(code[at]); break;
                case OperandPattern::TcallN: operand = std::to_string(info.fixed); break;
                default: throw std::runtime_error("Assembly export: unknown SPC700 operand.");
            }
            text += (index == 0 ? " " : ",") + operand;
        }
        // Every relocation must start at an operand of this instruction.
        const auto first_relocation = relocations.lower_bound(offset);
        std::size_t inside = 0;
        for (auto it = first_relocation; it != relocations.end() && it->first < offset + info.size; ++it) ++inside;
        if (inside != used_relocations) throw std::runtime_error("Assembly export: relocation inside instruction.");
        // A nonzero placeholder under a relocation would be lost on reassembly.
        for (auto it = first_relocation; it != relocations.end() && it->first < offset + info.size; ++it) {
            const auto span = relocationSpan(it->second->type);
            const bool keeps_bit = it->second->type == RelocationType::SPC_MEMBIT13;
            for (std::size_t i = 0; i < span.operand_bytes; ++i) {
                const auto value = code[it->first + i] & (keeps_bit && i == 1 ? 0x1fu : 0xffu);
                if (value != 0) throw std::runtime_error("Assembly export: relocation placeholder is not zero.");
            }
        }
        out << "    " << text << " ; CODE+" << hex(static_cast<std::uint32_t>(offset), 4) << '\n';
        offset += info.size;
    }
    labelsAt(code_labels, code.size());
    out << "\n.segment \"DATA\"\n";
    for (std::size_t offset = 0; offset < m_object.data_section.size(); ++offset) {
        labelsAt(data_labels, offset);
        out << "    .byte " << hex(m_object.data_section[offset]) << '\n';
    }
    labelsAt(data_labels, m_object.data_section.size());
    if (!m_object.ram_section.empty() || !ram_labels.empty()) {
        out << "\n.segment \"RAM\"\n";
        for (std::size_t offset = 0; offset < m_object.ram_section.size(); ++offset) {
            labelsAt(ram_labels, offset);
            out << "    .byte " << hex(m_object.ram_section[offset]) << '\n';
        }
        labelsAt(ram_labels, m_object.ram_section.size());
    }
    return out.str();
}
