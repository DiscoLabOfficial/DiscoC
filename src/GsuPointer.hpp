#pragma once

#include <cstdint>
#include <stdexcept>

#include "Types.hpp"

// Address policy shared by semantic constant checking and its unit tests.
// ROM aliases remain separate domains: LoROM uses 32 KiB windows, the
// $40-$5F full-bank view uses 64 KiB windows. Neither wraps into the other.
namespace GsuPointer {
inline void validate(std::uint32_t address, AddressSpace space, bool far, int width, int alignment = 0) {
    if (width <= 0 || width > 65536) throw std::runtime_error("Invalid pointer access width.");
    if (alignment == 0) alignment = width == 1 ? 1 : 2;
    const auto bank = address >> 16;
    const auto offset = address & 0xffffu;
    if (address > 0xffffffu || (!far && bank != 0))
        throw std::runtime_error("Pointer address exceeds its representation.");
    if (far) {
        if (space == AddressSpace::ROM) {
            if (bank > 0x5f || (bank <= 0x3f && offset < 0x8000))
                throw std::runtime_error("Pointer address is outside GSU-visible ROM.");
        } else if (bank != 0x70 && bank != 0x71) {
            throw std::runtime_error("Pointer address is outside GSU-visible RAM.");
        }
    }
    if (!far && address == 0) throw std::runtime_error("Null pointer access is illegal.");
    if (alignment < 1 || alignment > 128 || (alignment & (alignment - 1))) throw std::runtime_error("Invalid pointer alignment.");
    if (alignment > 1 && (offset & static_cast<std::uint32_t>(alignment - 1)))
        throw std::runtime_error("Misaligned 16-bit pointer address is illegal.");
    if (static_cast<std::uint32_t>(width) > 65536u - offset)
        throw std::runtime_error("Pointer access crosses a bank boundary.");
}

inline std::uint32_t add(std::uint32_t address, std::int64_t index, int stride,
                         AddressSpace space, bool far, int alignment = 0) {
    validate(address, space, far, stride, alignment);
    // Source displacements are at most one 16-bit integer. Multiplication
    // therefore cannot overflow int64_t even for the largest legal stride.
    if (index < -65535 || index > 65535) throw std::runtime_error("Pointer displacement exceeds 16 bits.");
    const std::int64_t offset = address & 0xffffu;
    const std::int64_t displaced = offset + index * stride;
    const auto bank = address >> 16;
    if (stride > 1 || !far) {
        const auto lower = far && space == AddressSpace::ROM && bank <= 0x3f ? 0x8000 : 0;
        if (displaced < lower || displaced > 65536 - stride)
            throw std::runtime_error("Pointer arithmetic crosses a bank boundary.");
        const auto result = (address & 0xff0000u) | static_cast<std::uint32_t>(displaced);
        validate(result, space, far, stride, alignment);
        return result;
    }
    const bool lorom = space == AddressSpace::ROM && bank <= 0x3f;
    const std::int64_t first_bank = space == AddressSpace::RAM ? 0x70 : (lorom ? 0 : 0x40);
    const std::int64_t bank_count = space == AddressSpace::RAM ? 2 : (lorom ? 64 : 32);
    const std::int64_t window = lorom ? 0x8000 : 0x10000;
    const std::int64_t lower = lorom ? 0x8000 : 0;
    const std::int64_t linear = (bank - first_bank) * window + offset - lower + index;
    if (linear < 0 || linear >= bank_count * window)
        throw std::runtime_error("Pointer arithmetic exceeds the accessible bank domain.");
    return static_cast<std::uint32_t>(((first_bank + linear / window) << 16) |
                                      (lower + linear % window));
}
} // namespace GsuPointer
