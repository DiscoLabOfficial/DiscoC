#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <fstream>
#include <iomanip>
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

        uint32_t current_code_offset = 0;
        for (const auto& obj : objects) {
            // Process CODE symbols
            for (const auto& sym : obj.symbol_table) {
                if (sym.section == SymbolSection::CODE) {
                    if (final_addresses.count(sym.name)) {
                        throw std::runtime_error("Linker Error: Symbol '" + sym.name + "' defined multiple times.");
                    }
                    final_addresses[sym.name] = config.code_start_address + current_code_offset + sym.offset;
                }
            }
            final_code.insert(final_code.end(), obj.code_section.begin(), obj.code_section.end());
            // FIX: Add static_cast to silence the warning.
            current_code_offset += static_cast<uint32_t>(obj.code_section.size());
        }
        
        uint32_t data_start_address = config.code_start_address + current_code_offset;
        uint32_t current_data_offset = 0;
        for (const auto& obj : objects) {
            // Process DATA symbols
            for (const auto& sym : obj.symbol_table) {
                if (sym.section == SymbolSection::DATA) {
                    if (final_addresses.count(sym.name)) {
                        throw std::runtime_error("Linker Error: Symbol '" + sym.name + "' defined multiple times.");
                    }
                    final_addresses[sym.name] = data_start_address + current_data_offset + sym.offset;
                }
            }
            final_data.insert(final_data.end(), obj.data_section.begin(), obj.data_section.end());
            // FIX: Add static_cast to silence the warning.
            current_data_offset += static_cast<uint32_t>(obj.data_section.size());
        }

        // --- 3. Relocation (Patching) ---
        current_code_offset = 0;
        uint32_t code_base_in_rom = 0;

        for (const auto& obj : objects) {
            for (const auto& reloc : obj.relocation_table) {
                if (final_addresses.find(reloc.target_symbol_name) == final_addresses.end()) {
                    throw std::runtime_error("Linker Error: Undefined symbol '" + reloc.target_symbol_name + "'.");
                }
                uint32_t target_addr = final_addresses.at(reloc.target_symbol_name);
                uint32_t patch_location = (reloc.section_to_patch == SymbolSection::CODE ? code_base_in_rom : data_start_address) + reloc.patch_offset;
                
                std::vector<uint8_t>& section_to_patch = (reloc.section_to_patch == SymbolSection::CODE) ? final_code : final_data;
                uint32_t offset_in_section = patch_location - (reloc.section_to_patch == SymbolSection::CODE ? 0 : data_start_address);

                switch (reloc.type) {
                    case RelocationType::ADDR16_JAL:
                    case RelocationType::ADDR16_IWT:
                    case RelocationType::ADDR24_OFFSET:
                        section_to_patch[offset_in_section + 1] = target_addr & 0xFF;
                        section_to_patch[offset_in_section + 2] = (target_addr >> 8) & 0xFF;
                        break;
                    case RelocationType::ADDR24_BANK:
                        section_to_patch[offset_in_section + 1] = (target_addr >> 16) & 0xFF;
                        break;
                }
            }
            // FIX: Add static_cast to silence the warning.
            code_base_in_rom += static_cast<uint32_t>(obj.code_section.size());
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
