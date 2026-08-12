#include "ObjectFile.hpp"
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>

void ObjectFile::write_string(std::ofstream& out, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.length());
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    out.write(s.c_str(), len);
}

std::uint64_t ObjectFile::remaining_bytes(std::istream& in) {
    const auto current = in.tellg();
    if (current < 0) throw std::runtime_error("Object file: unable to inspect stream position.");
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    in.seekg(current);
    if (end < current) throw std::runtime_error("Object file: invalid stream bounds.");
    return static_cast<std::uint64_t>(end - current);
}

std::string ObjectFile::read_string(std::istream& in, const char* field_name) {
    uint32_t len;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (!in) throw std::runtime_error(std::string("Object file: truncated ") + field_name + " length.");
    if (len > MaxStringBytes || static_cast<std::uint64_t>(len) > remaining_bytes(in)) {
        throw std::runtime_error(std::string("Object file: invalid ") + field_name + " length.");
    }
    std::string s;
    try {
        s.resize(len);
    } catch (const std::bad_alloc&) {
        throw std::runtime_error(std::string("Object file: unable to allocate ") + field_name + ".");
    }
    if (len > 0) in.read(&s[0], static_cast<std::streamsize>(len));
    if (!in) throw std::runtime_error(std::string("Object file: truncated ") + field_name + ".");
    return s;
}

template<typename T>
void ObjectFile::write_vec(std::ofstream& out, const std::vector<T>& vec) {
    uint32_t size = static_cast<uint32_t>(vec.size());
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    out.write(reinterpret_cast<const char*>(vec.data()), size * sizeof(T));
}

template<typename T>
void ObjectFile::read_vec(std::istream& in, std::vector<T>& vec,
                          std::uint32_t max_elements, const char* field_name) {
    uint32_t size;
    in.read(reinterpret_cast<char*>(&size), sizeof(size));
    if (!in) throw std::runtime_error(std::string("Object file: truncated ") + field_name + " size.");
    if (size > max_elements ||
        static_cast<std::uint64_t>(size) >
            std::numeric_limits<std::uint64_t>::max() / sizeof(T) ||
        static_cast<std::uint64_t>(size) * sizeof(T) > remaining_bytes(in)) {
        throw std::runtime_error(std::string("Object file: invalid ") + field_name + " size.");
    }
    try {
        vec.resize(size);
    } catch (const std::bad_alloc&) {
        throw std::runtime_error(std::string("Object file: unable to allocate ") + field_name + ".");
    }
    if (size > 0) {
        in.read(reinterpret_cast<char*>(vec.data()), static_cast<std::streamsize>(size * sizeof(T)));
        if (!in) throw std::runtime_error(std::string("Object file: truncated ") + field_name + ".");
    }
}


// Main I/O Methods

void ObjectFile::write(const std::string& path) {
    if (code_section.size() > MaxSectionBytes || data_section.size() > MaxSectionBytes) {
        throw std::runtime_error("Object file section exceeds the supported size limit.");
    }
    if (symbol_table.size() > MaxSymbolCount || relocation_table.size() > MaxRelocationCount) {
        throw std::runtime_error("Object file table exceeds the supported entry limit.");
    }
    for (const auto& symbol : symbol_table) {
        if (symbol.name.size() > MaxStringBytes) {
            throw std::runtime_error("Object file symbol name exceeds the supported size limit.");
        }
    }
    for (const auto& relocation : relocation_table) {
        if (relocation.target_symbol_name.size() > MaxStringBytes) {
            throw std::runtime_error("Object file relocation target exceeds the supported size limit.");
        }
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Failed to open object file for writing: " + path);

    // Magic Header
    out.write("DISCO", 5);
    const auto version = CurrentFormatVersion;
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));

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

    return read_stream(in, path);
}

