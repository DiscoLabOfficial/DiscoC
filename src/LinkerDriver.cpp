#include "LinkerDriver.hpp"
#include "GSUCodeLayout.hpp"
#include "ProjectManifest.hpp"
#include "Placement.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include "ObjectFile.hpp"
#include "Parser.hpp"
#include "GsuMemoryMap.hpp"
#include "AssemblyGenerator.hpp"
#include "ABI.hpp"
#include "RuntimeInitialization.hpp"
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
    unsigned long value = 0;
    try { value = std::stoul(text, &consumed, 0); }
    catch (const std::invalid_argument&) { throw std::runtime_error("Invalid numeric option value: " + text); }
    catch (const std::out_of_range&) { throw std::runtime_error("Numeric option value is out of range: " + text); }
    if (consumed != text.size() || value > maximum)
        throw std::runtime_error("Numeric option value is out of range: " + text);
    return static_cast<std::uint32_t>(value);
}
bool sameOutputPath(const std::string& left, const std::string& right) {
    return DiscoProject::samePath(left, right);
}
void usage() {
    std::cout << "Usage: discld [options] <file1.o> <file2.o> ... -o <payload.bin>\n"
                 "  --config <discoc.toml>     Load link/runtime/output defaults.\n"
                 "  --no-init-runtime | --no-host-initialized-globals | --no-emit-asm\n"
                 "  --target <gsu|spc700>      Assert the objects' target.\n"
                 "  --memory-mapping <lorom|hirom> Assert the objects' mapping.\n"
                 "  --execution-memory <rom|ram>   Validate the execution region.\n"
                 "  --origin <24-bit address> Override the link/execution origin.\n"
                 "  --init-runtime           Initialize RAMBR/R10 and jump to the entry.\n"
                 "  --ram-bank <0|1>          Data/stack bank (default: 0, bank $70).\n"
                 "  --ram-origin <word>       Static RAM start (default: 0x0400).\n"
                 "  --host-initialized-globals Host supplies global initial values instead of startup.\n"
                 "  --rom-bank <bank>         Near ROM bank (default: execution bank, or 0 for RAM code).\n"
                 "  --stack-pointer <word>    Initial aligned R10 (default: 0x2000).\n"
                 "  --entry <symbol>          Runtime entry (default: main).\n"
                 "  --emit-asm <file.s>       Export exact linked instructions/data.\n";
}
} // namespace

