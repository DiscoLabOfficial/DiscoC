#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <fstream>
#include <iomanip>
#include <limits>
#include <algorithm>
#include "ObjectFile.hpp"
#include "Parser.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: discld [options] <file1.o> <file2.o> ... -o <rom.bin>" << std::endl;
        return 1;
    }
    
    std::vector<std::string> object_files;
    std::string out_filepath = "new.bin";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o") {
            if (i + 1 < argc) out_filepath = argv[++i];
            else { std::cerr << "Error: -o requires a filename." << std::endl; return 1; }
        } else {
            object_files.push_back(arg);
        }
    }
    
    if (object_files.empty()) {
        std::cerr << "Error: No input object files specified." << std::endl;
        return 1;
    }
    
    std::cout << "DiscoC Linker: Linking " << object_files.size() << " object file(s) -> " << out_filepath << std::endl;

    try {
        const auto checkedAddress = [](std::uint64_t value, const char* what) -> std::uint32_t {
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error(std::string("Linker Error: ") + what + " exceeds the address range.");
            }
            return static_cast<std::uint32_t>(value);
        };

        // --- 1. Load all object files ---
        std::vector<ObjectFile> objects;
        for (const auto& path : object_files) {
            objects.push_back(ObjectFile::read(path));
        }

        const CompilerConfig& config = objects.front().config;
        for (std::size_t index = 1; index < objects.size(); ++index) {
            if (objects[index].config != config) {
                throw std::runtime_error(
                    "Input objects use incompatible target configurations.");
            }
        }

        // --- 2. Layout and Symbol Resolution ---
        std::vector<uint8_t> final_code;
        std::vector<uint8_t> final_data;
        std::map<std::string, uint32_t> final_addresses;

        std::uint64_t current_code_offset = 0;
        for (const auto& obj : objects) {
            // Process CODE symbols
            for (const auto& sym : obj.symbol_table) {
                if (sym.section == SymbolSection::CODE) {
                    if (!sym.name.empty() && sym.name.front() == '\x01') continue;
                    if (final_addresses.count(sym.name)) {
                        throw std::runtime_error("Linker Error: Symbol '" + sym.name + "' defined multiple times.");
                    }
                    final_addresses[sym.name] = checkedAddress(
                        static_cast<std::uint64_t>(config.code_start_address) +
                            current_code_offset + sym.offset,
                        "code symbol address");
                }
            }
            final_code.insert(final_code.end(), obj.code_section.begin(), obj.code_section.end());
            current_code_offset += obj.code_section.size();
        }
        
        const uint32_t data_start_address = checkedAddress(
            static_cast<std::uint64_t>(config.code_start_address) + current_code_offset,
            "data section address");
        std::uint64_t current_data_offset = 0;
        for (const auto& obj : objects) {
            // Process DATA symbols
            for (const auto& sym : obj.symbol_table) {
                if (sym.section == SymbolSection::DATA) {
                    if (!sym.name.empty() && sym.name.front() == '\x01') continue;
                    if (final_addresses.count(sym.name)) {
                        throw std::runtime_error("Linker Error: Symbol '" + sym.name + "' defined multiple times.");
                    }
                    final_addresses[sym.name] = checkedAddress(
                        static_cast<std::uint64_t>(data_start_address) +
                            current_data_offset + sym.offset,
                        "data symbol address");
                }
            }
            final_data.insert(final_data.end(), obj.data_section.begin(), obj.data_section.end());
            current_data_offset += obj.data_section.size();
        }

        // --- 3. Relocation (Patching) ---
        std::uint64_t current_code_base = 0;
        std::uint64_t current_data_base = 0;

        for (const auto& obj : objects) {
            for (const auto& reloc : obj.relocation_table) {
                std::uint32_t target_addr = 0;
                if (!reloc.target_symbol_name.empty() && reloc.target_symbol_name.front() == '\x01') {
                    const auto local_name = reloc.target_symbol_name.substr(1);
                    const auto local_symbol = std::find_if(
                        obj.symbol_table.begin(), obj.symbol_table.end(),
                        [&](const auto& symbol) {
                            return symbol.name == reloc.target_symbol_name &&
                                   symbol.section == SymbolSection::CODE;
                        });
                    if (local_symbol == obj.symbol_table.end()) {
                        throw std::runtime_error("Linker Error: Undefined local symbol '" + local_name + "'.");
                    }
                    target_addr = checkedAddress(
                        static_cast<std::uint64_t>(config.code_start_address) +
                            current_code_base + local_symbol->offset,
                        "local branch target address");
                } else {
                    if (final_addresses.find(reloc.target_symbol_name) == final_addresses.end()) {
                        throw std::runtime_error("Linker Error: Undefined symbol '" + reloc.target_symbol_name + "'.");
                    }
                    target_addr = final_addresses.at(reloc.target_symbol_name);
                }
                std::vector<uint8_t>& section_to_patch =
                    reloc.section_to_patch == SymbolSection::CODE ? final_code : final_data;
                const auto section_base = reloc.section_to_patch == SymbolSection::CODE
                    ? current_code_base : current_data_base;
                const auto offset_in_section = section_base + reloc.patch_offset;
                const auto patch_size = reloc.type == RelocationType::ADDR24_BANK ? 2u : 3u;
                if (static_cast<std::uint64_t>(offset_in_section) + patch_size > section_to_patch.size()) {
                    throw std::runtime_error("Linker Error: relocation extends beyond its section.");
                }
                const auto patch_index = static_cast<std::size_t>(offset_in_section);

                switch (reloc.type) {
                    case RelocationType::ADDR16_JAL:
                    case RelocationType::ADDR16_IWT:
                    case RelocationType::ADDR24_OFFSET:
                        section_to_patch[patch_index + 1] = target_addr & 0xFF;
                        section_to_patch[patch_index + 2] = (target_addr >> 8) & 0xFF;
                        break;
                    case RelocationType::ADDR24_BANK:
                        section_to_patch[patch_index + 1] = (target_addr >> 16) & 0xFF;
                        break;
                }
            }
            current_code_base += obj.code_section.size();
            current_data_base += obj.data_section.size();
        }

        // --- 4. Final Assembly & Output ---
        std::vector<uint8_t> final_rom = final_code;
        final_rom.insert(final_rom.end(), final_data.begin(), final_data.end());

        std::ofstream outFile(out_filepath, std::ios::out | std::ios::binary);
        if (!outFile) throw std::runtime_error("Failed to open output file for writing: " + out_filepath);
        outFile.write(reinterpret_cast<const char*>(final_rom.data()), final_rom.size());
        
        std::cout << "Successfully linked ROM. Total size: " << final_rom.size() << " bytes." << std::endl;

    } catch (const std::runtime_error& e) {
        std::cerr << "\nLinker Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
