#pragma once

#include <cstdint>
#include <stdexcept>

// Test-only Baseline 0.1 integer semantics. Do not include compiler types,
// folding helpers or NumericSemantics here: they are what this oracle tests.
namespace DifferentialOracle {
using Word = std::uint16_t;

inline Word wrap(std::uint64_t value) { return static_cast<Word>(value & 65535u); }
inline std::int32_t signedWord(Word bits) {
    return static_cast<std::int32_t>(bits) - (bits >= 32768 ? 65536 : 0);
}
inline Word add(Word a, Word b) { return wrap(static_cast<std::uint32_t>(a) + b); }
inline Word subtract(Word a, Word b) { return wrap(static_cast<std::uint32_t>(a) + 65536u - b); }
inline Word multiply(Word a, Word b) { return wrap(static_cast<std::uint64_t>(a) * b); }
inline Word negate(Word a) { return subtract(0, a); }
inline Word complement(Word a) { return static_cast<Word>(a ^ 65535u); }
inline Word byte(Word a) { return static_cast<Word>(a & 255u); }
inline Word signedByte(Word a) {
    const auto low = byte(a);
    return static_cast<Word>(low | (low >= 128 ? 65280u : 0u));
}
inline Word negateByte(Word a) { return byte(negate(byte(a))); }
inline Word complementByte(Word a) { return byte(complement(byte(a))); }

inline Word leftShift(Word a, unsigned count) {
    if (count > 15) throw std::domain_error("oracle: invalid shift count");
    return wrap(static_cast<std::uint32_t>(a) << count);
}
inline Word logicalRight(Word a, unsigned count) {
    if (count > 15) throw std::domain_error("oracle: invalid shift count");
    return static_cast<Word>(static_cast<std::uint32_t>(a) >> count);
}
inline Word arithmeticRight(Word a, unsigned count) {
    if (count > 15) throw std::domain_error("oracle: invalid shift count");
    if (count == 0) return a;
    const auto fill = a >= 32768 ? (65535u << (16u - count)) : 0u;
    return wrap((static_cast<std::uint32_t>(a) >> count) | fill);
}
inline Word divide(Word a, Word b, bool is_signed) {
    if (!b) throw std::domain_error("oracle: division by zero");
    // Widening before / also defines the guest's -32768 / -1 case without
    // overflowing a host signed integer. Unsigned conversion is modulo 2^32.
    if (is_signed)
        return wrap(static_cast<std::uint32_t>(signedWord(a) / signedWord(b)));
    return static_cast<Word>(a / b);
}
inline Word remainder(Word a, Word b, bool is_signed) {
    if (!b) throw std::domain_error("oracle: remainder by zero");
    if (is_signed)
        return wrap(static_cast<std::uint32_t>(signedWord(a) % signedWord(b)));
    return static_cast<Word>(a % b);
}
} // namespace DifferentialOracle
