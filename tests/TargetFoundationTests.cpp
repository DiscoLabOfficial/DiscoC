#include "Lexer.hpp"
#include "Parser.hpp"
#include "SPC700Target.hpp"
#include "CompilerError.hpp"
#include "Placement.hpp"

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
        expectDiagnostic("set execution_memory = ram;", "Source-level set configuration");
        expectDiagnostic("set memory_mapping = hirom;", "Source-level set configuration");
        expectDiagnostic("set code_start_address = 0x710000;", "Source-level set configuration");
        for (const auto mapping : {MemoryMapping::LoROM, MemoryMapping::HiROM}) {
            for (const auto memory : {PlacementOptions::Execution::Automatic, PlacementOptions::Execution::Rom, PlacementOptions::Execution::Ram}) {
                PlacementOptions options; options.mapping = mapping; options.execution = memory;
                CompilerConfig placement; applyPlacement(placement, options);
                const auto expected = memory == PlacementOptions::Execution::Ram ? 0x708000u :
                    mapping == MemoryMapping::LoROM ? 0x8000u : 0x408000u;
                if (placement.mapping != mapping || placement.code_start_address != expected)
                    throw std::runtime_error("Incorrect CLI placement defaults.");
                options.origin = memory == PlacementOptions::Execution::Ram ? 0x710000u : 0x409000u;
                options.explicit_origin = true; applyPlacement(placement, options);
                if (placement.code_start_address != options.origin) throw std::runtime_error("Explicit origin was lost.");
            }
        }
        for (const auto origin : {0x1000000u, 0x7e8000u, 0x8000u}) {
            PlacementOptions options; options.origin = origin; options.explicit_origin = true; options.execution = PlacementOptions::Execution::Ram;
            bool rejected = false;
            try { CompilerConfig placement; applyPlacement(placement, options); }
            catch (const std::runtime_error&) { rejected = true; }
            if (!rejected) throw std::runtime_error("Invalid placement accepted.");
        }
        PlacementOptions spc_options; spc_options.execution = PlacementOptions::Execution::Ram;
        bool rejected = false;
        try { CompilerConfig placement; placement.target = TargetKind::SPC700; applyPlacement(placement, spc_options); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected) throw std::runtime_error("GSU options accepted for SPC700.");

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
