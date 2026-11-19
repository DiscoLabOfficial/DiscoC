#include "Assembler.hpp"

#include "AssemblerSupport.hpp"
#include "AssemblerTarget.hpp"
#include "GSUAssemblerTarget.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace AsmSupport {

std::unique_ptr<TargetEncoder> makeTargetEncoder(TargetKind target) {
    switch (target) {
        case TargetKind::GSU: return makeGSUTargetEncoder();
        case TargetKind::SPC700: break;
    }
    fail("SPC700 assembly is not implemented yet.", 1);
}

} // namespace AsmSupport

namespace {

using namespace AsmSupport;

struct SymbolDefinition {
    SymbolSection section;
    std::size_t offset = 0;
};

struct BranchFixup {
    std::size_t patch_offset = 0;
    std::string target;
    int line_number = 0;
};

bool isDriverDirective(const std::string& operation) {
    return operation == ".segment" || operation == ".export" || operation == ".setcpu" ||
           operation == ".include" || operation == ".define";
}

} // namespace

// Target-neutral two-pass driver. Layout passes size every statement through
// the target encoder with a null output; the emission pass then produces the
// same sizes with bytes and relocations.
class AssemblerImpl final : private EncodeContext {
public:
    explicit AssemblerImpl(TargetKind target) : m_encoder(makeTargetEncoder(target)) {}

    ObjectFile assemble(const std::string& source) {
        m_lines = parse(source);
        m_encoder->validateDefinitions();
        firstPass();
        relaxBranches();
        secondPass();
        resolveBranches();
        addInternalSymbols();
        addExportedSymbols();
        return std::move(m_object);
    }

private:
    std::unique_ptr<TargetEncoder> m_encoder;
    std::vector<ParsedLine> m_lines;
    std::map<std::string, SymbolDefinition> m_symbols;
    std::set<std::string> m_exports;
    std::vector<BranchFixup> m_branch_fixups;
    ObjectFile m_object;
    std::size_t m_code_size = 0;
    std::size_t m_data_size = 0;
    std::size_t m_ram_size = 0;
    std::size_t m_short_ram_size = 0;
    std::set<int> m_relaxed_branch_lines;
    std::map<int, std::size_t> m_code_offsets_by_line;

    // EncodeContext
    bool isRelaxed(int line_number) const override {
        return m_relaxed_branch_lines.count(line_number) != 0;
    }
    void addRelocation(const RelocationEntry& relocation) override {
        m_object.relocation_table.push_back(relocation);
    }
    void addBranchFixup(std::size_t patch_offset, const std::string& target, int line_number) override {
        m_branch_fixups.push_back({patch_offset, target, line_number});
    }

    // Running byte offset of every section during a layout pass.
    struct SectionOffsets {
        std::size_t code = 0, data = 0, ram = 0, short_ram = 0;
        std::size_t& at(Section section) {
            return section == Section::Code ? code : section == Section::Data ? data : section == Section::Ram ? ram : short_ram;
        }
    };

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
            if (line.operation == ".define") m_encoder->parseDefine(rest, line_number);
            line.operands = splitOperands(rest);
            lines.push_back(line);

