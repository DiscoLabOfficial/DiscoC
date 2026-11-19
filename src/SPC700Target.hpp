#pragma once

#include <cstdint>

// Architectural facts and the runtime/ABI contract of the SPC-700 target. This
// header is deliberately independent from the GSU ABI and from the parser so
// that target-neutral IR passes can include it without code-generation coupling.
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

    // Default load address of a linked payload: the first byte above the
    // direct page and the hardware stack page.
    static constexpr std::uint16_t DefaultOrigin = 0x0200;
};

// Calling convention and memory ownership shared by the linker, the runtime
// startup and the future code generator. See docs/spc700-target.md.
//
// Arguments and locals live on a software stack that grows downward in
// ordinary RAM; the hardware stack in page 1 holds only return addresses and
// balanced PUSH/POP pairs. The caller pushes arguments and pops them after
// the call. The callee saves the caller's frame pointer and addresses its
// frame through `[FramePointer]+Y`, which limits one frame to 256 bytes.
struct Abi {
    static constexpr Register ByteReturnRegister = Register::A;
    static constexpr Register WordReturnLowRegister = Register::A;
    static constexpr Register WordReturnHighRegister = Register::Y;

    // A, X, Y, PSW and the scratch bytes are clobbered by a call; the
    // software stack pointer and frame pointer are preserved. PSW.P is clear
    // at every call boundary, so the direct page is page 0.
    static constexpr std::uint32_t ScalarArgumentAlignment = 1;
    static constexpr std::uint32_t WordArgumentAlignment = 2;

    // Compiler-owned direct-page bytes $00-$1F.
    static constexpr std::uint8_t SoftwareStackPointer = 0x00; // word: last allocated byte
    static constexpr std::uint8_t FramePointer = 0x02;         // word
    static constexpr std::uint8_t ScratchBegin = 0x04;         // caller-saved
    static constexpr std::uint8_t ScratchEnd = 0x1F;           // inclusive
    static constexpr std::uint16_t CompilerDirectPageEnd = 0x0020; // exclusive

    // The software stack starts empty at its top, which must not exceed the
    // IPL ROM base, and may grow down to the end of the linked image.
    static constexpr std::uint16_t DefaultStackTop = DataLayout::IplRomBase;

    // `__disco_stack_limit_N` resolves to the lowest software-stack address
    // plus N, as on GSU; a function needing N bytes compares against it.
    static constexpr const char* StackLimitPrefix = "__disco_stack_limit_";
};

// Runtime contract: the startup code returns `main`'s YA result on ports 2/3
// ($F6/$F7) and jumps back to the IPL ROM, which then announces $AA/$BB on
// ports 0/1 again. The host reads the result and may start the program again.
struct Runtime {
    static constexpr std::uint16_t ResultPortLow = 0x00F6;
    static constexpr std::uint16_t ResultPortHigh = 0x00F7;
    static constexpr std::uint16_t Control = 0x00F1;
    static constexpr std::uint8_t ControlIplMapped = 0x80;
};

// Address ranges no linked SPC700 code, data or static RAM may occupy.
struct ReservedRegion {
    std::uint16_t begin;
    std::uint32_t end; // exclusive
    const char* name;
};

constexpr ReservedRegion MemoryMappedRegisters{DataLayout::MemoryMappedRegisterBase, 0x0100u, "the I/O registers ($00F0-$00FF)"};
constexpr ReservedRegion HardwareStack{DataLayout::HardwareStackBase, 0x0200u, "the hardware stack page ($0100-$01FF)"};
constexpr ReservedRegion IplRom{DataLayout::IplRomBase, 0x10000u, "the IPL ROM area ($FFC0-$FFFF)"};
constexpr ReservedRegion CompilerDirectPage{0x0000, Abi::CompilerDirectPageEnd, "the runtime's direct page ($0000-$001F)"};

constexpr bool isMemoryMappedRegister(std::uint16_t address) {
    return address >= DataLayout::MemoryMappedRegisterBase &&
           address <= DataLayout::MemoryMappedRegisterEnd;
}

} // namespace SPC700Target
