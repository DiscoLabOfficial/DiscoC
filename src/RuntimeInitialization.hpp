#pragma once
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <vector>
#include "ABI.hpp"

struct RuntimeInitialization {
    std::vector<std::uint8_t> bytes;
    std::size_t entry_patch = 0;
};

// Clears and fills the static RAM image at `ram_origin` and, if present, the
// short RAM image at GSUAbi::ShortRamBase, both in RAM bank `bank`.
inline RuntimeInitialization makeRuntimeInitialization(std::uint8_t bank, std::uint16_t stack,
    std::uint16_t ram_origin, const std::vector<std::uint8_t>& initial_image,
    const std::vector<std::uint8_t>& short_image = {}) {
    if (bank > 1 || initial_image.size() > 65536u - ram_origin || (ram_origin & 1) != 0 ||
        short_image.size() > GSUAbi::ShortRamCapacity)
        throw std::runtime_error("Runtime RAM initialization exceeds its aligned bank allocation.");
    RuntimeInitialization result;
    // Validation bounds the images to 64 KiB plus the short window. Startup
    // and footer use at most 11 bytes and each region's clear at most 16;
    // each pair needs at most two IWTs + STW (7), and an odd tail adds one
    // ALT1. Reserve before emitting so append operations never grow the
    // allocation (also avoids GCC 13's vector-insert overflow warning).
    result.bytes.reserve(43u + 7u * ((initial_image.size() + 1u) / 2u) + 7u * ((short_image.size() + 1u) / 2u));
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
    const auto region = [&](std::uint16_t origin, const std::vector<std::uint8_t>& image) {
        if (image.empty()) return;
        literal(0, 0);
        literal(1, origin);
        // Clear whole words: STW (R1) / INC R1 / DEC R2 / BNE / INC R1. The
        // delay slot runs on both paths, so R1 advances by two per word and
        // ends on the odd tail byte, if any. The origin is even and a full
        // 64 KiB image is 32768 words, so the count is never zero here.
        const auto words = image.size() / 2u;
        if (words != 0) {
            literal(2, static_cast<std::uint16_t>(words));
            result.bytes.insert(result.bytes.end(), {0x31, 0xd1, 0xe2, 0x08, 0xfb, 0xd1});
        }
        if ((image.size() & 1u) != 0) result.bytes.insert(result.bytes.end(), {0x3d, 0x31});
        // No ROM access is needed for RAM execution.
        for (std::size_t offset = 0; offset < image.size(); offset += 2) {
            const bool has_high_byte = image.size() - offset > 1;
            const auto low = image[offset];
            const auto high = has_high_byte ? image[offset + 1] : 0;
            if (low == 0 && high == 0) continue;
            literal(1, static_cast<std::uint16_t>(origin + offset));
            literal(0, static_cast<std::uint16_t>(low | (high << 8)));
            if (!has_high_byte) result.bytes.push_back(0x3d);
            result.bytes.push_back(0x31);
        }
    };
    literal(0, bank);
    result.bytes.insert(result.bytes.end(), {0x3e, 0xdf});
    literal(10, stack);
    region(ram_origin, initial_image);
    region(GSUAbi::ShortRamBase, short_image);
    result.entry_patch = result.bytes.size() + 1;
    // The execution address is patched after layout, so it must stay IWT.
    word(15, 0);
    result.bytes.push_back(1);
    return result;
}
