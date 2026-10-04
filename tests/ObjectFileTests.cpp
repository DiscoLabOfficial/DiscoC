#include "ObjectFile.hpp"

#include <filesystem>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("failed to create test object");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

void appendString(std::vector<std::uint8_t>& bytes, const char* value) {
    const auto length = static_cast<std::uint32_t>(std::strlen(value));
    appendU32(bytes, length);
    bytes.insert(bytes.end(), value, value + length);
}

std::vector<std::uint8_t> objectPrefix() {
    std::vector<std::uint8_t> bytes{
        'D', 'I', 'S', 'C', 'O', 4,
        static_cast<std::uint8_t>(TargetKind::GSU), 0};
    appendU32(bytes, 0x8000);
    bytes.push_back(1); // Packed DATA by default in version 4.
    return bytes;
}

void expectReadFailure(const std::filesystem::path& path) {
    try {
        (void)ObjectFile::read(path.string());
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("malformed object was accepted: " + path.string());
}

} // namespace

int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path() / "discoc-object-tests";
        std::filesystem::create_directories(directory);

        auto huge_section = objectPrefix();
        appendU32(huge_section, 0xffffffffu);
        writeBytes(directory / "huge-section.o", huge_section);
        expectReadFailure(directory / "huge-section.o");

        auto truncated_section = objectPrefix();
        appendU32(truncated_section, 1);
        writeBytes(directory / "truncated-section.o", truncated_section);
        expectReadFailure(directory / "truncated-section.o");

        auto invalid_relocation = objectPrefix();
        appendU32(invalid_relocation, 0); // code section
        appendU32(invalid_relocation, 0); // data section
        appendU32(invalid_relocation, 0); // symbols
        appendU32(invalid_relocation, 1); // relocations
        appendU32(invalid_relocation, 1); // target name length
        invalid_relocation.push_back('x');
        invalid_relocation.push_back(0); // CODE
        appendU32(invalid_relocation, 0); // patch offset
        invalid_relocation.push_back(0); // ADDR16_JAL
        writeBytes(directory / "invalid-relocation.o", invalid_relocation);
        expectReadFailure(directory / "invalid-relocation.o");

        ObjectFile valid;
        valid.config.target = TargetKind::SPC700;
        valid.code_section = {0x01, 0x02};
        valid.data_section = {0x03};
        valid.symbol_table.push_back({"entry", SymbolSection::CODE, 0});
        const auto valid_path = directory / "valid.o";
        valid.write(valid_path.string());
        const auto loaded = ObjectFile::read(valid_path.string());
        if (loaded.code_section != valid.code_section ||
            loaded.data_section != valid.data_section ||
            loaded.config.target != valid.config.target ||
            loaded.symbol_table.size() != 1 ||
            loaded.symbol_table.front().name != "entry") {
            throw std::runtime_error("valid object did not round-trip");
        }

        std::ifstream valid_input(valid_path, std::ios::binary);
        const std::vector<std::uint8_t> valid_bytes(
            std::istreambuf_iterator<char>(valid_input), {});
        if (valid_bytes.size() < 12 || valid_bytes[5] != ObjectFile::CurrentFormatVersion ||
            valid_bytes[8] != 0x00 || valid_bytes[9] != 0x80 ||
            valid_bytes[10] != 0x00 || valid_bytes[11] != 0x00) {
            throw std::runtime_error("object header is not serialized as little-endian bytes");
        }
        for (const auto byte : {0u, 3u, 255u}) {
            for (const auto index : {12u, 13u}) {
                auto malformed = valid_bytes;
                malformed.at(index) = static_cast<std::uint8_t>(byte);
                bool rejected = false;
                try { (void)ObjectFile::readBytes(malformed); } catch (const std::runtime_error&) { rejected = true; }
                if (!rejected) throw std::runtime_error("Invalid v6 DATA/RAM alignment was accepted.");
            }
        }
        const auto loaded_from_memory = ObjectFile::readBytes(valid_bytes);
        if (loaded_from_memory.code_section != valid.code_section ||
            loaded_from_memory.data_section != valid.data_section) {
            throw std::runtime_error("valid object did not load from memory");
        }

        // This fixture is assembled byte by byte instead of being produced by
        // ObjectFile::write, so a host-endian reader cannot hide behind a
        // matching host-endian writer.
        auto little_endian_fixture = objectPrefix();
        appendU32(little_endian_fixture, 0); // code section
        appendU32(little_endian_fixture, 0); // data section
        appendU32(little_endian_fixture, 1); // symbol count
        appendString(little_endian_fixture, "entry");
        little_endian_fixture.push_back(0); // CODE
        appendU32(little_endian_fixture, 0);
        appendU32(little_endian_fixture, 0); // relocation count
        const auto manual = ObjectFile::readBytes(little_endian_fixture);
        if (manual.symbol_table.size() != 1 || manual.symbol_table.front().name != "entry") {
            throw std::runtime_error("manual little-endian string fixture was not decoded");
        }
        valid_input.close();

        auto legacy = valid_bytes;
        legacy.erase(legacy.begin() + 14); // Remove v7 bitmap-presence flag.
        legacy[5] = 6;
        if (ObjectFile::readBytes(legacy).code_section != valid.code_section)
            throw std::runtime_error("Version 6 compatibility failed");
        legacy.erase(legacy.begin() + 13); // Remove v6 RAM alignment.
        legacy[5] = 5;
        if (ObjectFile::readBytes(legacy).code_section != valid.code_section)
            throw std::runtime_error("Version 5 compatibility failed");
        // Remove v5's empty RAM image before exercising the v3/v4 layouts.
        legacy.erase(legacy.begin() + 24, legacy.begin() + 28);
        legacy[5] = 4;
        if (ObjectFile::readBytes(legacy).code_section != valid.code_section)
            throw std::runtime_error("Version 4 compatibility failed");
        legacy[5] = 3;
        legacy.erase(legacy.begin() + 12);
        if (ObjectFile::readBytes(legacy).data_alignment != 1)
            throw std::runtime_error("Version 3 DATA must retain packed layout");
        auto invalid_alignment = valid_bytes;
        invalid_alignment[12] = 3;
        writeBytes(directory / "invalid-alignment.o", invalid_alignment);
        expectReadFailure(directory / "invalid-alignment.o");
        valid.data_alignment = 2;
        valid.ram_alignment = 8;
        valid.write(valid_path.string());
        if (ObjectFile::read(valid_path.string()).data_alignment != 2 || ObjectFile::read(valid_path.string()).ram_alignment != 8)
            throw std::runtime_error("DATA alignment did not round-trip");

        auto ram_fixture = objectPrefix();
        ram_fixture[5] = 5;
        appendU32(ram_fixture, 0); // CODE
        appendU32(ram_fixture, 0); // DATA
        appendU32(ram_fixture, 2); // RAM image
        ram_fixture.insert(ram_fixture.end(), {0x95, 0x00});
        appendU32(ram_fixture, 1);
        appendString(ram_fixture, "counter");
        ram_fixture.push_back(2); // RAM symbol
        const auto ram_symbol_offset = ram_fixture.size();
        appendU32(ram_fixture, 0);
        appendU32(ram_fixture, 0);
        const auto decoded_ram = ObjectFile::readBytes(ram_fixture);
        if (decoded_ram.ram_section != std::vector<std::uint8_t>({0x95, 0}) ||
            decoded_ram.symbol_table.front().section != SymbolSection::RAM)
            throw std::runtime_error("RAM initialization image/symbol did not decode");

        auto bad_ram_span = ram_fixture;
        bad_ram_span[ram_symbol_offset] = 3;
        writeBytes(directory / "ram-offset.o", bad_ram_span);
        expectReadFailure(directory / "ram-offset.o");

        auto oversized_ram = objectPrefix();
        oversized_ram[5] = 5;
        appendU32(oversized_ram, 0);
        appendU32(oversized_ram, 0);
        appendU32(oversized_ram, 65537);
        writeBytes(directory / "huge-ram.o", oversized_ram);
        expectReadFailure(directory / "huge-ram.o");
        oversized_ram[21] = 2; oversized_ram[22] = 0; oversized_ram[23] = 0;
        writeBytes(directory / "truncated-ram.o", oversized_ram);
        expectReadFailure(directory / "truncated-ram.o");

        auto unsupported_ram_patch = ram_fixture;
        unsupported_ram_patch[unsupported_ram_patch.size() - 4] = 1;
        appendString(unsupported_ram_patch, "counter");
        unsupported_ram_patch.push_back(2); // RAM patch section is unsupported.
        appendU32(unsupported_ram_patch, 0);
        unsupported_ram_patch.push_back(4);
        writeBytes(directory / "ram-patch.o", unsupported_ram_patch);
        expectReadFailure(directory / "ram-patch.o");

        auto legacy_ram_symbol = ram_fixture;
        legacy_ram_symbol.erase(legacy_ram_symbol.begin() + 21, legacy_ram_symbol.begin() + 27);
        legacy_ram_symbol[5] = 4;
        writeBytes(directory / "legacy-ram.o", legacy_ram_symbol);
        expectReadFailure(directory / "legacy-ram.o");

        for (std::size_t length = 0; length < ram_fixture.size(); ++length) {
            try {
                (void)ObjectFile::readBytes(std::vector<std::uint8_t>(ram_fixture.begin(), ram_fixture.begin() + length));
            } catch (const std::runtime_error&) { continue; }
            throw std::runtime_error("truncated v5 record was accepted");
        }

        ObjectFile bitmap;
        bitmap.config.bitmap.enabled = true;
        bitmap.config.bitmap.height = 192;
        bitmap.config.bitmap.depth = 4;
        bitmap.config.bitmap.base = 0x4000;
        const auto bitmap_path = directory / "bitmap.o";
        bitmap.write(bitmap_path.string());
        const auto decoded_bitmap = ObjectFile::read(bitmap_path.string());
        if (decoded_bitmap.config.bitmap != bitmap.config.bitmap || decoded_bitmap.config.bitmap.scbr() != 16 ||
            decoded_bitmap.config.bitmap.scmr() != 33)
            throw std::runtime_error("Bitmap metadata did not round-trip");
        std::vector<std::uint8_t> bitmap_bytes;
        {
            std::ifstream input(bitmap_path, std::ios::binary);
            bitmap_bytes.assign(std::istreambuf_iterator<char>(input), {});
        }
        for (std::size_t length = 0; length < bitmap_bytes.size(); ++length) {
            try { (void)ObjectFile::readBytes(std::vector<std::uint8_t>(bitmap_bytes.begin(), bitmap_bytes.begin() + length)); }
            catch (const std::runtime_error&) { continue; }
            throw std::runtime_error("Truncated bitmap object was accepted");
        }
        for (const auto field : std::vector<std::size_t>{14, 15, 16, 17, 21}) {
            auto malformed = bitmap_bytes; malformed[field] = 255;
            writeBytes(directory / "bitmap-malformed.o", malformed);
            expectReadFailure(directory / "bitmap-malformed.o");
        }
        auto wrong_target = bitmap_bytes; wrong_target[6] = 1;
        writeBytes(directory / "bitmap-target.o", wrong_target);
        expectReadFailure(directory / "bitmap-target.o");

        std::filesystem::remove_all(directory);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
