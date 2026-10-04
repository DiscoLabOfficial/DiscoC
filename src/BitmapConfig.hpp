#pragma once

#include <cstdint>
#include <stdexcept>

// Host-side SCMR/SCBR configuration, never an instruction stream for the GSU.
struct BitmapConfig {
    bool enabled = false;
    bool object_mode = false;
    std::uint16_t height = 128;
    std::uint8_t depth = 4;
    std::uint32_t base = 0;

    std::uint32_t sizeBytes() const {
        return 256u * (object_mode ? 256u : height) * depth / 8u;
    }
    std::uint8_t scbr() const { return static_cast<std::uint8_t>(base >> 10); }
    // RON/RAN ownership bits are deliberately left to the SNES host.
    std::uint8_t scmr() const {
        const unsigned h = object_mode ? 0x24 : height == 192 ? 0x20 : height == 160 ? 4 : 0;
        return static_cast<std::uint8_t>(h | (depth == 8 ? 3 : depth == 4 ? 1 : 0));
    }
    void validate() const {
        if (!enabled) return;
        if ((!object_mode && height != 128 && height != 160 && height != 192) ||
            (object_mode && height != 256) || (depth != 2 && depth != 4 && depth != 8))
            throw std::runtime_error("Invalid SuperFX bitmap mode, size or depth.");
        if ((base & 1023u) || base >= 131072u || sizeBytes() > 131072u - base)
            throw std::runtime_error("SuperFX bitmap base must be SCBR-aligned (1024 bytes) and its framebuffer must fit in cartridge RAM.");
    }
};

inline bool operator==(const BitmapConfig& a, const BitmapConfig& b) {
    return a.enabled == b.enabled && (!a.enabled || (a.object_mode == b.object_mode &&
        a.height == b.height && a.depth == b.depth && a.base == b.base));
}
inline bool operator!=(const BitmapConfig& a, const BitmapConfig& b) { return !(a == b); }
