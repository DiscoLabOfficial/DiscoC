#pragma once

#include <cstdint>

enum class TargetKind : std::uint8_t {
    GSU = 0,
    SPC700 = 1
};

// Target placement is part of the object-file contract.  Keeping it in a
// small standalone header lets both the compiler and linker share the same
// representation without making ObjectFile depend on Parser.
enum class MemoryMapping : std::uint8_t {
    LoROM = 0,
    HiROM = 1
};

struct CompilerConfig {
    TargetKind target = TargetKind::GSU;
    MemoryMapping mapping = MemoryMapping::LoROM;
    std::uint32_t code_start_address = 0x8000;
    bool optimize_loop_setup = false;
    bool warn_on_cache_overflow = true;
};

inline bool operator==(const CompilerConfig& lhs, const CompilerConfig& rhs) {
    return lhs.target == rhs.target &&
           lhs.mapping == rhs.mapping &&
           lhs.code_start_address == rhs.code_start_address;
}

inline bool operator!=(const CompilerConfig& lhs, const CompilerConfig& rhs) {
    return !(lhs == rhs);
}

inline const char* targetName(TargetKind target) {
    switch (target) {
        case TargetKind::GSU: return "GSU";
        case TargetKind::SPC700: return "SPC700";
    }
    return "unknown";
}