ObjectFile ObjectFile::readBytes(const std::vector<std::uint8_t>& bytes) {
    std::string serialized;
    if (!bytes.empty()) {
        serialized.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    std::istringstream in(serialized, std::ios::in | std::ios::binary);
    return read_stream(in, "<memory>");
}

ObjectFile ObjectFile::read_stream(std::istream& in, const std::string& path) {

    ObjectFile obj;
    char magic[5] = {0};
    in.read(magic, 5);
    if (!in || std::string(magic, sizeof(magic)) != "DISCO") {
        throw std::runtime_error("File is not a valid DiscoC object file: " + path);
    }

    uint8_t version = 0;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!in || version != CurrentFormatVersion) {
        throw std::runtime_error("File has an unsupported DiscoC object format version: " + path);
    }

    uint8_t mapping = 0;
    in.read(reinterpret_cast<char*>(&mapping), sizeof(mapping));
    in.read(reinterpret_cast<char*>(&obj.config.code_start_address), sizeof(obj.config.code_start_address));
    if (!in || mapping > static_cast<uint8_t>(MemoryMapping::HiROM)) {
        throw std::runtime_error("File has an invalid target configuration: " + path);
    }
    obj.config.mapping = static_cast<MemoryMapping>(mapping);
    
    read_vec(in, obj.code_section, MaxSectionBytes, "code section");
    read_vec(in, obj.data_section, MaxSectionBytes, "data section");

    uint32_t sym_count;
    in.read(reinterpret_cast<char*>(&sym_count), sizeof(sym_count));
    if (!in || sym_count > MaxSymbolCount) {
        throw std::runtime_error("Object file has an invalid symbol count: " + path);
    }
    try {
        obj.symbol_table.resize(sym_count);
    } catch (const std::bad_alloc&) {
        throw std::runtime_error("Object file cannot allocate its symbol table: " + path);
    }
    for (uint32_t i = 0; i < sym_count; ++i) {
        obj.symbol_table[i].name = read_string(in, "symbol name");
        uint8_t section = 0;
        in.read(reinterpret_cast<char*>(&section), sizeof(section));
        if (!in || section > static_cast<uint8_t>(SymbolSection::DATA)) {
            throw std::runtime_error("Object file has an invalid symbol section: " + path);
        }
        obj.symbol_table[i].section = static_cast<SymbolSection>(section);
        in.read(reinterpret_cast<char*>(&obj.symbol_table[i].offset), sizeof(uint32_t));
        if (!in) throw std::runtime_error("Object file has a truncated symbol record: " + path);
        const auto section_size = obj.symbol_table[i].section == SymbolSection::CODE
            ? obj.code_section.size() : obj.data_section.size();
        if (obj.symbol_table[i].offset > section_size) {
            throw std::runtime_error("Object file has a symbol outside its section: " + path);
        }
    }

    uint32_t reloc_count;
    in.read(reinterpret_cast<char*>(&reloc_count), sizeof(reloc_count));
    if (!in || reloc_count > MaxRelocationCount) {
        throw std::runtime_error("Object file has an invalid relocation count: " + path);
    }
    try {
        obj.relocation_table.resize(reloc_count);
    } catch (const std::bad_alloc&) {
        throw std::runtime_error("Object file cannot allocate its relocation table: " + path);
    }
    for (uint32_t i = 0; i < reloc_count; ++i) {
        obj.relocation_table[i].target_symbol_name = read_string(in, "relocation target name");
        uint8_t section = 0;
        in.read(reinterpret_cast<char*>(&section), sizeof(section));
        if (!in || section > static_cast<uint8_t>(SymbolSection::DATA)) {
            throw std::runtime_error("Object file has an invalid relocation section: " + path);
        }
        obj.relocation_table[i].section_to_patch = static_cast<SymbolSection>(section);
        in.read(reinterpret_cast<char*>(&obj.relocation_table[i].patch_offset), sizeof(uint32_t));
        uint8_t type = 0;
        in.read(reinterpret_cast<char*>(&type), sizeof(type));
        if (!in || type > static_cast<uint8_t>(RelocationType::ADDR24_OFFSET)) {
            throw std::runtime_error("Object file has an invalid relocation type: " + path);
        }
        obj.relocation_table[i].type = static_cast<RelocationType>(type);
        validate_relocation(obj.relocation_table[i], obj);
    }

    if (in.peek() != std::istream::traits_type::eof()) {
        throw std::runtime_error("Object file has trailing data: " + path);
    }

    return obj;
}

void ObjectFile::validate_relocation(const RelocationEntry& relocation,
                                     const ObjectFile& object) {
    if (relocation.target_symbol_name.empty()) {
        throw std::runtime_error("Object file has a relocation without a target symbol.");
    }
    const auto& section = relocation.section_to_patch == SymbolSection::CODE
        ? object.code_section : object.data_section;
    const std::uint64_t patch_size = relocation.type == RelocationType::ADDR24_BANK ? 2u : 3u;
    if (static_cast<std::uint64_t>(relocation.patch_offset) + patch_size > section.size()) {
        throw std::runtime_error("Object file relocation extends beyond its section.");
    }
}
