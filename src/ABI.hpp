#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Stable compiler-side names for the GSU calling convention. The convention
// retains the scalar stack-frame layout; far argument slots are four bytes.
namespace GSUAbi {
constexpr const char* StackLimitPrefix = "__disco_stack_limit_";

constexpr std::uint8_t ReturnValueRegister = 0;
// Far returns: R0 = offset, R4 = zero-extended bank. R6 is a volatile
// temporary; on a checked-address STOP it reports the failure category.
constexpr std::uint8_t FarBankRegister = 4;
constexpr std::uint8_t AddressFaultRegister = 6;
constexpr const char* NearRamBankSymbol = "__disco_near_ram_bank";
constexpr const char* NearRomBankSymbol = "__disco_near_rom_bank";
// Private DATA in a payload copied to the selected near RAM execution bank.
// Linkers must reject these RAM relocations for ROM or mismatched placement.
constexpr const char* ImmutablePoolPrefix = "\x01" "__disco_pool_";
// Private RAM marker at offset 0: this object's code omits null/wrap checks
// for its own static RAM objects. The linker rejects such an object whose
// static RAM segment starts at address zero. Objects carrying it are written
// as format version 8, which older linkers refuse instead of ignoring.
constexpr const char* StaticRamProofSymbol = "\x01" "__disco_static_ram_nonnull";
// Private RAM marker at offset 0, suffixed by a decimal byte count N: this
// object omits the stack guards of `main` and of the internal functions only
// it reaches, because none can fail when `main` starts at the runtime's
// initial SP with N bytes above the stack floor. The linker must prove that
// (runtime initialization, `main` as entry and otherwise unreferenced, floor +
// N <= SP) or reject the object. Carried only by format version 9 objects.
constexpr const char* StaticStackBoundPrefix = "\x01" "__disco_static_stack_";
// Short RAM: the near RAM bank's $0000-$01FF, the only range LMS/SMS reach
// (ALT1/ALT2 + IBT opcode, operand = word address / 2). The linker places
// every object's short RAM section from $0002 so that address zero stays a
// null pointer; the window then holds 510 bytes.
constexpr std::uint16_t ShortRamBase = 0x0002;
constexpr std::uint16_t ShortRamEnd = 0x0200;
constexpr std::uint16_t ShortRamCapacity = ShortRamEnd - ShortRamBase;
constexpr std::uint8_t FramePointerRegister = 9;
constexpr std::uint8_t StackPointerRegister = 10;
constexpr std::uint8_t LinkRegister = 11;
constexpr std::uint8_t LoopCountRegister = 12;
constexpr std::uint8_t LoopAddressRegister = 13;
constexpr std::uint8_t RomAddressRegister = 14;
constexpr std::uint8_t ProgramCounterRegister = 15;

constexpr std::size_t StackAlignment = 2;
constexpr std::size_t ParameterSlotSize = 2;
// PUSH stores before decrementing SP. FP therefore points to the empty slot
// below saved R9 (FP+2) and saved R11 (FP+4).
constexpr int FirstParameterOffset = 6;
// Module-private O2/Os convention, never visible to other objects: an
// internal function with 1..4 one-word parameters (no address taken)
// receives them in these registers instead of a stack argument area. None is
// touched by LINK, the prologue (R0/R3/R9-R11) or its stack/alignment guards.
// The caller writes them, so they belong to the callee's clobber set.
constexpr std::size_t RegisterArgumentCount = 4;
constexpr std::uint8_t RegisterArguments[RegisterArgumentCount] = {5, 7, 8, 4};

// Public register entry, `__disco_regcall<N>$<name>` (N = 1..4): an O2/Os
// definition of public `name` whose N parameters are one-word values also
// exports this CODE symbol. It takes the arguments in RegisterArguments and
// otherwise behaves exactly like `name`, which keeps the stack convention for
// every other caller. `$` cannot occur in a source identifier, so the name
// never collides with a function. An O2/Os caller of an external `name` with
// such a prototype calls the register entry; when no input object defines
// it, the linker supplies an adapter that pushes the registers as stack
// arguments and calls `name`. Objects that define or reference a register
// entry are written as format version 12, which older linkers refuse.
constexpr const char* RegisterCallPrefix = "__disco_regcall";
inline std::string registerCallSymbol(const std::string& name, std::size_t count) {
    return std::string(RegisterCallPrefix) + std::to_string(count) + "$" + name;
}
// Splits a register-entry symbol into its target name and argument count.
// Returns false for any other symbol, including malformed register names.
inline bool parseRegisterCallSymbol(const std::string& symbol, std::string& name, std::size_t& count) {
    const std::string prefix(RegisterCallPrefix);
    if (symbol.size() < prefix.size() + 3 || symbol.compare(0, prefix.size(), prefix) != 0) return false;
    const char digit = symbol[prefix.size()];
    if (digit < '1' || digit > '0' + static_cast<int>(RegisterArgumentCount) || symbol[prefix.size() + 1] != '$') return false;
    count = static_cast<std::size_t>(digit - '0');
    name = symbol.substr(prefix.size() + 2);
    return true;
}

// R9 and R11 are preserved by every non-entry function: a frameless one never
// touches R9 unless it is module-private and its in-module callers save R9
// per its call-clobber mask. Allocated value registers are caller-saved and
// must be protected around calls when their values remain live. R0, R1, R3,
// R4, and R6 are volatile backend registers.
constexpr std::uint8_t FirstAllocatedValueRegister = 5;

} // namespace GSUAbi
