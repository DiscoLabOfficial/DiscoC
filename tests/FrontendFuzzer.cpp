#include "Assembler.hpp"
#include "Lexer.hpp"
#include "ObjectFile.hpp"
#include "Parser.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string input(reinterpret_cast<const char*>(data), size);

    try {
        Lexer lexer(input);
        const auto tokens = lexer.scanTokens();
        Parser parser(tokens);
        (void)parser.parseProgram();
    } catch (...) {
        // Invalid source is an expected fuzzing result.
    }

    try {
        Assembler assembler;
        (void)assembler.assemble(input);
    } catch (...) {
        // Invalid assembly is an expected fuzzing result.
    }

    try {
        std::vector<std::uint8_t> bytes;
        if (size > 0) {
            bytes.assign(data, data + size);
        }
        (void)ObjectFile::readBytes(bytes);
    } catch (...) {
        // Invalid object bytes are an expected fuzzing result.
    }

    return 0;
}
