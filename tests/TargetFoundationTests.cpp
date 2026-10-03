#include "Lexer.hpp"
#include "Parser.hpp"
#include "SPC700Target.hpp"
#include "CompilerError.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

CompilerConfig parseConfig(const std::string& source, TargetKind target = TargetKind::GSU) {
    Lexer lexer(source);
    const auto tokens = lexer.scanTokens();
    Parser parser(tokens);
    parser.getConfigForUpdate().target = target;
    (void)parser.parseProgram();
    return parser.getConfig();
}

void expectPlacement(const std::string& directives, MemoryMapping mapping, std::uint32_t origin) {
    const auto config = parseConfig(directives + "void main() { return; }\n");
    if (config.mapping != mapping || config.code_start_address != origin) {
        throw std::runtime_error("incorrect placement for: " + directives);
    }
}

void expectDiagnostic(const std::string& source, const std::string& diagnostic,
                      TargetKind target = TargetKind::GSU) {
    try {
        (void)parseConfig(source, target);
    } catch (const CompilerError& error) {
        if (std::string(error.what()).find(diagnostic) != std::string::npos &&
            error.getLine() > 0 && error.getCol() > 0) return;
        throw std::runtime_error("unexpected placement diagnostic: " + std::string(error.what()));
    }
    throw std::runtime_error("invalid placement was accepted: " + source);
}

} // namespace

int main() {
    try {
        const auto config = parseConfig("void main() { return; }\n");
        if (config.target != TargetKind::GSU) {
            throw std::runtime_error("default compiler target is not GSU");
        }

        expectPlacement("", MemoryMapping::LoROM, 0x008000);
        expectPlacement("set execution_memory = rom;", MemoryMapping::LoROM, 0x008000);
        expectPlacement("set memory_mapping = lorom;", MemoryMapping::LoROM, 0x008000);
        expectPlacement("set memory_mapping = hirom;", MemoryMapping::HiROM, 0x408000);
        expectPlacement("set execution_memory = ram;", MemoryMapping::LoROM, 0x708000);
        expectPlacement("set execution_memory = ram; set memory_mapping = lorom;",
                        MemoryMapping::LoROM, 0x708000);
        expectPlacement("set memory_mapping = hirom; set execution_memory = ram;",
                        MemoryMapping::HiROM, 0x708000);
        expectPlacement("set execution_memory = ram; set memory_mapping = hirom;",
                        MemoryMapping::HiROM, 0x708000);
        expectPlacement("set execution_memory = ram; set execution_memory = rom;",
                        MemoryMapping::LoROM, 0x008000);
        expectPlacement("set code_start_address = 0x710000; set execution_memory = ram;",
                        MemoryMapping::LoROM, 0x710000);
        expectPlacement("set execution_memory = ram; set code_start_address = 0x710000;",
                        MemoryMapping::LoROM, 0x710000);
        expectPlacement("set code_start_address = 0x709000; set memory_mapping = lorom; set execution_memory = ram;",
                        MemoryMapping::LoROM, 0x709000);
        expectPlacement("set code_start_address = 0x409000; set memory_mapping = hirom;",
                        MemoryMapping::HiROM, 0x409000);
        // Existing full-address placement remains valid without the new selector.
        expectPlacement("set code_start_address = 0x708000;", MemoryMapping::LoROM, 0x708000);
        expectDiagnostic("set execution_memory = wram;", "'rom' or 'ram'");
        expectDiagnostic("set execution_memory = ram", "Expect ';'");
        expectDiagnostic("set execution_memory = ram; set code_start_address = 0x8000;",
                         "does not match selected execution memory");
        expectDiagnostic("set code_start_address = 0x708000; set execution_memory = rom;",
                         "does not match selected execution memory");
        expectDiagnostic("set execution_memory = ram; set code_start_address = 0x7E8000;",
                         "does not match selected execution memory");
        expectDiagnostic("set execution_memory = ram;", "only supported for GSU", TargetKind::SPC700);

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
