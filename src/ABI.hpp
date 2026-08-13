#pragma once

#include <cstddef>
#include <cstdint>

// Stable compiler-side names for the GSU calling convention. The convention
// intentionally remains compatible with the existing stack-frame layout.
namespace GSUAbi {

constexpr std::uint8_t ReturnValueRegister = 0;
constexpr std::uint8_t FramePointerRegister = 9;
constexpr std::uint8_t StackPointerRegister = 10;
constexpr std::uint8_t LinkRegister = 11;
constexpr std::uint8_t LoopCountRegister = 12;
constexpr std::uint8_t LoopAddressRegister = 13;
constexpr std::uint8_t RomAddressRegister = 14;
constexpr std::uint8_t ProgramCounterRegister = 15;

constexpr std::size_t StackAlignment = 2;
constexpr std::size_t ParameterSlotSize = 2;
constexpr int FirstParameterOffset = 4;

// R9 and R11 are saved by every non-entry function. Allocated value registers
// are caller-saved and must be protected around calls when their values remain
// live. R0, R1, and R3 are volatile backend registers.
constexpr std::uint8_t FirstAllocatedValueRegister = 5;

} // namespace GSUAbi
