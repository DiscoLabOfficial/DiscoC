#include "ObjectFile.hpp"
#include <stdexcept>

void ObjectFile::write_string(std::ofstream& out, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.length());
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    out.write(s.c_str(), len);
}

std::string ObjectFile::read_string(std::ifstream& in) {
    uint32_t len;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (!in) return "";
    std::string s(len, '\0');
    in.read(&s[0], len);
    return s;
}

template<typename T>
void ObjectFile::write_vec(std::ofstream& out, const std::vector<T>& vec) {
    uint32_t size = static_cast<uint32_t>(vec.size());
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    out.write(reinterpret_cast<const char*>(vec.data()), size * sizeof(T));
}

template<typename T>
void ObjectFile::read_vec(std::ifstream& in, std::vector<T>& vec) {
    uint32_t size;
    in.read(reinterpret_cast<char*>(&size), sizeof(size));
    if (!in) return;
    vec.resize(size);
    in.read(reinterpret_cast<char*>(vec.data()), size * sizeof(T));
}


// Main I/O Methods

void ObjectFile::write(const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Failed to open object file for writing: " + path);

    // Magic Header
    out.write("DISCO", 5);

    const auto mapping = static_cast<uint8_t>(config.mapping);
    out.write(reinterpret_cast<const char*>(&mapping), sizeof(mapping));
    out.write(reinterpret_cast<const char*>(&config.code_start_address), sizeof(config.code_start_address));

    // Write sections
    write_vec(out, code_section);
    write_vec(out, data_section);

    // Write symbol table
    uint32_t sym_count = static_cast<uint32_t>(symbol_table.size());
    out.write(reinterpret_cast<const char*>(&sym_count), sizeof(sym_count));
    for (const auto& sym : symbol_table) {
        write_string(out, sym.name);
        out.write(reinterpret_cast<const char*>(&sym.section), sizeof(sym.section));
        out.write(reinterpret_cast<const char*>(&sym.offset), sizeof(sym.offset));
    }

    // Write relocation table
    uint32_t reloc_count = static_cast<uint32_t>(relocation_table.size());
    out.write(reinterpret_cast<const char*>(&reloc_count), sizeof(reloc_count));
    for (const auto& reloc : relocation_table) {
        write_string(out, reloc.target_symbol_name);
        out.write(reinterpret_cast<const char*>(&reloc.section_to_patch), sizeof(reloc.section_to_patch));
        out.write(reinterpret_cast<const char*>(&reloc.patch_offset), sizeof(reloc.patch_offset));
        out.write(reinterpret_cast<const char*>(&reloc.type), sizeof(reloc.type));
    }
}

ObjectFile ObjectFile::read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Failed to open object file for reading: " + path);

    ObjectFile obj;
    char magic[6] = {0};
    in.read(magic, 5);
    if (std::string(magic) != "DISCO") {
        throw std::runtime_error("File is not a valid DiscoC object file: " + path);
    }

    uint8_t mapping = 0;
    in.read(reinterpret_cast<char*>(&mapping), sizeof(mapping));
    in.read(reinterpret_cast<char*>(&obj.config.code_start_address), sizeof(obj.config.code_start_address));
    if (!in || mapping > static_cast<uint8_t>(MemoryMapping::HiROM)) {
        throw std::runtime_error("File has an invalid target configuration: " + path);
    }
    obj.config.mapping = static_cast<MemoryMapping>(mapping);
    
    read_vec(in, obj.code_section);
    read_vec(in, obj.data_section);

    uint32_t sym_count;
    in.read(reinterpret_cast<char*>(&sym_count), sizeof(sym_count));
    obj.symbol_table.resize(sym_count);
    for (uint32_t i = 0; i < sym_count; ++i) {
        obj.symbol_table[i].name = read_string(in);
        in.read(reinterpret_cast<char*>(&obj.symbol_table[i].section), sizeof(SymbolSection));
        in.read(reinterpret_cast<char*>(&obj.symbol_table[i].offset), sizeof(uint32_t));
    }

    uint32_t reloc_count;
    in.read(reinterpret_cast<char*>(&reloc_count), sizeof(reloc_count));
    obj.relocation_table.resize(reloc_count);
    for (uint32_t i = 0; i < reloc_count; ++i) {
        obj.relocation_table[i].target_symbol_name = read_string(in);
        in.read(reinterpret_cast<char*>(&obj.relocation_table[i].section_to_patch), sizeof(SymbolSection));
        in.read(reinterpret_cast<char*>(&obj.relocation_table[i].patch_offset), sizeof(uint32_t));
        in.read(reinterpret_cast<char*>(&obj.relocation_table[i].type), sizeof(RelocationType));
    }

    return obj;
}
