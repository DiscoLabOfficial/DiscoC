#pragma once
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <vector>

struct RuntimeInitialization {
    std::vector<std::uint8_t> bytes;
    std::size_t entry_patch = 0;
};

inline RuntimeInitialization makeRuntimeInitialization(std::uint8_t bank, std::uint16_t stack,
    std::uint16_t ram_origin, const std::vector<std::uint8_t>& initial_image) {
    if (bank > 1 || initial_image.size() > 65536u - ram_origin || (ram_origin & 1) != 0)
        throw std::runtime_error("Runtime RAM initialization exceeds its aligned bank allocation.");
    RuntimeInitialization result;
    const auto word = [&](std::uint8_t reg, std::uint16_t value) {
        result.bytes.push_back(static_cast<std::uint8_t>(0xf0 | reg));
        result.bytes.push_back(static_cast<std::uint8_t>(value));
        result.bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    };
    const auto literal = [&](std::uint8_t reg, std::uint16_t value) {
        // IBT always sign-extends. Select it by the resulting 16-bit pattern,
        // not by source signedness: $FE is $FFFE, never $00FE.
        if (value <= 0x7f || value >= 0xff80) {
            result.bytes.push_back(static_cast<std::uint8_t>(0xa0 | reg));
            result.bytes.push_back(static_cast<std::uint8_t>(value));
        } else word(reg, value);
    };
    literal(0, bank);
    result.bytes.insert(result.bytes.end(), {0x3e, 0xdf});
    literal(10, stack);
    if (!initial_image.empty()) {
        literal(0, 0);
        literal(1, ram_origin);
        literal(2, static_cast<std::uint16_t>(initial_image.size()));
        // A byte loop also handles odd-size objects and exactly 64 KiB
        // (zero count wraps once). No ROM access is needed for RAM execution.
        result.bytes.insert(result.bytes.end(), {0x3d, 0x31, 0xd1, 0xe2, 0x08, 0xfa, 0x01});
        for (std::size_t offset = 0; offset < initial_image.size(); offset += 2) {
            const auto low = initial_image[offset];
            const auto high = offset + 1 < initial_image.size() ? initial_image[offset + 1] : 0;
            if (low == 0 && high == 0) continue;
            literal(1, static_cast<std::uint16_t>(ram_origin + offset));
            literal(0, static_cast<std::uint16_t>(low | (high << 8)));
            if (offset + 1 == initial_image.size()) result.bytes.push_back(0x3d);
            result.bytes.push_back(0x31);
        }
    }
    result.entry_patch = result.bytes.size() + 1;
    // The execution address is patched after layout, so it must stay IWT.
    word(15, 0);
    result.bytes.push_back(1);
    return result;
}
