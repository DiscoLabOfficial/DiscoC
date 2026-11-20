#include "ObjectFile.hpp"
#include "ABI.hpp"
#include "TestTempDirectory.hpp"

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

void expectMemoryFailure(const std::vector<std::uint8_t>& bytes) {
    try { (void)ObjectFile::readBytes(bytes); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("malformed in-memory object was accepted");
}

void expectWriteFailure(ObjectFile object, const std::filesystem::path& path) {
    const std::vector<std::uint8_t> sentinel{0x95, 0x42, 0x00};
    writeBytes(path, sentinel);
    bool rejected = false;
    try { object.write(path.string()); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("malformed object was written");
    std::ifstream input(path, std::ios::binary);
    const std::vector<std::uint8_t> preserved(std::istreambuf_iterator<char>(input), {});
    if (preserved != sentinel) throw std::runtime_error("invalid object overwrote its destination");
}

// Independently encoded v4 records exercise validation without relying on the
// writer to generate an invalid object that its own validator now rejects.
std::vector<std::uint8_t> symbolFixture(const std::string& name, std::uint8_t section = 0,
                                      std::uint32_t offset = 0, unsigned count = 1) {
    auto bytes = objectPrefix();
    appendU32(bytes, 1); bytes.push_back(0); // CODE
    appendU32(bytes, 0); // DATA
    appendU32(bytes, count);
    for (unsigned i = 0; i < count; ++i) {
        appendU32(bytes, static_cast<std::uint32_t>(name.size()));
        bytes.insert(bytes.end(), name.begin(), name.end());
        bytes.push_back(section); appendU32(bytes, offset);
    }
    appendU32(bytes, 0); // relocations
    return bytes;
}

} // namespace

int main() {
    try {
        TestTempDirectory temporary("object-tests");
        const auto& directory = temporary.path();

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
        if (valid_bytes.size() < 12 || valid_bytes[5] != ObjectFile::BaselineFormatVersion ||
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

        // The static-RAM proof marker is the only version-8 feature. It is
        // written as v8 so a linker that cannot enforce it refuses the object.
        ObjectFile proved;
        proved.code_section = {0x00, 0x01};
        proved.ram_section = {0x00, 0x00};
        proved.symbol_table.push_back({"main", SymbolSection::CODE, 0});
        proved.symbol_table.push_back({GSUAbi::StaticRamProofSymbol, SymbolSection::RAM, 0});
        const auto proved_path = directory / "static-proof.o";
        proved.write(proved_path.string());
        std::ifstream proved_input(proved_path, std::ios::binary);
        auto proved_bytes = std::vector<std::uint8_t>(std::istreambuf_iterator<char>(proved_input), {});
        proved_input.close();
        if (proved_bytes.size() < 6 || proved_bytes[5] != ObjectFile::StaticRamProofFormatVersion ||
            !ObjectFile::readBytes(proved_bytes).hasStaticRamProof())
            throw std::runtime_error("static-RAM proof marker did not round-trip as format version 8");
        proved_bytes[5] = ObjectFile::BaselineFormatVersion;
        expectMemoryFailure(proved_bytes);
        auto misplaced = proved;
        misplaced.symbol_table.back().offset = 2;
        expectWriteFailure(misplaced, directory / "misplaced-proof.o");
        misplaced = proved;
        misplaced.symbol_table.back().section = SymbolSection::CODE;
        expectWriteFailure(misplaced, directory / "code-proof.o");
        misplaced = proved;
        misplaced.config.target = TargetKind::SPC700;
        expectWriteFailure(misplaced, directory / "spc-proof.o");
        misplaced = proved;
        misplaced.code_section = {0xf0, 0x00, 0x00};
        misplaced.relocation_table.push_back({GSUAbi::StaticRamProofSymbol, SymbolSection::CODE, 0, RelocationType::ADDR16_RAM});
        expectWriteFailure(misplaced, directory / "addressed-proof.o");

        // The static stack bound is the version-9 feature: older readers
        // must refuse it, and only one canonical 1..65535 marker is valid.
        const auto stack_marker = [](const std::string& digits) { return GSUAbi::StaticStackBoundPrefix + digits; };
        auto bounded = proved;
        bounded.symbol_table.push_back({stack_marker("1140"), SymbolSection::RAM, 0});
        const auto bounded_path = directory / "stack-bound.o";
        bounded.write(bounded_path.string());
        std::ifstream bounded_input(bounded_path, std::ios::binary);
        auto bounded_bytes = std::vector<std::uint8_t>(std::istreambuf_iterator<char>(bounded_input), {});
        bounded_input.close();
        if (bounded_bytes.size() < 6 || bounded_bytes[5] != ObjectFile::StaticStackFormatVersion ||
            ObjectFile::readBytes(bounded_bytes).staticStackBound() != 1140 ||
            !ObjectFile::readBytes(bounded_bytes).hasStaticRamProof())
            throw std::runtime_error("static stack bound did not round-trip as format version 9");
        if (proved.staticStackBound() != 0) throw std::runtime_error("unmarked object reports a static stack bound");
        bounded_bytes[5] = ObjectFile::StaticRamProofFormatVersion;
        expectMemoryFailure(bounded_bytes);
        for (const auto* digits : {"0", "01140", "65536", "999999", "12a", "-4"}) {
            auto invalid = proved;
            invalid.symbol_table.push_back({stack_marker(digits), SymbolSection::RAM, 0});
            expectWriteFailure(invalid, directory / "invalid-stack-bound.o");
        }
        auto stack_invalid = bounded;
        stack_invalid.symbol_table.back().offset = 2;
        expectWriteFailure(stack_invalid, directory / "misplaced-stack-bound.o");
        stack_invalid = bounded;
        stack_invalid.symbol_table.back().section = SymbolSection::CODE;
        expectWriteFailure(stack_invalid, directory / "code-stack-bound.o");
        stack_invalid = bounded;
        stack_invalid.config.target = TargetKind::SPC700;
        stack_invalid.symbol_table.erase(stack_invalid.symbol_table.begin() + 1); // keep only the stack marker
        expectWriteFailure(stack_invalid, directory / "spc-stack-bound.o");
        stack_invalid = bounded;
        stack_invalid.symbol_table.push_back({stack_marker("2"), SymbolSection::RAM, 0});
        expectWriteFailure(stack_invalid, directory / "twice-stack-bound.o");
        stack_invalid = bounded;
        stack_invalid.code_section = {0xf0, 0x00, 0x00};
        stack_invalid.relocation_table.push_back({stack_marker("1140"), SymbolSection::CODE, 0, RelocationType::ADDR16_RAM});
        expectWriteFailure(stack_invalid, directory / "addressed-stack-bound.o");

        // Short RAM (section, SHORT_RAM symbols, LMS/SMS word-index
        // relocations) is the version-10 feature; older readers refuse it.
        ObjectFile shorted;
        shorted.code_section = {0x3d, 0xa5, 0x00, 0x3e, 0xa0, 0x00, 0x00};
        shorted.short_ram_section = {0x00, 0x00, 0x34, 0x12};
        shorted.symbol_table.push_back({"main", SymbolSection::CODE, 0});
        shorted.symbol_table.push_back({"\x01" "slot", SymbolSection::SHORT_RAM, 2});
        shorted.relocation_table.push_back({"\x01" "slot", SymbolSection::CODE, 1, RelocationType::ADDR8_SHORT_RAM});
        shorted.relocation_table.push_back({"\x01" "slot", SymbolSection::CODE, 4, RelocationType::ADDR8_SHORT_RAM});
        const auto shorted_path = directory / "short-ram.o";
        shorted.write(shorted_path.string());
        std::ifstream shorted_input(shorted_path, std::ios::binary);
        auto shorted_bytes = std::vector<std::uint8_t>(std::istreambuf_iterator<char>(shorted_input), {});
        shorted_input.close();
        const auto reread = ObjectFile::readBytes(shorted_bytes);
        if (shorted_bytes.size() < 6 || shorted_bytes[5] != ObjectFile::ShortRamFormatVersion ||
            reread.short_ram_section != shorted.short_ram_section || reread.symbol_table.at(1).section != SymbolSection::SHORT_RAM ||
            reread.relocation_table.at(1).type != RelocationType::ADDR8_SHORT_RAM || !reread.usesShortRam())
            throw std::runtime_error("short RAM did not round-trip as format version 10");
        if (proved.usesShortRam() || bounded.usesShortRam()) throw std::runtime_error("object without short RAM reports it");
        for (const auto older : {ObjectFile::StaticStackFormatVersion, ObjectFile::BaselineFormatVersion}) {
            auto downgraded = shorted_bytes;
            downgraded[5] = older;
            expectMemoryFailure(downgraded);
        }
        auto short_invalid = shorted;
        short_invalid.short_ram_section.assign(GSUAbi::ShortRamCapacity + 2u, 0);
        expectWriteFailure(short_invalid, directory / "oversized-short-ram.o");
        short_invalid = shorted;
        short_invalid.config.target = TargetKind::SPC700;
        expectWriteFailure(short_invalid, directory / "spc-short-ram.o");
        short_invalid = shorted;
        short_invalid.symbol_table.back().offset = 6;
        expectWriteFailure(short_invalid, directory / "outside-short-ram.o");
        // The word index must patch the operand of ALT1/ALT2 + IBT opcode.
        for (const std::uint32_t patch : {0u, 2u, 3u, 5u}) {
            short_invalid = shorted;
            short_invalid.relocation_table.back().patch_offset = patch;
            expectWriteFailure(short_invalid, directory / "misplaced-short-relocation.o");
        }
        short_invalid = shorted;
        short_invalid.code_section[3] = 0x3f;
        expectWriteFailure(short_invalid, directory / "alt3-short-relocation.o");

        // GSU register entries (defined or referenced) are the version-12
        // feature: older readers refuse them and version 12 requires one.
        ObjectFile registered;
        registered.code_section = {0x94, 0xff, 0x00, 0x00, 0x01, 0x9b, 0x01};
        registered.symbol_table.push_back({"add", SymbolSection::CODE, 5});
        registered.symbol_table.push_back({GSUAbi::registerCallSymbol("add", 2), SymbolSection::CODE, 5});
        registered.relocation_table.push_back({GSUAbi::registerCallSymbol("step", 1), SymbolSection::CODE, 1,
            RelocationType::ADDR16_JAL});
        const auto registered_path = directory / "register-entry.o";
        registered.write(registered_path.string());
        std::ifstream registered_input(registered_path, std::ios::binary);
        auto registered_bytes = std::vector<std::uint8_t>(std::istreambuf_iterator<char>(registered_input), {});
        registered_input.close();
        if (registered_bytes.size() < 6 || registered_bytes[5] != ObjectFile::RegisterCallFormatVersion ||
            !ObjectFile::readBytes(registered_bytes).usesRegisterCalls())
            throw std::runtime_error("register entries did not round-trip as format version 12");
        if (proved.usesRegisterCalls() || shorted.usesRegisterCalls()) throw std::runtime_error("object without register entries reports one");
        for (const auto older : {ObjectFile::SPC700RelocationFormatVersion, ObjectFile::ShortRamFormatVersion,
                                 ObjectFile::BaselineFormatVersion}) {
            auto downgraded = registered_bytes;
            downgraded[5] = older;
            expectMemoryFailure(downgraded);
        }
        auto plain_bytes = registered_bytes;
        {
            auto plain = registered;
            plain.symbol_table.pop_back();
            plain.relocation_table.clear();
            const auto plain_path = directory / "register-plain.o";
            plain.write(plain_path.string());
            std::ifstream plain_input(plain_path, std::ios::binary);
            plain_bytes = std::vector<std::uint8_t>(std::istreambuf_iterator<char>(plain_input), {});
        }
        plain_bytes[5] = ObjectFile::RegisterCallFormatVersion;
        expectMemoryFailure(plain_bytes);
        // The reassembled spelling of a call (IWT relocation) is accepted;
        // a data or RAM use of a register entry is not.
        auto register_variant = registered;
        register_variant.relocation_table.back().type = RelocationType::ADDR16_IWT;
        register_variant.write((directory / "register-iwt.o").string());
        register_variant = registered;
        register_variant.relocation_table.back().type = RelocationType::ADDR16_RAM;
        expectWriteFailure(register_variant, directory / "register-ram.o");
        register_variant = registered;
        register_variant.data_section = {0, 0};
        register_variant.symbol_table.back().section = SymbolSection::DATA;
        register_variant.symbol_table.back().offset = 0;
        expectWriteFailure(register_variant, directory / "register-data.o");
        register_variant = registered;
        register_variant.config.target = TargetKind::SPC700;
        register_variant.relocation_table.clear();
        expectWriteFailure(register_variant, directory / "register-spc.o");
        std::string parsed_name;
        std::size_t parsed_count = 0;
        for (const auto* malformed : {"__disco_regcall0$f", "__disco_regcall5$f", "__disco_regcall2$", "__disco_regcall2f",
                                      "__disco_regcall", "add"})
            if (GSUAbi::parseRegisterCallSymbol(malformed, parsed_name, parsed_count))
                throw std::runtime_error(std::string("malformed register entry name was accepted: ") + malformed);
        if (!GSUAbi::parseRegisterCallSymbol(GSUAbi::registerCallSymbol("a.b", 4), parsed_name, parsed_count) ||
            parsed_name != "a.b" || parsed_count != 4)
            throw std::runtime_error("register entry name did not round-trip");
        short_invalid = shorted;
        short_invalid.relocation_table.back().section_to_patch = SymbolSection::DATA;
        short_invalid.data_section = short_invalid.code_section;
        expectWriteFailure(short_invalid, directory / "data-short-relocation.o");

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

        for (const auto version : {0u, 1u, 2u, ObjectFile::CurrentFormatVersion + 1u, 255u}) {
            auto malformed = valid_bytes; malformed[5] = static_cast<std::uint8_t>(version);
            expectMemoryFailure(malformed);
        }
        for (const auto index : {6u, 7u}) {
            auto malformed = valid_bytes; malformed[index] = 255;
            expectMemoryFailure(malformed);
        }
        for (std::size_t length = 0; length < valid_bytes.size(); ++length)
            expectMemoryFailure({valid_bytes.begin(), valid_bytes.begin() + length});
        auto trailing = valid_bytes; trailing.push_back(0);
        expectMemoryFailure(trailing);
        for (const auto& name : std::vector<std::string>{"", std::string(1, '\x01'),
                std::string("a\0b", 3), "a\nb", "a\tb", std::string("a\x7f", 2),
                std::string(ObjectFile::MaxStringBytes + 1u, 'x')})
            expectMemoryFailure(symbolFixture(name));
        expectMemoryFailure(symbolFixture("x", 255));
        expectMemoryFailure(symbolFixture("x", 0, 2));
        expectMemoryFailure(symbolFixture("x", 0, 0, 2));
        expectMemoryFailure(symbolFixture(std::string(1, '\x01') + "local", 0, 0, 2));
        if (ObjectFile::readBytes(symbolFixture("end", 0, 1)).symbol_table.front().offset != 1)
            throw std::runtime_error("one-past-section labels must remain representable");

        auto impossible_count = objectPrefix();
        appendU32(impossible_count, 0); appendU32(impossible_count, 0);
        appendU32(impossible_count, ObjectFile::MaxSymbolCount); appendU32(impossible_count, 0);
        expectMemoryFailure(impossible_count);
        impossible_count = objectPrefix();
        appendU32(impossible_count, 0); appendU32(impossible_count, 0); appendU32(impossible_count, 0);
        appendU32(impossible_count, ObjectFile::MaxRelocationCount);
        expectMemoryFailure(impossible_count);

        ObjectFile relocations;
        relocations.code_section = {0xa0, 0, 0xf0, 0, 0};
        relocations.data_section = {0x95};
        relocations.symbol_table.push_back({"value", SymbolSection::DATA, 0});
        relocations.relocation_table = {{"value", SymbolSection::CODE, 0, RelocationType::ADDR24_BANK},
            {"value", SymbolSection::CODE, 2, RelocationType::ADDR24_OFFSET}};
        const auto reloc_path = directory / "relocations.o";
        relocations.write(reloc_path.string());
        if (ObjectFile::read(reloc_path.string()).relocation_table.size() != 2)
            throw std::runtime_error("adjacent bank/offset relocations must remain valid");
        auto invalid = relocations;
        invalid.relocation_table.push_back(invalid.relocation_table.back());
        expectWriteFailure(invalid, directory / "preserve.o");
        invalid = relocations; invalid.relocation_table.back().patch_offset = 0;
        expectWriteFailure(invalid, directory / "preserve.o");
        invalid = relocations; invalid.relocation_table.back().patch_offset = 4;
        expectWriteFailure(invalid, directory / "preserve.o");
        invalid = relocations; invalid.relocation_table.back().type = static_cast<RelocationType>(255);
        expectWriteFailure(invalid, directory / "preserve.o");
        invalid = relocations; invalid.relocation_table.back().section_to_patch = SymbolSection::RAM;
        expectWriteFailure(invalid, directory / "preserve.o");
        invalid = relocations; invalid.relocation_table.back().target_symbol_name.clear();
        expectWriteFailure(invalid, directory / "preserve.o");
        invalid = relocations; invalid.symbol_table.push_back(invalid.symbol_table.front());
        expectWriteFailure(invalid, directory / "preserve.o");
        invalid = relocations; invalid.symbol_table.front().offset = 2;
        expectWriteFailure(invalid, directory / "preserve.o");
        invalid = relocations; invalid.symbol_table.front().name = "x\ny";
        expectWriteFailure(invalid, directory / "preserve.o");
        invalid = relocations; invalid.data_alignment = 3;
        expectWriteFailure(invalid, directory / "preserve.o");
        invalid = relocations; invalid.config.target = static_cast<TargetKind>(255);
        expectWriteFailure(invalid, directory / "preserve.o");

        std::vector<std::uint8_t> relocation_bytes;
        { std::ifstream input(reloc_path, std::ios::binary);
          relocation_bytes.assign(std::istreambuf_iterator<char>(input), {}); }
        // Last relocation record: 4-byte length, 5-byte name, section,
        // 4-byte offset, type. Mutate each structural field independently.
        for (const auto index : {relocation_bytes.size() - 6u, relocation_bytes.size() - 5u,
                                 relocation_bytes.size() - 1u}) {
            auto malformed = relocation_bytes; malformed[index] = 255;
            expectMemoryFailure(malformed);
        }
        auto overlapping_bytes = relocation_bytes;
        overlapping_bytes[overlapping_bytes.size() - 5u] = 0;
        expectMemoryFailure(overlapping_bytes);

        const auto huge_path = directory / "total-size.o";
        { std::ofstream output(huge_path, std::ios::binary);
          output.seekp(ObjectFile::MaxObjectBytes); output.put(0);
          if (!output) throw std::runtime_error("cannot create oversized object fixture"); }
        expectReadFailure(huge_path);

        temporary.cleanup();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
