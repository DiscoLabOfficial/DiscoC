#include "ObjectFile.hpp"

#include <filesystem>
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

std::vector<std::uint8_t> objectPrefix() {
    std::vector<std::uint8_t> bytes{'D', 'I', 'S', 'C', 'O', ObjectFile::CurrentFormatVersion, 0};
    appendU32(bytes, 0x8000);
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
        valid.code_section = {0x01, 0x02};
        valid.data_section = {0x03};
        valid.symbol_table.push_back({"entry", SymbolSection::CODE, 0});
        const auto valid_path = directory / "valid.o";
        valid.write(valid_path.string());
        const auto loaded = ObjectFile::read(valid_path.string());
        if (loaded.code_section != valid.code_section ||
            loaded.data_section != valid.data_section ||
            loaded.symbol_table.size() != 1 ||
            loaded.symbol_table.front().name != "entry") {
            throw std::runtime_error("valid object did not round-trip");
        }

        std::ifstream valid_input(valid_path, std::ios::binary);
        const std::vector<std::uint8_t> valid_bytes(
            std::istreambuf_iterator<char>(valid_input), {});
        const auto loaded_from_memory = ObjectFile::readBytes(valid_bytes);
        if (loaded_from_memory.code_section != valid.code_section ||
            loaded_from_memory.data_section != valid.data_section) {
            throw std::runtime_error("valid object did not load from memory");
        }
        valid_input.close();

        std::filesystem::remove_all(directory);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
