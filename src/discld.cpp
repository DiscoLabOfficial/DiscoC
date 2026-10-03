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
#include "GsuMemoryMap.hpp"
#include "AssemblyGenerator.hpp"
#include <cctype>
#if __cplusplus >= 201703L
#include <filesystem>
#endif

namespace {
std::uint32_t parseOptionNumber(const std::string& text, std::uint32_t maximum) {
    if (text.empty() || text.size() > 10 || text.front() == '-' || text.front() == '+' ||
        std::isspace(static_cast<unsigned char>(text.front())))
        throw std::runtime_error("Invalid numeric option value: " + text);
    std::size_t consumed = 0;
    const auto value = std::stoul(text, &consumed, 0);
    if (consumed != text.size() || value > maximum)
        throw std::runtime_error("Numeric option value is out of range: " + text);
    return static_cast<std::uint32_t>(value);
}
bool sameOutputPath(const std::string& left, const std::string& right) {
#if __cplusplus >= 201703L
    std::error_code error;
    if (std::filesystem::equivalent(left, right, error) && !error) return true;
    return std::filesystem::weakly_canonical(left) == std::filesystem::weakly_canonical(right);
#else
    // DOS/C++14 has no standard filesystem API. Protect exact path reuse.
    return left == right;
#endif
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: discld [options] <file1.o> <file2.o> ... -o <payload.bin>\n"
                     "  --origin <24-bit address>  Override the link/execution origin.\n"
                     "  --init-runtime            Initialize RAMBR/R10 and jump to the entry.\n"
                     "  --ram-bank <0|1>          Data/stack bank (default: 0, bank $70).\n"
                     "  --stack-pointer <word>    Initial aligned R10 (default: 0x2000).\n"
                     "  --entry <symbol>          Entry for runtime initialization (default: main).\n"
                     "  --emit-asm <file.s>       Export the exact linked instructions/data.\n";
        return 1;
    }
    
    std::vector<std::string> object_files;
    std::string out_filepath = "new.bin";
    std::string assembly_filepath, entry_name = "main";
    bool origin_override = false, initialize_runtime = false, runtime_options = false;
    std::uint32_t origin = 0, ram_bank = 0, stack_pointer = 0x2000;

