#pragma once
#include "GsuMemoryMap.hpp"
#include "TargetConfig.hpp"
#include <string>

struct PlacementOptions {
    MemoryMapping mapping = MemoryMapping::LoROM;
    std::uint32_t origin = 0x8000;
    bool explicit_origin = false;
    enum class Execution { Automatic, Rom, Ram };
    Execution execution = Execution::Automatic;
};

inline void applyPlacement(CompilerConfig& config, const PlacementOptions& options) {
    config.mapping = options.mapping;
    config.code_start_address = options.explicit_origin ? options.origin :
        options.execution == PlacementOptions::Execution::Ram ? 0x708000 :
        options.mapping == MemoryMapping::LoROM ? 0x8000 : 0x408000;
    if (config.code_start_address > 0xffffff) throw std::runtime_error("Origin must fit in 24 bits.");
    if (config.target != TargetKind::GSU) {
        if (options.explicit_origin || options.mapping != MemoryMapping::LoROM || options.execution != PlacementOptions::Execution::Automatic)
            throw std::runtime_error("GSU placement options are not supported for this target.");
        return;
    }
    if (options.execution != PlacementOptions::Execution::Automatic) {
        const auto expected = options.execution == PlacementOptions::Execution::Ram ? GsuMemoryMap::Region::Ram : GsuMemoryMap::Region::Rom;
        if (GsuMemoryMap::region(config.code_start_address) != expected)
            throw std::runtime_error("Origin does not match selected execution memory.");
    }
    GsuMemoryMap::validateAddress(config.code_start_address);
}
