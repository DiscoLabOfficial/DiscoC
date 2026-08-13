#pragma once

#include <cstdint>

// Architectural facts used by the future SPC-700 backend. This header is
// deliberately independent from the GSU ABI and from the parser so that
// target-neutral IR passes can include it without code-generation coupling.
namespace SPC700Target {

enum class Register : std::uint8_t {
    A,
    X,
    Y,
    YA,
    SP,
    PC,
    PSW
};

struct DataLayout {
    static constexpr std::uint32_t AddressBits = 16;
    static constexpr std::uint32_t AddressBytes = 2;
    static constexpr std::uint32_t ByteBytes = 1;
    static constexpr std::uint32_t WordBytes = 2;
    static constexpr std::uint32_t PointerBytes = 2;
    static constexpr std::uint32_t AddressSpaceBytes = 0x10000;

    static constexpr std::uint16_t DirectPageBytes = 0x0100;
    static constexpr std::uint16_t HardwareStackBase = 0x0100;
    static constexpr std::uint16_t HardwareStackEnd = 0x01FF;
    static constexpr std::uint16_t MemoryMappedRegisterBase = 0x00F0;
    static constexpr std::uint16_t MemoryMappedRegisterEnd = 0x00FF;
    static constexpr std::uint16_t IplRomBase = 0xFFC0;
    static constexpr std::uint16_t IplRomEnd = 0xFFFF;
};

struct Abi {
    // Initial scalar return conventions for the future compiler backend.
    static constexpr Register ByteReturnRegister = Register::A;
    static constexpr Register WordReturnLowRegister = Register::A;
    static constexpr Register WordReturnHighRegister = Register::Y;

    // A, X and Y are caller-clobbered value registers. SP, PC and PSW remain
    // architectural state and are never allocated as ordinary IR values.
    static constexpr std::uint32_t ScalarArgumentAlignment = 1;
    static constexpr std::uint32_t WordArgumentAlignment = 2;
};

constexpr bool isMemoryMappedRegister(std::uint16_t address) {
    return address >= DataLayout::MemoryMappedRegisterBase &&
           address <= DataLayout::MemoryMappedRegisterEnd;
}

} // namespace SPC700Target
