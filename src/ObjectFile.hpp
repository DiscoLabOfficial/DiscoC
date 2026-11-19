#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <map>
#include <fstream>
#include <istream>
#include <ostream>
#include "TargetConfig.hpp"

// The type of relocation required.
enum class RelocationType : uint8_t {
    ADDR16_JAL,     // A 16-bit address for a JAL pseudo-op (IWT R15)
    ADDR16_IWT,     // A 16-bit address for a general IWT instruction
    ADDR24_BANK,    // The 8-bit bank byte of a 24-bit far address
    ADDR24_OFFSET,  // The 16-bit offset of a 24-bit far address
    ADDR16_RAM,    // RAM static-storage offset, independent of the program bank
    ADDR8_SHORT_RAM // LMS/SMS operand: (even near RAM address < $0200) / 2
};

// Operand bytes a relocation rewrites, relative to RelocationEntry::patch_offset.
// GSU relocation types record the opcode position and patch the operand that
// immediately follows it. Every reader, writer, linker and exporter derives
// relocation geometry from this one function. Callers validate `type` before
// asking for its span.
struct RelocationSpan {
    std::uint32_t operand_offset; // First patched byte, relative to patch_offset.
    std::uint32_t operand_bytes;  // Number of patched bytes.

    // Bytes from patch_offset through the last patched byte.
    constexpr std::uint32_t extent() const { return operand_offset + operand_bytes; }
};

constexpr RelocationSpan relocationSpan(RelocationType type) {
    switch (type) {
        case RelocationType::ADDR24_BANK:
        case RelocationType::ADDR8_SHORT_RAM:
            return RelocationSpan{1, 1};
        case RelocationType::ADDR16_JAL:
        case RelocationType::ADDR16_IWT:
        case RelocationType::ADDR24_OFFSET:
        case RelocationType::ADDR16_RAM:
            break;
    }
    return RelocationSpan{1, 2};
}

// Which section the symbol or relocation belongs to.
enum class SymbolSection : uint8_t {
    CODE,
    DATA,
    RAM,
    SHORT_RAM // Static RAM linked inside the LMS/SMS window (GSUAbi::ShortRamBase..End)
};

// Represents a symbol defined in this object file.
struct SymbolEntry {
    std::string name;
    SymbolSection section;
    uint32_t offset; // Offset within its section.
};

// Represents a location that needs to be patched by the linker.
struct RelocationEntry {
    std::string target_symbol_name;
    SymbolSection section_to_patch; // The section containing the placeholder
    uint32_t patch_offset;          // The offset within that section
    RelocationType type;
};

// Represents the entire contents of a .o file.
class ObjectFile {
public:
    // Newest readable version. Objects are written as BaselineFormatVersion
    // unless they carry a proof marker: version 8 for
    // GSUAbi::StaticRamProofSymbol, version 9 for a GSUAbi::StaticStackBoundPrefix
    // marker, version 10 for a short RAM section, SHORT_RAM symbols or
    // ADDR8_SHORT_RAM relocations. A linker that cannot enforce a marker or
    // place short RAM must refuse the object rather than ignore it.
    static constexpr std::uint8_t CurrentFormatVersion = 10;
    static constexpr std::uint8_t ShortRamFormatVersion = 10;
    static constexpr std::uint8_t StaticStackFormatVersion = 9;
    static constexpr std::uint8_t StaticRamProofFormatVersion = 8;
    static constexpr std::uint8_t BaselineFormatVersion = 7;
    bool hasStaticRamProof() const;
    // Bytes the static stack-bound marker requires above the stack floor at
    // `main`'s entry, or 0 when the object carries no such marker.
    std::uint32_t staticStackBound() const;
    bool usesShortRam() const;
    static constexpr std::uint32_t MaxSectionBytes = 64u * 1024u * 1024u;
    static constexpr std::uint32_t MaxObjectBytes = 128u * 1024u * 1024u;
    static constexpr std::uint32_t MaxStringBytes = 4096u;
    static constexpr std::uint32_t MaxSymbolCount = 1'000'000u;
    static constexpr std::uint32_t MaxRelocationCount = 1'000'000u;

    ObjectFile() = default;

    // Data members
    std::vector<uint8_t> code_section;
    std::vector<uint8_t> data_section;
    std::vector<uint8_t> ram_section; // Initial image of separately allocated static RAM, not payload DATA.
    // Initial image of static RAM the linker places in the short RAM window;
    // at most GSUAbi::ShortRamCapacity bytes, word-aligned like RAM.
    std::vector<uint8_t> short_ram_section;
    std::vector<SymbolEntry> symbol_table;
    std::vector<RelocationEntry> relocation_table;
    CompilerConfig config;
    std::uint8_t data_alignment = 1; // Version 3 objects implicitly use packed DATA.
    std::uint8_t ram_alignment = 2; // Versions <=5 use word-aligned RAM.
    
    // Serialization / Deserialization
    void write(const std::string& path);
    static ObjectFile read(const std::string& path);
    static ObjectFile readBytes(const std::vector<std::uint8_t>& bytes);

private:
    // Helpers for binary I/O
    static void write_u32_le(std::ostream& out, std::uint32_t value);
    static std::uint32_t read_u32_le(std::istream& in, const char* field_name);
    static void write_u8(std::ostream& out, std::uint8_t value);
    static std::uint8_t read_u8(std::istream& in, const char* field_name);
    static void write_string(std::ostream& out, const std::string& s);
    static std::string read_string(std::istream& in, const char* field_name);
    template<typename T>
    static void write_vec(std::ofstream& out, const std::vector<T>& vec);
    template<typename T>
    static void read_vec(std::istream& in, std::vector<T>& vec,
                         std::uint32_t max_elements, const char* field_name);
    static std::uint64_t remaining_bytes(std::istream& in);
    static ObjectFile read_stream(std::istream& in, const std::string& source_name);
    void validate() const;
    static void validate_relocation(const RelocationEntry& relocation,
                                    const ObjectFile& object);
};
