#include "Lexer.hpp"
#include "Parser.hpp"
#include "SPC700Target.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

CompilerConfig parseConfig(const std::string& source) {
    Lexer lexer(source);
    const auto tokens = lexer.scanTokens();
    Parser parser(tokens);
    (void)parser.parseProgram();
    return parser.getConfig();
}

} // namespace

int main() {
    try {
        const auto config = parseConfig("void main() { return; }\n");
        if (config.target != TargetKind::GSU) {
            throw std::runtime_error("default compiler target is not GSU");
        }

        if (SPC700Target::DataLayout::AddressBits != 16 ||
            SPC700Target::DataLayout::PointerBytes != 2 ||
            SPC700Target::DataLayout::HardwareStackBase != 0x0100 ||
            SPC700Target::DataLayout::HardwareStackEnd != 0x01FF) {
            throw std::runtime_error("SPC700 data layout constants are incorrect");
        }
        if (!SPC700Target::isMemoryMappedRegister(0x00F4) ||
            SPC700Target::isMemoryMappedRegister(0x0200)) {
            throw std::runtime_error("SPC700 memory-mapped register range is incorrect");
        }
        if (SPC700Target::Abi::ByteReturnRegister != SPC700Target::Register::A ||
            SPC700Target::Abi::WordReturnLowRegister != SPC700Target::Register::A ||
            SPC700Target::Abi::WordReturnHighRegister != SPC700Target::Register::Y) {
            throw std::runtime_error("SPC700 return ABI model is incorrect");
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