            if (line.operation == ".segment") {
                if (line.operands.size() != 1) fail(".segment expects one name.", line_number);
                const auto name = lower(line.operands.front());
                Section selected = Section::None;
                if (name == "\"code\"") selected = Section::Code;
                else if (name == "\"data\"") selected = Section::Data;
                else if (name == "\"ram\"") selected = Section::Ram;
                else if (name == "\"shortram\"") selected = Section::ShortRam;
                if (selected == Section::None || !m_encoder->supportsSection(selected))
                    fail("unsupported segment " + line.operands.front() + ".", line_number);
                current = selected;
            }
        }
        return lines;
    }

    void requireSection(const ParsedLine& line) const {
        if (line.section == Section::None) fail("statement appears before a .segment directive.", line.line_number);
    }

    void defineLabel(const ParsedLine& line, SectionOffsets& offsets) {
        if (line.label.empty()) return;
        requireSection(line);
        if (m_symbols.count(line.label)) fail("duplicate label '" + line.label + "'.", line.line_number);
        m_symbols.emplace(line.label, SymbolDefinition{symbolSection(line.section), offsets.at(line.section)});
    }

    void firstPass() {
        m_symbols.clear();
        m_code_offsets_by_line.clear();
        SectionOffsets offsets;
        for (const auto& line : m_lines) {
            defineLabel(line, offsets);
            if (line.operation.empty() || isDriverDirective(line.operation)) {
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
                m_code_offsets_by_line[line.line_number] = offsets.code;
            }
            if (line.operation == ".byte" || line.operation == ".word") {
                if (line.operands.empty()) fail(line.operation + " expects at least one value.", line.line_number);
                offsets.at(line.section) += line.operands.size() * (line.operation == ".byte" ? 1u : 2u);
            } else {
                if (line.section == Section::Ram || line.section == Section::ShortRam)
                    fail("RAM initial images contain data directives, not instructions.", line.line_number);
                offsets.at(line.section) += m_encoder->encode(line, nullptr, *this);
            }
        }
        m_code_size = offsets.code;
        m_data_size = offsets.data;
        if (offsets.ram > 65536u) fail("RAM initial image exceeds one bank.", 1);
        m_encoder->validateLayout(offsets.short_ram);
        m_ram_size = offsets.ram;
        m_short_ram_size = offsets.short_ram;
    }

    void relaxBranches() {
        for (;;) {
            bool changed = false;
            for (const auto& line : m_lines) {
                std::string target;
                std::size_t short_size = 0;
                if (line.section != Section::Code || line.operation.empty() ||
                    !m_encoder->shortBranch(line, target, short_size) ||
                    m_relaxed_branch_lines.count(line.line_number) != 0) {
                    continue;
                }
                const auto symbol = m_symbols.find(target);
                if (symbol == m_symbols.end()) {
                    fail("undefined branch label '" + target + "'.", line.line_number);
                }
                if (symbol->second.section != SymbolSection::CODE) {
                    fail("branch target must be in the code section.", line.line_number);
                }
                const auto source = m_code_offsets_by_line.at(line.line_number);
                const auto distance = static_cast<std::int64_t>(symbol->second.offset) -
                                      static_cast<std::int64_t>(source + short_size);
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
        m_encoder->configureObject(m_object);
        m_object.code_section.reserve(m_code_size);
        m_object.data_section.reserve(m_data_size);
        m_object.ram_section.reserve(m_ram_size);
        m_object.short_ram_section.reserve(m_short_ram_size);

        for (const auto& line : m_lines) {
            if (line.operation.empty() || isDriverDirective(line.operation)) continue;
            requireSection(line);
            if (line.operation == ".byte" || line.operation == ".word") {
                encodeData(line);
            } else {
                m_encoder->encode(line, output(line.section), *this);
            }
        }
    }

    std::vector<uint8_t>* output(Section section) {
        return section == Section::Code ? &m_object.code_section : section == Section::Data ? &m_object.data_section :
            section == Section::Ram ? &m_object.ram_section : &m_object.short_ram_section;
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
        // Absolute references to non-exported code labels must remain private
        // per object, just like the targets introduced by branch relaxation.
        for (auto& relocation : m_object.relocation_table) {
            const auto symbol = m_symbols.find(relocation.target_symbol_name);
            if (symbol == m_symbols.end() || m_exports.count(symbol->first) != 0) continue;
            const auto name = symbol->first;
            relocation.target_symbol_name = std::string(1, InternalSymbolPrefix) + name;
            if (added.insert(name).second) m_object.symbol_table.push_back({
                relocation.target_symbol_name, symbol->second.section,
                static_cast<uint32_t>(symbol->second.offset)});
        }
        for (const auto& line : m_lines) {
            if (m_relaxed_branch_lines.count(line.line_number) == 0) continue;
            std::string target;
            std::size_t short_size = 0;
            if (!m_encoder->shortBranch(line, target, short_size)) continue;
            const auto symbol = m_symbols.find(target);
            if (symbol == m_symbols.end() || !added.insert(target).second) continue;
            m_object.symbol_table.push_back({
                std::string(1, InternalSymbolPrefix) + target,
                SymbolSection::CODE, static_cast<std::uint32_t>(symbol->second.offset)});
        }
    }
};

ObjectFile Assembler::assemble(const std::string& source) {
    return AssemblerImpl(m_target).assemble(source);
}