    try {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o") {
            if (i + 1 < argc) out_filepath = argv[++i];
            else { std::cerr << "Error: -o requires a filename." << std::endl; return 1; }
        } else if (arg == "--init-runtime") {
            initialize_runtime = true;
        } else if (arg == "--origin" || arg == "--ram-bank" || arg == "--stack-pointer" ||
                   arg == "--entry" || arg == "--emit-asm") {
            if (i + 1 >= argc) throw std::runtime_error(arg + " requires a value.");
            const std::string value = argv[++i];
            if (arg == "--origin") { origin = parseOptionNumber(value, 0xffffff); origin_override = true; }
            else if (arg == "--ram-bank") { ram_bank = parseOptionNumber(value, 1); runtime_options = true; }
            else if (arg == "--stack-pointer") { stack_pointer = parseOptionNumber(value, 0xfffe); runtime_options = true; }
            else if (arg == "--entry") { entry_name = value; runtime_options = true; }
            else assembly_filepath = value;
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("Unknown linker option: " + arg);
        } else {
            object_files.push_back(arg);
        }
    }
    if (runtime_options && !initialize_runtime) throw std::runtime_error("Runtime options require --init-runtime.");
    if (initialize_runtime && (stack_pointer < 8 || (stack_pointer & 1u)))
        throw std::runtime_error("Initial stack pointer must be even and within 0x0008..0xFFFE.");
    if (!assembly_filepath.empty() && sameOutputPath(assembly_filepath, out_filepath))
        throw std::runtime_error("Binary and assembly output paths must be different.");
    for (const auto& input : object_files) {
        if (sameOutputPath(input, out_filepath) ||
            (!assembly_filepath.empty() && sameOutputPath(input, assembly_filepath)))
            throw std::runtime_error("Output path must not overwrite an input object.");
    }
    } catch (const std::exception& error) {
        std::cerr << "Linker Error: " << error.what() << '\n';
        return 1;
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

        CompilerConfig config = objects.front().config;
        if (origin_override) config.code_start_address = origin;
        for (std::size_t index = 1; index < objects.size(); ++index) {
            auto candidate = objects[index].config;
            if (origin_override) candidate.code_start_address = origin;
            if (candidate != config) {
                throw std::runtime_error(
                    "Input objects use incompatible target configurations.");
            }
        }
        if (initialize_runtime && config.target != TargetKind::GSU)
            throw std::runtime_error("Runtime initialization is only supported for GSU.");
        const std::size_t startup_size = initialize_runtime ? 12u : 0u;

        if (config.target == TargetKind::GSU) {
            GsuMemoryMap::validatePayload(config.code_start_address, 0, 0);
            std::uint64_t code_bytes = startup_size;
            std::uint64_t data_bytes = 0;
            for (const auto& object : objects) {
                // Each successful iteration leaves both totals <= 64 KiB;
                // individual sections are bounded by ObjectFile::read.
                code_bytes += object.code_section.size();
                data_bytes += object.data_section.size();
                GsuMemoryMap::validatePayload(config.code_start_address, code_bytes, data_bytes);
            }
        }

        // --- 2. Layout and Symbol Resolution ---
        std::vector<uint8_t> final_code;
        if (initialize_runtime) {
            final_code = {0xf0, static_cast<std::uint8_t>(ram_bank), 0x00, // IWT R0, bank bit
                          0x3e, 0xdf, // RAMB, before the first stack access
                          0xfa, static_cast<std::uint8_t>(stack_pointer),
                          static_cast<std::uint8_t>(stack_pointer >> 8), // IWT R10, SP
                          0xff, 0x00, 0x00, 0x01}; // IWT R15, entry; NOP
        }
        std::vector<uint8_t> final_data;
        std::map<std::string, uint32_t> final_addresses;
        std::map<std::string, SymbolSection> final_sections;

        std::uint64_t current_code_offset = startup_size;
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
                    final_sections[sym.name] = SymbolSection::CODE;
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
                    final_sections[sym.name] = SymbolSection::DATA;
                }
            }
            final_data.insert(final_data.end(), obj.data_section.begin(), obj.data_section.end());
            current_data_offset += obj.data_section.size();
        }

        // --- 3. Relocation (Patching) ---
        if (initialize_runtime) {
            const auto entry = final_addresses.find(entry_name);
            if (entry == final_addresses.end() || final_sections.at(entry_name) != SymbolSection::CODE ||
                entry->second >= data_start_address)
                throw std::runtime_error("Runtime entry must name an instruction in CODE: " + entry_name);
            GsuMemoryMap::validateNearTarget(config.code_start_address, entry->second);
            final_code[9] = static_cast<std::uint8_t>(entry->second);
            final_code[10] = static_cast<std::uint8_t>(entry->second >> 8);
            const auto stack_address = 0x700000u | (ram_bank << 16) | stack_pointer;
            const std::uint64_t payload_end = static_cast<std::uint64_t>(data_start_address) + final_data.size();
            if (stack_address < payload_end &&
                static_cast<std::uint64_t>(stack_address) + 2 > config.code_start_address)
                throw std::runtime_error("Initial stack word overlaps the linked payload.");
        }
        std::uint64_t current_code_base = startup_size;
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
                if (target_addr > 0xffffffu) {
                    throw std::runtime_error("Relocation target exceeds the 24-bit address range.");
                }
                if (config.target == TargetKind::GSU) {
                    if (reloc.type == RelocationType::ADDR16_JAL ||
                        reloc.type == RelocationType::ADDR16_IWT) {
                        GsuMemoryMap::validateNearTarget(config.code_start_address, target_addr);
                    } else {
                        // ADDR24_OFFSET is paired with a bank relocation and
                        // may intentionally represent a full 24-bit address.
                        GsuMemoryMap::validateAddress(target_addr);
                    }
                } else if ((reloc.type == RelocationType::ADDR16_JAL ||
                            reloc.type == RelocationType::ADDR16_IWT) && target_addr > 0xffffu) {
                    throw std::runtime_error("Relocation target exceeds the 16-bit address range.");
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

        std::string linked_assembly;
        if (!assembly_filepath.empty()) {
            ObjectFile linked;
            linked.config = config;
            linked.code_section = final_code;
            linked.data_section = final_data;
            for (const auto& symbol : final_addresses) {
                const auto section = final_sections.at(symbol.first);
                const auto base = section == SymbolSection::CODE ? config.code_start_address : data_start_address;
                linked.symbol_table.push_back({symbol.first, section, symbol.second - base});
            }
            linked_assembly = AssemblyGenerator(linked).generate();
        }

        std::ofstream outFile(out_filepath, std::ios::out | std::ios::binary);
        if (!outFile) throw std::runtime_error("Failed to open output file for writing: " + out_filepath);
        outFile.write(reinterpret_cast<const char*>(final_rom.data()), final_rom.size());
        outFile.flush();
        if (!outFile) throw std::runtime_error("Failed to write linked payload: " + out_filepath);
        if (!assembly_filepath.empty()) {
            std::ofstream assembly(assembly_filepath);
            if (!assembly) throw std::runtime_error("Failed to open linked assembly output: " + assembly_filepath);
            assembly << linked_assembly;
            assembly.flush();
            if (!assembly) throw std::runtime_error("Failed to write linked assembly output: " + assembly_filepath);
        }
        
        std::cout << "Successfully linked payload. Total size: " << final_rom.size() << " bytes." << std::endl;

    } catch (const std::runtime_error& e) {
        std::cerr << "\nLinker Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
