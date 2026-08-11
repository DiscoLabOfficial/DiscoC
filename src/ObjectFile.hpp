#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <map>
#include <fstream>
#include "TargetConfig.hpp"

// The type of relocation required.
enum class RelocationType : uint8_t {
    ADDR16_JAL,     // A 16-bit address for a JAL pseudo-op (IWT R15)
    ADDR16_IWT,     // A 16-bit address for a general IWT instruction
    ADDR24_BANK,    // The 8-bit bank byte of a 24-bit far address
    ADDR24_OFFSET   // The 16-bit offset of a 24-bit far address
};

// Which section the symbol or relocation belongs to.
enum class SymbolSection : uint8_t {
    CODE,
    DATA
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
    ObjectFile() = default;

    // Data members
    std::vector<uint8_t> code_section;
    std::vector<uint8_t> data_section;
    std::vector<SymbolEntry> symbol_table;
    std::vector<RelocationEntry> relocation_table;
    CompilerConfig config;
    
    // Serialization / Deserialization
    void write(const std::string& path);
    static ObjectFile read(const std::string& path);

private:
    // Helpers for binary I/O
    static void write_string(std::ofstream& out, const std::string& s);
    static std::string read_string(std::ifstream& in);
    template<typename T>
    static void write_vec(std::ofstream& out, const std::vector<T>& vec);
    template<typename T>
    static void read_vec(std::ifstream& in, std::vector<T>& vec);
};
