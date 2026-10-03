#pragma once

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>

// This is the GSU's address space, not the SNES CPU's cartridge/WRAM map.
// Reference: Super NES Programming / Super FX tutorial, "Memory Map".
// Only documented windows are supported; lower-half ROM mirrors are not
// relied on. Cartridge RAM capacity and bus ownership remain host concerns.
namespace GsuMemoryMap {

enum class Region { Unmapped, Rom, Ram };

inline std::string addressText(std::uint32_t address) {
    std::ostringstream text;
    text << '$' << std::hex << std::uppercase << address;
    return text.str();
}

inline std::uint32_t bank(std::uint32_t address) noexcept {
    return address >> 16;
}

inline Region region(std::uint32_t address) noexcept {
    if (address > 0xffffffu) return Region::Unmapped;
    const auto address_bank = bank(address);
    const auto offset = address & 0xffffu;
    if ((address_bank <= 0x3fu && offset >= 0x8000u) ||
        (address_bank >= 0x40u && address_bank <= 0x5fu)) {
        return Region::Rom;
    }
    if (address_bank == 0x70u || address_bank == 0x71u) return Region::Ram;
    return Region::Unmapped;
}

inline void validateAddress(std::uint32_t address) {
    if (region(address) == Region::Unmapped) {
        throw std::runtime_error(
            "GSU address " + addressText(address) + " is outside supported ROM/RAM regions: "
            "$00-$3F:$8000-$FFFF, $40-$5F:$0000-$FFFF, or $70-$71:$0000-$FFFF.");
    }
}

inline void validatePayload(std::uint32_t origin, std::uint64_t code_bytes,
                            std::uint64_t data_bytes) {
    validateAddress(origin);
    const std::uint64_t capacity = 0x10000u - (origin & 0xffffu);
    // Subtract before comparing: even hostile sizes cannot overflow a sum.
    if (code_bytes > capacity || data_bytes > capacity - code_bytes) {
        throw std::runtime_error(
            "GSU payload crosses a program-bank boundary; multi-bank placement is not supported.");
    }
}

inline void validateNearTarget(std::uint32_t origin, std::uint32_t target) {
    validateAddress(origin);
    validateAddress(target);
    // IWT R15/JAL does not change PBR. A nonzero bank is valid, but a
    // different bank cannot be encoded by truncating the target to 16 bits.
    if (bank(origin) != bank(target)) {
        throw std::runtime_error("16-bit GSU relocation targets a different bank (" +
            addressText(origin) + " -> " + addressText(target) + ").");
    }
}

} // namespace GsuMemoryMap
