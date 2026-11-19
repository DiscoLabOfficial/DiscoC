#pragma once

// Startup code that `discld --init-runtime` places at the start of an SPC700
// payload. It implements the runtime contract in SPC700Target.hpp:
//
//   clrp                        ; direct page = page 0
//   mov x,#$FF / mov sp,x       ; empty hardware stack
//   movw $00,ya / movw $02,ya   ; software stack pointer and frame pointer
//   [zero the static RAM image]         (only when there is static RAM)
//   [copy its nonzero span from DATA]   (only when it has nonzero bytes)
//   call !entry                 ; patched after layout
//   mov $F6,a / mov $F7,y       ; report main's YA result
//   mov $F1,#$80 / jmp !$FFC0   ; keep the IPL ROM mapped and return to it
//
// The loops use the scratch bytes $04-$09, which the ABI gives to the
// compiler, so they never touch program-owned direct page. The byte count
// depends only on whether each optional part is present, never on addresses,
// so the linker can size the startup before layout and fill it in afterwards.

#include "SPC700Target.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

struct SPC700RuntimeLayout {
    std::uint16_t stack_top = SPC700Target::Abi::DefaultStackTop;
    std::uint16_t ram_start = 0;
    std::uint32_t ram_bytes = 0;      // Static RAM cleared before `main`.
    std::uint16_t copy_source = 0;    // Nonzero initial bytes, stored in DATA,
    std::uint16_t copy_destination = 0;
    std::uint32_t copy_bytes = 0;     // copied over the cleared RAM.
};

struct SPC700RuntimeInitialization {
    std::vector<std::uint8_t> bytes;
    std::size_t entry_patch = 0; // Low byte of the CALL operand.
};

// Range [first, first + bytes) of an image that holds every nonzero byte.
struct SPC700NonzeroSpan {
    std::size_t first = 0;
    std::size_t bytes = 0;
};

inline SPC700NonzeroSpan spc700NonzeroSpan(const std::vector<std::uint8_t>& image) {
    SPC700NonzeroSpan span;
    std::size_t begin = 0;
    while (begin < image.size() && image[begin] == 0) ++begin;
    if (begin == image.size()) return span;
    std::size_t end = image.size();
    while (image[end - 1] == 0) --end;
    span.first = begin;
    span.bytes = end - begin;
    return span;
}

inline SPC700RuntimeInitialization makeSPC700RuntimeInitialization(const SPC700RuntimeLayout& layout) {
    using SPC700Target::Abi;
    if (layout.stack_top > SPC700Target::DataLayout::IplRomBase)
        throw std::runtime_error("SPC700 software stack top must not exceed $FFC0.");
    // End addresses are 16-bit loop bounds, so they must stay below $10000.
    if (layout.ram_bytes > 0xFFFFu - layout.ram_start ||
        layout.copy_bytes > 0xFFFFu - layout.copy_destination || layout.copy_bytes > 0xFFFFu - layout.copy_source)
        throw std::runtime_error("SPC700 runtime initialization exceeds the 64 KiB address space.");
    SPC700RuntimeInitialization result;
    auto& out = result.bytes;
    out.reserve(80);
    const auto loadYa = [&](std::uint32_t value) {
        out.push_back(0xE8); out.push_back(static_cast<std::uint8_t>(value));      // mov a,#lo
        out.push_back(0x8D); out.push_back(static_cast<std::uint8_t>(value >> 8)); // mov y,#hi
    };
    const auto storeYa = [&](std::uint8_t dp) { out.push_back(0xDA); out.push_back(dp); }; // movw dp,ya

    out.insert(out.end(), {0x20, 0xCD, 0xFF, 0xBD}); // clrp; mov x,#$FF; mov sp,x
    loadYa(layout.stack_top);
    storeYa(Abi::SoftwareStackPointer);
    storeYa(Abi::FramePointer);

    if (layout.ram_bytes != 0) {
        // $04 = cursor, $06 = end; do { [cursor] = 0 } while (++cursor != end)
        loadYa(layout.ram_start);
        storeYa(0x04);
        loadYa(layout.ram_start + layout.ram_bytes);
        storeYa(0x06);
        out.insert(out.end(), {0xE8, 0x00, 0x8D, 0x00, 0xD7, 0x04, 0x3A, 0x04,
                               0xBA, 0x04, 0x5A, 0x06, 0xD0, 0xF2});
    }
    if (layout.copy_bytes != 0) {
        // $04 = source, $06 = destination, $08 = destination end.
        loadYa(layout.copy_source);
        storeYa(0x04);
        loadYa(layout.copy_destination);
        storeYa(0x06);
        loadYa(layout.copy_destination + layout.copy_bytes);
        storeYa(0x08);
        out.insert(out.end(), {0x8D, 0x00, 0xF7, 0x04, 0xD7, 0x06, 0x3A, 0x04,
                               0x3A, 0x06, 0xBA, 0x06, 0x5A, 0x08, 0xD0, 0xF0});
    }
    out.push_back(0x3F); // call !entry
    result.entry_patch = out.size();
    out.insert(out.end(), {0x00, 0x00});
    out.insert(out.end(), {0xC4, static_cast<std::uint8_t>(SPC700Target::Runtime::ResultPortLow),
                           0xCB, static_cast<std::uint8_t>(SPC700Target::Runtime::ResultPortHigh),
                           0x8F, SPC700Target::Runtime::ControlIplMapped,
                           static_cast<std::uint8_t>(SPC700Target::Runtime::Control),
                           0x5F, 0xC0, 0xFF});
    return result;
}