int runLinker(std::vector<std::string> arguments) {
    std::unique_ptr<DiscoProject::Manifest> manifest;
    try {
        manifest = DiscoProject::commandManifest(arguments, DiscoProject::Consumer::Linker);
        arguments = DiscoProject::configurationArguments(arguments, DiscoProject::Consumer::Linker, manifest.get());
    } catch (const std::exception& error) {
        std::cerr << "Linker Error: " << error.what() << '\n'; return 1;
    }
    const auto argc = static_cast<int>(arguments.size());
    if (argc < 2) { usage(); return 1; }

    std::vector<std::string> object_files;
    std::string out_filepath = "new.bin";
    std::string assembly_filepath, entry_name = "main";
    bool origin_override = false, initialize_runtime = false, runtime_options = false;
    std::uint32_t origin = 0, ram_bank = 0, stack_pointer = 0x2000;
    std::uint32_t rom_bank = 0;
    bool rom_bank_override = false;
    std::uint32_t ram_origin = 0x0400;
    bool host_globals = false;
    bool target_expected = false, mapping_expected = false;
    TargetKind expected_target = TargetKind::GSU;
    MemoryMapping expected_mapping = MemoryMapping::LoROM;
    PlacementOptions::Execution execution = PlacementOptions::Execution::Automatic;

    try {
    for (int i = 1; i < argc; ++i) {
        std::string arg = arguments[i];
        if (arg == "--help" || arg == "-h") { usage(); return 0; }
        if (arg == "-o") {
            if (i + 1 < argc) out_filepath = arguments[++i];
            else { std::cerr << "Error: -o requires a filename." << std::endl; return 1; }
        } else if (arg == "--target" || arg == "--memory-mapping" || arg == "--execution-memory") {
            if (i + 1 == argc) throw std::runtime_error(arg + " requires a value.");
            const auto value = arguments[++i];
            if (arg == "--target") { expected_target = DiscoProject::parseTarget(value); target_expected = true; }
            else if (arg == "--memory-mapping") {
                if (value != "lorom" && value != "hirom") throw std::runtime_error("Unsupported memory mapping; use lorom or hirom.");
                expected_mapping = value == "lorom" ? MemoryMapping::LoROM : MemoryMapping::HiROM; mapping_expected = true;
            } else {
                if (value != "rom" && value != "ram") throw std::runtime_error("Execution memory must be rom or ram.");
                execution = value == "rom" ? PlacementOptions::Execution::Rom : PlacementOptions::Execution::Ram;
            }
        } else if (arg == "--no-init-runtime") {
            initialize_runtime = false;
        } else if (arg == "--no-host-initialized-globals") {
            host_globals = false;
        } else if (arg == "--no-emit-asm") {
            assembly_filepath.clear();
        } else if (arg == "--init-runtime") {
            initialize_runtime = true;
        } else if (arg == "--host-initialized-globals") {
            host_globals = true;
        } else if (arg == "--origin" || arg == "--ram-bank" || arg == "--rom-bank" || arg == "--stack-pointer" ||
                   arg == "--entry" || arg == "--emit-asm" || arg == "--ram-origin") {
            if (i + 1 >= argc) throw std::runtime_error(arg + " requires a value.");
            const std::string value = arguments[++i];
            if (arg == "--origin") { origin = parseOptionNumber(value, 0xffffff); origin_override = true; }
            else if (arg == "--ram-bank") { ram_bank = parseOptionNumber(value, 1); }
            else if (arg == "--ram-origin") { ram_origin = parseOptionNumber(value, 65535); }
            else if (arg == "--rom-bank") { rom_bank = parseOptionNumber(value, 0x5f); rom_bank_override = true; }
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
    if (ram_origin & 1u) throw std::runtime_error("Static RAM origin must be word-aligned.");
    if (host_globals && initialize_runtime) throw std::runtime_error("Choose runtime initialization or host-initialized globals, not both.");
    if (initialize_runtime && (stack_pointer < 8 || (stack_pointer & 1u)))
        throw std::runtime_error("Initial stack pointer must be even and within 0x0008..0xFFFE.");
    if (!assembly_filepath.empty() && sameOutputPath(assembly_filepath, out_filepath))
        throw std::runtime_error("Binary and assembly output paths must be different.");
    if (manifest) DiscoProject::protectManifest(*manifest, assembly_filepath.empty()
        ? std::vector<std::string>{out_filepath} : std::vector<std::string>{out_filepath, assembly_filepath});
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
        std::uint64_t input_code_bytes = 0, input_data_bytes = 0, input_ram_bytes = 0;
        std::uint64_t input_record_bytes = 0;
        for (const auto& path : object_files) {
            auto object = ObjectFile::read(path);
            if (objects.empty()) {
                if (target_expected && object.config.target != expected_target)
                    throw std::runtime_error("Manifest/CLI target does not match input objects.");
                if (mapping_expected && object.config.mapping != expected_mapping)
                    throw std::runtime_error("Manifest/CLI mapping does not match input objects.");
            }
            // Charge decoded records using the current wire layout, including
            // names/tables, not just payload bytes. Legacy records are charged
            // at the same rate. Bound the complete link as well as each reader.
            std::uint64_t object_bytes = 35u + (object.config.bitmap.enabled ? 10u : 0u) +
                static_cast<std::uint64_t>(object.code_section.size()) + object.data_section.size() + object.ram_section.size();
            for (const auto& symbol : object.symbol_table) object_bytes += 9u + symbol.name.size();
            for (const auto& relocation : object.relocation_table) object_bytes += 10u + relocation.target_symbol_name.size();
            if (object_bytes > ObjectFile::MaxObjectBytes - input_record_bytes)
                throw std::runtime_error("Combined link inputs exceed the supported 128 MiB record budget.");
            input_record_bytes += object_bytes;
            if (!objects.empty()) {
                auto first_config = objects.front().config, next_config = object.config;
                if (origin_override) first_config.code_start_address = next_config.code_start_address = origin;
                if (first_config != next_config)
                    throw std::runtime_error("Input objects use incompatible target configurations.");
            }
            // Reject impossible raw totals before retaining more objects.
            // Startup/alignment can only grow them and are checked again after
            // layout; a GSU link must not accumulate gigabytes of input images.
            if (object.config.target == TargetKind::GSU) {
                input_code_bytes += object.code_section.size();
                input_data_bytes += object.data_section.size();
                input_ram_bytes += object.ram_section.size();
                GsuMemoryMap::validatePayload(origin_override ? origin : object.config.code_start_address,
                    input_code_bytes, input_data_bytes);
                if (input_ram_bytes > 65536u - ram_origin)
                    throw std::runtime_error("Static storage crosses a RAM bank boundary.");
            }
            objects.push_back(std::move(object));
        }

        CompilerConfig config = objects.front().config;
        if (target_expected && config.target != expected_target) throw std::runtime_error("Manifest/CLI target does not match input objects.");
        if (mapping_expected && config.mapping != expected_mapping) throw std::runtime_error("Manifest/CLI mapping does not match input objects.");
        if (origin_override) config.code_start_address = origin;
        if (execution != PlacementOptions::Execution::Automatic) {
            if (config.target != TargetKind::GSU) throw std::runtime_error("Execution-memory configuration is GSU-only.");
            const auto region = execution == PlacementOptions::Execution::Ram ? GsuMemoryMap::Region::Ram : GsuMemoryMap::Region::Rom;
            if (GsuMemoryMap::region(config.code_start_address) != region) throw std::runtime_error("Origin does not match selected execution memory.");
        }
        if (!rom_bank_override) {
            const auto bank = config.code_start_address >> 16;
            rom_bank = bank <= 0x5f ? bank : 0;
        }
        for (std::size_t index = 1; index < objects.size(); ++index) {
            auto candidate = objects[index].config;
            if (origin_override) candidate.code_start_address = origin;
            if (candidate != config) {
                throw std::runtime_error(
                    "Input objects use incompatible target configurations.");
            }
            if (candidate.bitmap.enabled) {
                if (config.bitmap.enabled && config.bitmap != candidate.bitmap)
                    throw std::runtime_error("Input objects select conflicting host bitmap configurations.");
                config.bitmap = candidate.bitmap;
            }
        }
        config.bitmap.validate();
        if (initialize_runtime && config.target != TargetKind::GSU)
            throw std::runtime_error("Runtime initialization is only supported for GSU.");
        std::vector<std::uint8_t> final_ram;
        std::vector<std::size_t> ram_bases;
        for (const auto& object : objects) {
            while ((ram_origin + final_ram.size()) % object.ram_alignment) final_ram.push_back(0);
            ram_bases.push_back(final_ram.size());
            if (final_ram.size() > 65536u - ram_origin || object.ram_section.size() > 65536u - ram_origin - final_ram.size())
                throw std::runtime_error("Static storage crosses a RAM bank boundary.");
            final_ram.insert(final_ram.end(), object.ram_section.begin(), object.ram_section.end());
        }
        if (!final_ram.empty() && !initialize_runtime && !host_globals)
            throw std::runtime_error("Static globals require --init-runtime or --host-initialized-globals.");
        if (!final_ram.empty() && config.target != TargetKind::GSU)
            throw std::runtime_error("Static storage placement is currently GSU-only.");
        const auto startup = initialize_runtime ? makeRuntimeInitialization(static_cast<std::uint8_t>(ram_bank),
            static_cast<std::uint16_t>(stack_pointer), static_cast<std::uint16_t>(ram_origin), final_ram) : RuntimeInitialization{};
        const std::size_t startup_size = startup.bytes.size();

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
            final_code = startup.bytes;
        }
        std::vector<uint8_t> final_data;
        std::map<std::string, uint32_t> final_addresses;
        std::map<std::string, SymbolSection> final_sections;
        std::map<std::string, bool> code_targets;
        bool entry_is_instruction = false;
        for (std::size_t index = 0; index < objects.size(); ++index) {
            for (const auto& symbol : objects[index].symbol_table) {
                if (symbol.section != SymbolSection::RAM || (!symbol.name.empty() && symbol.name.front() == '\x01')) continue;
                if (final_addresses.count(symbol.name)) throw std::runtime_error("Duplicate global symbol: " + symbol.name);
                final_addresses.emplace(symbol.name, 0x700000u | (ram_bank << 16) |
                    checkedAddress(ram_origin + ram_bases[index] + symbol.offset, "RAM symbol address"));
                final_sections.emplace(symbol.name, SymbolSection::RAM);
            }
        }

        std::uint64_t current_code_offset = startup_size;
        std::vector<std::uint64_t> code_bases;
        for (const auto& obj : objects) {
            const auto code_alignment = GSUCodeLayout::alignment(obj);
            while ((config.code_start_address + current_code_offset) % code_alignment) {
                final_code.push_back(1); ++current_code_offset;
            }
            code_bases.push_back(current_code_offset);
            for (const auto& sym : obj.symbol_table)
                if (sym.name == GSUAbi::NearRamBankSymbol || sym.name == GSUAbi::NearRomBankSymbol ||
                    sym.name.compare(0, std::string(GSUAbi::StackLimitPrefix).size(), GSUAbi::StackLimitPrefix) == 0)
                    throw std::runtime_error("Symbol name is reserved for the pointer ABI: " + sym.name);
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
                    code_targets.emplace(sym.name, sym.offset < obj.code_section.size());
                    if (sym.name == entry_name) entry_is_instruction = sym.offset < obj.code_section.size();
                }
            }
            final_code.insert(final_code.end(), obj.code_section.begin(), obj.code_section.end());
            current_code_offset += obj.code_section.size();
        }
        std::uint8_t data_alignment = 1;
        for (const auto& object : objects)
            if (!object.data_section.empty()) data_alignment = std::max(data_alignment, object.data_alignment);
        while ((config.code_start_address + current_code_offset) % data_alignment) {
            final_code.push_back(1); // Alignment padding is explicit in byte-exact assembly exports.
            ++current_code_offset;
        }

        const uint32_t data_start_address = checkedAddress(
            static_cast<std::uint64_t>(config.code_start_address) + current_code_offset,
            "data section address");
        std::uint64_t current_data_offset = 0;
        for (const auto& obj : objects) {
            while ((data_start_address + current_data_offset) % obj.data_alignment) {
                final_data.push_back(0);
                ++current_data_offset;
            }
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
        if (config.target == TargetKind::GSU)
            GsuMemoryMap::validatePayload(config.code_start_address, final_code.size(), final_data.size());
        if (config.bitmap.enabled) {
            const std::uint64_t begin = 0x700000u + config.bitmap.base;
            const auto end = begin + config.bitmap.sizeBytes();
            const auto overlaps = [&](std::uint64_t address, std::uint64_t bytes) {
                return bytes && address < end && address + bytes > begin;
            };
            if (overlaps(config.code_start_address, final_code.size() + final_data.size()))
                throw std::runtime_error("Bitmap framebuffer overlaps the linked RAM payload.");
            if (overlaps(0x700000u + (ram_bank << 16) + ram_origin, final_ram.size()))
                throw std::runtime_error("Bitmap framebuffer overlaps static RAM.");
            if (initialize_runtime && overlaps(0x700000u + (ram_bank << 16) + stack_pointer, 2))
                throw std::runtime_error("Bitmap framebuffer overlaps the initial stack word.");
        }
        if (!final_ram.empty()) {
            const auto ram_start = 0x700000u | (ram_bank << 16) | ram_origin;
            const auto ram_end = static_cast<std::uint64_t>(ram_start) + final_ram.size();
            const auto payload_end = static_cast<std::uint64_t>(config.code_start_address) + final_code.size() + final_data.size();
            if (ram_start < payload_end && ram_end > config.code_start_address)
                throw std::runtime_error("Static RAM overlaps the linked payload.");
            if (initialize_runtime && ram_origin + final_ram.size() >= stack_pointer)
                throw std::runtime_error("Static RAM must end below the initial stack pointer.");
        }

        // --- 3. Relocation (Patching) ---
        if (initialize_runtime) {
            const auto entry = final_addresses.find(entry_name);
            if (entry == final_addresses.end() || final_sections.at(entry_name) != SymbolSection::CODE ||
                !entry_is_instruction)
                throw std::runtime_error("Runtime entry must name an instruction in CODE: " + entry_name);
            GsuMemoryMap::validateNearTarget(config.code_start_address, entry->second);
            final_code.at(startup.entry_patch) = static_cast<std::uint8_t>(entry->second);
            final_code.at(startup.entry_patch + 1) = static_cast<std::uint8_t>(entry->second >> 8);
            const auto stack_address = 0x700000u | (ram_bank << 16) | stack_pointer;
            const std::uint64_t payload_end = static_cast<std::uint64_t>(data_start_address) + final_data.size();
            if (stack_address < payload_end &&
                static_cast<std::uint64_t>(stack_address) + 2 > config.code_start_address)
                throw std::runtime_error("Initial stack word overlaps the linked payload.");
        }
        std::uint64_t current_code_base = startup_size;
        std::uint64_t current_data_base = 0;
        std::size_t current_object = 0;

        const auto ram_bank_base = 0x700000u + (ram_bank << 16);
        auto stack_floor = final_ram.empty() ? 0u :
            static_cast<std::uint32_t>((ram_origin + final_ram.size() + 1u) & ~1u);
        const auto reserve_below_stack = [&](std::uint64_t begin, std::uint64_t end) {
            if (begin < ram_bank_base + stack_pointer && end > ram_bank_base)
                stack_floor = std::max(stack_floor, checkedAddress(end - ram_bank_base, "Stack reservation"));
        };
        if (config.bitmap.enabled)
            reserve_below_stack(0x700000u + config.bitmap.base,
                0x700000u + config.bitmap.base + config.bitmap.sizeBytes());
        // A descending stack must not grow into still-executable RAM code or
        // payload DATA, even when its initial word itself does not overlap.
        if (initialize_runtime && GsuMemoryMap::region(config.code_start_address) == GsuMemoryMap::Region::Ram &&
            (config.code_start_address >> 16) == 0x70u + ram_bank)
            reserve_below_stack(config.code_start_address,
                static_cast<std::uint64_t>(config.code_start_address) + final_code.size() + final_data.size());

        for (const auto& obj : objects) {
            current_code_base = code_bases.at(current_object);
            while ((data_start_address + current_data_base) % obj.data_alignment) ++current_data_base;
            // The object owns these entries for the whole relocation pass.
            // Index local symbols once rather than rescanning for every use.
            std::map<std::string, const SymbolEntry*> local_symbols;
            for (const auto& symbol : obj.symbol_table)
                if (!symbol.name.empty() && symbol.name.front() == '\x01')
                    local_symbols.emplace(symbol.name, &symbol);
            const bool uses_near_rom = std::any_of(obj.relocation_table.begin(), obj.relocation_table.end(),
                [](const RelocationEntry& entry) { return entry.target_symbol_name == GSUAbi::NearRomBankSymbol; });
            for (const auto& reloc : obj.relocation_table) {
                std::uint32_t target_addr = 0;
                SymbolSection target_section = SymbolSection::CODE;
                bool target_is_instruction = false;
                if (!reloc.target_symbol_name.empty() && reloc.target_symbol_name.front() == '\x01') {
                    const auto local_name = reloc.target_symbol_name.substr(1);
                    const auto local = local_symbols.find(reloc.target_symbol_name);
                    if (local == local_symbols.end()) {
                        throw std::runtime_error("Linker Error: Undefined local symbol '" + local_name + "'.");
                    }
                    const auto* local_symbol = local->second;
                    target_section = local_symbol->section;
                    target_is_instruction = target_section == SymbolSection::CODE && local_symbol->offset < obj.code_section.size();
                    const auto base = local_symbol->section == SymbolSection::CODE
                        ? static_cast<std::uint64_t>(config.code_start_address) + current_code_base
                        : local_symbol->section == SymbolSection::DATA ? data_start_address + current_data_base
                        : (0x700000u | (ram_bank << 16)) + ram_origin + ram_bases.at(current_object);
                    target_addr = checkedAddress(base + local_symbol->offset,
                        "local branch target address");
                } else if (reloc.target_symbol_name.compare(0, std::string(GSUAbi::StackLimitPrefix).size(), GSUAbi::StackLimitPrefix) == 0) {
                    if (reloc.type != RelocationType::ADDR16_RAM || config.target != TargetKind::GSU)
                        throw std::runtime_error("Stack-limit symbol requires a GSU RAM relocation.");
                    const auto required = parseOptionNumber(reloc.target_symbol_name.substr(std::string(GSUAbi::StackLimitPrefix).size()), 65535);
                    if (stack_floor > 65535u - required) throw std::runtime_error("Reserved RAM leaves no room for the requested stack frame.");
                    if (initialize_runtime && stack_floor + required > stack_pointer)
                        throw std::runtime_error("Initial stack pointer leaves no room for the requested stack frame.");
                    target_addr = ram_bank_base + stack_floor + required;
                    target_section = SymbolSection::RAM;
                } else if (reloc.target_symbol_name == GSUAbi::NearRamBankSymbol ||
                           reloc.target_symbol_name == GSUAbi::NearRomBankSymbol) {
                    if (config.target != TargetKind::GSU || reloc.type != RelocationType::ADDR24_BANK)
                        throw std::runtime_error("Bank-context symbol requires a GSU bank-byte relocation.");
                    target_addr = reloc.target_symbol_name == GSUAbi::NearRamBankSymbol
                        ? (0x700000u | (ram_bank << 16))
                        : ((rom_bank << 16) | (rom_bank <= 0x3f ? 0x8000u : 0u));
                } else {
                    if (final_addresses.find(reloc.target_symbol_name) == final_addresses.end()) {
                        throw std::runtime_error("Linker Error: Undefined symbol '" + reloc.target_symbol_name + "'.");
                    }
                    target_addr = final_addresses.at(reloc.target_symbol_name);
                    target_section = final_sections.at(reloc.target_symbol_name);
                    if (target_section == SymbolSection::CODE)
                        target_is_instruction = code_targets.at(reloc.target_symbol_name);
                }
                if (reloc.type == RelocationType::ADDR16_JAL && !target_is_instruction)
                    throw std::runtime_error("Call relocation must target an instruction in CODE: " + reloc.target_symbol_name);
                if (target_addr > 0xffffffu) {
                    throw std::runtime_error("Relocation target exceeds the 24-bit address range.");
                }
                if (config.target == TargetKind::GSU) {
                    if (reloc.type == RelocationType::ADDR16_RAM) {
                        if (target_section != SymbolSection::RAM)
                            throw std::runtime_error("RAM relocation must target static RAM or a stack limit.");
                        if ((target_addr >> 16) != 0x70u + ram_bank)
                            throw std::runtime_error("RAM relocation target is outside the near RAM bank.");
                    } else if (reloc.type == RelocationType::ADDR16_JAL ||
                        reloc.type == RelocationType::ADDR16_IWT) {
                        if (reloc.type == RelocationType::ADDR16_IWT && target_section == SymbolSection::DATA && uses_near_rom &&
                            (GsuMemoryMap::region(target_addr) != GsuMemoryMap::Region::Rom || (target_addr >> 16) != rom_bank))
                            throw std::runtime_error("Near ROM data relocation does not match the selected ROM bank; separate RAM-code/ROM-data placement is unsupported.");
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
                    case RelocationType::ADDR16_RAM:
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
            ++current_object;
        }

        // --- 4. Final Assembly & Output ---
        std::vector<uint8_t> final_rom = final_code;
        final_rom.insert(final_rom.end(), final_data.begin(), final_data.end());

        std::string linked_assembly;
        if (!assembly_filepath.empty()) {
            ObjectFile linked;
            linked.config = config;
            linked.data_alignment = data_alignment;
            linked.code_section = final_code;
            linked.data_section = final_data;
            for (const auto& symbol : final_addresses) {
                const auto section = final_sections.at(symbol.first);
                if (section == SymbolSection::RAM) continue; // Addresses in linked code are already resolved.
                const auto base = section == SymbolSection::CODE ? config.code_start_address : data_start_address;
                linked.symbol_table.push_back({symbol.first, section, symbol.second - base});
            }
            linked_assembly = AssemblyGenerator(linked).generate();
        }

        if (manifest) {
            DiscoProject::createDirectories(DiscoProject::parentPath(out_filepath));
            if (!assembly_filepath.empty()) DiscoProject::createDirectories(DiscoProject::parentPath(assembly_filepath));
        }
        std::ofstream outFile(out_filepath, std::ios::out | std::ios::binary);
        if (!outFile) throw std::runtime_error("Failed to open output file for writing: " + out_filepath);
        outFile.write(reinterpret_cast<const char*>(final_rom.data()), final_rom.size());
        outFile.flush();
        if (!outFile) throw std::runtime_error("Failed to write linked payload: " + out_filepath);
        outFile.close();
        if (!outFile) throw std::runtime_error("Failed to close linked payload: " + out_filepath);
        if (!assembly_filepath.empty()) {
            std::ofstream assembly(assembly_filepath);
            if (!assembly) throw std::runtime_error("Failed to open linked assembly output: " + assembly_filepath);
            assembly << linked_assembly;
            assembly.flush();
            if (!assembly) throw std::runtime_error("Failed to write linked assembly output: " + assembly_filepath);
            assembly.close();
            if (!assembly) throw std::runtime_error("Failed to close linked assembly output: " + assembly_filepath);
        }

        std::cout << "Successfully linked payload. Total size: " << final_rom.size() << " bytes." << std::endl;
        if (config.bitmap.enabled)
            std::cout << "SNES host bitmap configuration: SCBR=" << static_cast<unsigned>(config.bitmap.scbr())
                      << ", SCMR mode bits=" << static_cast<unsigned>(config.bitmap.scmr())
                      << " (host must add ROM/RAM ownership bits)." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "\nLinker Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
