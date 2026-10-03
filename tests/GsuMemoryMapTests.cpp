#include "GsuMemoryMap.hpp"
#include "ObjectFile.hpp"

#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

template<typename Action>
void expectFailure(Action action, const std::string& diagnostic) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find(diagnostic) == std::string::npos) {
            throw std::runtime_error("Unexpected mapping diagnostic: " + std::string(error.what()));
        }
        return;
    }
    throw std::runtime_error("Invalid mapping was accepted: " + diagnostic);
}

void checkMap() {
    const std::array<std::uint32_t, 10> rom_addresses{{
        0x008000, 0x00ffff, 0x018000, 0x3f8000, 0x3fffff,
        0x400000, 0x408000, 0x40ffff, 0x5f0000, 0x5fffff}};
    const std::array<std::uint32_t, 6> ram_addresses{{
        0x700000, 0x708000, 0x70ffff, 0x710000, 0x718000, 0x71ffff}};
    const std::array<std::uint32_t, 17> invalid_addresses{{
        0x000000, 0x007fff, 0x010000, 0x3f7fff, 0x600000, 0x6fffff,
        0x720000, 0x780000, 0x7e8000, 0x7f8000, 0x808000, 0xbf8000,
        0xc00000, 0xf00000, 0xffffff, 0x1000000, 0xffffffff}};
    for (const auto address : rom_addresses) {
        if (GsuMemoryMap::region(address) != GsuMemoryMap::Region::Rom) {
            throw std::runtime_error("ROM address was not classified as ROM");
        }
        GsuMemoryMap::validatePayload(address, 1, 0);
    }
    for (const auto address : ram_addresses) {
        if (GsuMemoryMap::region(address) != GsuMemoryMap::Region::Ram) {
            throw std::runtime_error("RAM address was not classified as RAM");
        }
        GsuMemoryMap::validatePayload(address, 0, 1);
    }
    for (const auto address : invalid_addresses) {
        expectFailure([&] { GsuMemoryMap::validatePayload(address, 0, 0); }, "ROM/RAM");
    }
    for (const auto origin : {0x008000u, 0x3f8000u, 0x400000u, 0x5f0000u,
                              0x700000u, 0x708000u, 0x710000u, 0x71ffffu}) {
        const std::uint64_t capacity = 0x10000u - (origin & 0xffffu);
        GsuMemoryMap::validatePayload(origin, capacity, 0);
        GsuMemoryMap::validatePayload(origin, 0, capacity);
        GsuMemoryMap::validatePayload(origin, capacity - 1, 1);
        expectFailure([&] { GsuMemoryMap::validatePayload(origin, capacity + 1, 0); }, "boundary");
        expectFailure([&] { GsuMemoryMap::validatePayload(origin, capacity, 1); }, "boundary");
        expectFailure([&] { GsuMemoryMap::validatePayload(origin, 1, capacity); }, "boundary");
        GsuMemoryMap::validateNearTarget(origin, (origin & 0xff0000u) | 0xffffu);
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    expectFailure([&] { GsuMemoryMap::validatePayload(0x8000, maximum, maximum); }, "boundary");
    expectFailure([&] { GsuMemoryMap::validatePayload(0x8000, 1, maximum); }, "boundary");
    expectFailure([] { GsuMemoryMap::validateNearTarget(0x408000, 0x418000); }, "different bank");
    expectFailure([] { GsuMemoryMap::validateNearTarget(0x708000, 0x718000); }, "different bank");
    expectFailure([] { GsuMemoryMap::validateNearTarget(0x8000, 0x7e8000); }, "ROM/RAM");
}

ObjectFile sizedObject(std::uint32_t origin, std::size_t code_bytes, std::size_t data_bytes) {
    ObjectFile object;
    object.config.code_start_address = origin;
    object.code_section.assign(code_bytes, 1);
    object.data_section.assign(data_bytes, 0x34);
    return object;
}

ObjectFile nearDataObject(std::uint32_t origin) {
    auto object = sizedObject(origin, 3, 1);
    object.code_section = {0xf0, 0, 0};
    object.symbol_table.push_back({"value", SymbolSection::DATA, 0});
    object.relocation_table.push_back({"value", SymbolSection::CODE, 0, RelocationType::ADDR16_IWT});
    return object;
}

void writeFixtures(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto write = [&](const std::string& name, ObjectFile object) {
        object.write((directory / (name + ".o")).string());
    };
    write("near-rom", nearDataObject(0x008000));
    write("near-rom-last", nearDataObject(0x00fffc));
    write("near-rom-3f", nearDataObject(0x3f8000));
    write("near-mirror-40", nearDataObject(0x400000));
    write("near-mirror-5f", nearDataObject(0x5f0000));
    write("near-ram-70", nearDataObject(0x708000));
    write("near-ram-71", nearDataObject(0x710000));
    auto hirom = nearDataObject(0x408000);
    hirom.config.mapping = MemoryMapping::HiROM;
    write("near-hirom", hirom);

    for (const auto origin : {0x708000u, 0x710000u}) {
        auto call = sizedObject(origin, 6, 0);
        call.code_section = {0xff, 0, 0, 1, 0, 1};
        const bool local = origin == 0x710000u;
        const auto name = local ? std::string(1, '\x01') + "target" : std::string("target");
        call.symbol_table.push_back({name, SymbolSection::CODE, 4});
        call.relocation_table.push_back({name, SymbolSection::CODE, 0,
            local ? RelocationType::ADDR16_IWT : RelocationType::ADDR16_JAL});
        write(local ? "local-ram" : "call-ram", call);
    }
    for (const auto origin : {0x5f0000u, 0x710000u}) {
        auto far = sizedObject(origin, 5, 1);
        far.code_section = {0xa1, 0, 0xf2, 0, 0};
        far.symbol_table.push_back({"value", SymbolSection::DATA, 0});
        far.relocation_table.push_back({"value", SymbolSection::CODE, 0, RelocationType::ADDR24_BANK});
        far.relocation_table.push_back({"value", SymbolSection::CODE, 2, RelocationType::ADDR24_OFFSET});
        write(origin == 0x710000u ? "far-ram" : "far-rom", far);
    }
    for (const auto& entry : {std::pair<const char*, std::uint32_t>{"data-a", 0u}, {"data-b", 3u}}) {
        auto object = sizedObject(0x708000, 0, 3);
        object.data_section = {0xf0, 0, 0};
        object.symbol_table.push_back({entry.second == 0 ? "a" : "b", SymbolSection::DATA, 0});
        object.relocation_table.push_back({entry.second == 0 ? "b" : "a", SymbolSection::DATA,
            0, RelocationType::ADDR16_IWT});
        write(entry.first, object);
    }
    write("exact-code", sizedObject(0x00fff0, 16, 0));
    write("exact-data", sizedObject(0x00fff0, 8, 8));
    write("full-mirror", sizedObject(0x400000, 65536, 0));
    write("full-ram", sizedObject(0x710000, 0, 65536));
    write("cross-code", sizedObject(0x00fff0, 17, 0));
    write("cross-data", sizedObject(0x00fff0, 8, 9));
    write("cross-a", sizedObject(0x00fff0, 8, 1));
    write("cross-b", sizedObject(0x00fff0, 8, 1));
    write("cross-mirror", sizedObject(0x5ffff0, 17, 0));
    write("cross-ram", sizedObject(0x71fff0, 8, 9));
    write("invalid-wram", sizedObject(0x7e8000, 2, 0));
    write("invalid-low-rom", sizedObject(0x3f7fff, 2, 0));
    write("invalid-gap", sizedObject(0x600000, 2, 0));
    write("invalid-snes-mirror", sizedObject(0x808000, 2, 0));
    write("invalid-24bit", sizedObject(0x1000000, 2, 0));

    // Object symbols may mark the one-past-section position. Such a label
    // cannot be used as a near target when the section ends at a bank boundary.
    for (const auto type : {RelocationType::ADDR16_JAL, RelocationType::ADDR16_IWT}) {
        auto end = sizedObject(0x40fffd, 3, 0);
        end.code_section = {0xff, 0, 0};
        end.symbol_table.push_back({"end", SymbolSection::CODE, 3});
        end.relocation_table.push_back({"end", SymbolSection::CODE, 0, type});
        write(type == RelocationType::ADDR16_JAL ? "near-end-call" : "near-end-iwt", end);
    }
    for (const auto type : {RelocationType::ADDR24_BANK, RelocationType::ADDR24_OFFSET}) {
        auto end = sizedObject(0x5ffffd, 3, 0);
        end.symbol_table.push_back({"end", SymbolSection::CODE, 3});
        end.relocation_table.push_back({"end", SymbolSection::CODE, 0, type});
        write(type == RelocationType::ADDR24_BANK ? "far-end-bank" : "far-end-offset", end);
    }
    // The format's address-width guard also applies to hand-authored objects
    // for targets without a production backend; do not silently clip fields.
    auto wide = sizedObject(0xfffffe, 3, 0);
    wide.config.target = TargetKind::SPC700;
    wide.symbol_table.push_back({"end", SymbolSection::CODE, 2});
    wide.relocation_table.push_back({"end", SymbolSection::CODE, 0, RelocationType::ADDR24_OFFSET});
    write("far-overflow", wide);
    wide.config.code_start_address = 0xfffe;
    wide.relocation_table.front().type = RelocationType::ADDR16_IWT;
    write("near-overflow", wide);
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--write-fixtures") {
            writeFixtures(argv[2]);
        } else if (argc == 1) {
            checkMap();
        } else {
            throw std::runtime_error("Usage: disco_gsu_mapping_tests [--write-fixtures directory]");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
