#pragma once

// Generator of deterministic integer/control-flow/volatile differential
// programs, shared by the GSU and SPC700 differential runners. Programs
// observe memory through a 256-byte window at WindowStart; a target whose
// memory map reserves that range relocates the window in the generated text
// (see SPC700DifferentialTests.cpp). Expectations are indexed by window
// offset, so they do not depend on where the window lives.

#include "IntegerOracle.hpp"

#include <array>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>

namespace DifferentialCases {
namespace O = DifferentialOracle;
using O::Word;
constexpr unsigned ValidCases = 24;
constexpr unsigned CaseCount = ValidCases + 8;
constexpr unsigned WindowStart = 0x100;
constexpr unsigned WindowBytes = 256;

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
inline std::string hex(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

class Random {
public:
    explicit Random(std::uint32_t seed) : state_(seed ? seed : 0x6d2b79f5u) {}
    std::uint32_t next() {
        // Specified xorshift32, not implementation-dependent distributions.
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }
private:
    std::uint32_t state_;
};

struct Expression { std::string source; Word expected; };
inline Expression expression(Random& random, const std::array<Word, 3>& inputs, unsigned depth) {
    if (depth == 0) {
        const auto leaf = random.next() % 4;
        if (leaf < inputs.size()) return {std::string(1, static_cast<char>('a' + leaf)), inputs.at(leaf)};
        const auto value = O::wrap(random.next());
        return {"(u16)" + std::to_string(value), value};
    }
    const auto left = expression(random, inputs, depth - 1);
    const auto right = expression(random, inputs, depth - 1);
    std::string operation, rhs = right.source;
    Word value = 0;
    switch (random.next() % 10) {
        case 0: operation = "+"; value = O::add(left.expected, right.expected); break;
        case 1: operation = "-"; value = O::subtract(left.expected, right.expected); break;
        case 2: operation = "*"; value = O::multiply(left.expected, right.expected); break;
        case 3: operation = "^"; value = static_cast<Word>(left.expected ^ right.expected); break;
        case 4: operation = "&"; value = static_cast<Word>(left.expected & right.expected); break;
        case 5: operation = "|"; value = static_cast<Word>(left.expected | right.expected); break;
        case 6: operation = "<<"; rhs = "(" + rhs + " & 15)";
            value = O::leftShift(left.expected, right.expected & 15); break;
        case 7: operation = ">>"; rhs = "(" + rhs + " & 15)";
            value = O::logicalRight(left.expected, right.expected & 15); break;
        case 8: operation = "/"; rhs = "(" + rhs + " | 1)";
            value = O::divide(left.expected, static_cast<Word>(right.expected | 1), false); break;
        default: operation = "%"; rhs = "(" + rhs + " | 1)";
            value = O::remainder(left.expected, static_cast<Word>(right.expected | 1), false); break;
    }
    return {"(" + left.source + " " + operation + " " + rhs + ")", value};
}

struct Identity { std::uint32_t seed; unsigned index; };
struct Cell { unsigned address; Word value; };

struct Case {
    std::uint32_t seed;
    unsigned index;
    std::uint32_t origin;
    std::uint8_t bank;
    std::string source;
    std::array<std::uint8_t, WindowBytes> initial{}, expected{}, other_bank{};
    std::array<unsigned, WindowBytes> reads{}, writes{};
    Word result = 0, fault = 0;

    explicit Case(Identity identity)
        : seed(identity.seed), index(identity.index), origin(index % 2 ? 0x706000u : 0x008000u),
          bank(static_cast<std::uint8_t>((index / 2) & 1u)) {
        require(index < CaseCount, "case index must be 0..31");
        for (unsigned n = 0; n < WindowBytes; ++n) {
            initial[n] = static_cast<std::uint8_t>((n * 37u + (seed & 255u) + index * 11u) & 255u);
            other_bank[n] = static_cast<std::uint8_t>(initial[n] ^ 0xa5u);
        }
        expected = initial;
    }
    static std::size_t position(unsigned address) {
        require(address >= WindowStart && address + 1 < WindowStart + WindowBytes && !(address & 1),
                "oracle observation is outside its aligned window");
        return address - WindowStart;
    }
    void input(Cell cell) {
        const auto n = position(cell.address);
        initial[n] = expected[n] = static_cast<std::uint8_t>(cell.value & 255);
        initial[n + 1] = expected[n + 1] = static_cast<std::uint8_t>(cell.value >> 8);
    }
    void store(Cell cell) {
        const auto n = position(cell.address);
        expected[n] = static_cast<std::uint8_t>(cell.value & 255);
        expected[n + 1] = static_cast<std::uint8_t>(cell.value >> 8);
        ++writes[n];
    }
    void read(unsigned address) { ++reads[position(address)]; }
};

inline std::string commonMain() {
    return "u16 main() {\n"
        "  u16 a = *(volatile u16*)0x100;\n"
        "  u16 b = *(volatile u16*)0x102;\n"
        "  u16 c = *(volatile u16*)0x104;\n"
        "  u16 sh = *(volatile u16*)0x106;\n"
        "  *(volatile u16*)0x120 = 7;\n";
}

inline void validProgram(Case& test) {
    Random random(test.seed ^ ((test.index + 1u) * 0x9e3779b9u));
    constexpr std::array<Word, 14> edges{{0, 1, 2, 15, 16, 127, 128, 255, 256, 32767, 32768, 32769, 65534, 65535}};
    constexpr std::array<Word, 7> shifts{{0, 1, 7, 8, 9, 14, 15}};
    const Word a = test.index < edges.size() ? edges.at(test.index) : O::wrap(random.next());
    const Word b = test.index == 10 ? Word{65535} : test.index < edges.size() ?
        edges.at((test.index * 5u + 3u) % edges.size()) : O::wrap(random.next());
    const Word c = test.index < edges.size() ? edges.at((test.index * 3u + 1u) % edges.size()) : O::wrap(random.next());
    const Word shift = shifts.at(test.index % shifts.size());
    for (unsigned n = 0; n < 4; ++n) test.read(0x100 + n * 2);
    test.input({0x100, a}); test.input({0x102, b}); test.input({0x104, c}); test.input({0x106, shift});
    test.input({0x150, 0});
    const auto state = random;
    const auto first_expression = expression(random, {{a, b, c}}, 3);
    auto second_random = state;
    const auto second_expression = expression(second_random, {{c, a, b}}, 3);
    require(first_expression.source == second_expression.source, "expression shape depends on input values");
    const auto first = static_cast<Word>(first_expression.expected ^ second_expression.expected);
    const auto divisor = static_cast<Word>(b | 1u);
    const auto quotient = O::divide(a, divisor, true), remainder = O::remainder(a, divisor, true);
    const auto shifted = O::arithmeticRight(a, shift);
    auto casts = O::add(O::add(O::signedByte(a), O::byte(b)),
        O::add(O::signedByte(O::complementByte(c)), O::signedByte(O::negateByte(b))));
    casts = O::add(casts, O::add(O::negate(a), O::add(O::add(a, b), O::multiply(a, c))));
    std::array<Word, 4> items{{a, b, c, first}};
    Word sum = 0;
    for (unsigned n = 0; n < items.size(); ++n) {
        items[n] = O::add(items[n], O::add(a, static_cast<Word>(n)));
        if (n != 2) sum = static_cast<Word>(sum ^ items[n]);
    }
    const auto array_result = O::add(sum, items.at(c & 3));
    const unsigned iterations = 3 + test.index % 5;
    const auto counter = O::multiply(c, static_cast<Word>(iterations));
    auto branch = O::signedWord(a) < O::signedWord(b) ? O::add(first, a) : O::subtract(first, b);
    branch = c & 1 ? static_cast<Word>(branch ^ array_result) : O::add(branch, counter);
    switch (c & 3) {
        case 0: branch = O::add(branch, 1); break;
        case 1: branch = static_cast<Word>(branch ^ b); break;
        case 2: branch = O::subtract(branch, b); break;
        default: branch = O::add(branch, b); break;
    }
    // Persist each mark's value at its observed sequence position, not just a
    // checksum that could hide reordering through collisions.
    unsigned marks = 0;
    const auto mark = [&](Word value) {
        test.read(0x150); test.store({0x160 + marks * 2, value});
        test.store({0x150, static_cast<Word>(++marks)});
    };
    mark(a); mark(b); mark(c);
    const bool picked = c != 0 && O::add(c, 1) != 0;
    if (c != 0) mark(O::add(c, 1));
    const auto flags = static_cast<Word>(4 + (picked ? 2 : 0));
    const auto ordered = static_cast<Word>(O::add(a, b) ^ c);
    test.result = static_cast<Word>(first ^ quotient ^ remainder ^ shifted ^ casts ^ array_result ^ branch ^ ordered ^ counter ^ flags);
    for (const auto& output : std::array<Cell, 12>{{
        {0x120, 7}, {0x122, first}, {0x124, quotient}, {0x126, remainder}, {0x128, shifted}, {0x12a, casts},
        {0x12c, array_result}, {0x12e, branch}, {0x130, ordered}, {0x132, counter}, {0x134, 0xa55a}, {0x136, test.result}}})
        test.store(output);

    test.source =
        "u16 mix(u16 a, u16 b, u16 c) { return " + first_expression.source + "; }\n"
        "u16 nested(u16 a, u16 b, u16 c) { return mix(a,b,c) ^ mix(c,a,b); }\n"
        "u16 choose(u16 k, u16 x, u16 y) {\n"
        "  switch(k) { case 0: return x+1; case 1: return x^y; case 2: return x-y; default: return x+y; }\n}\n"
        "u16 mark(u16 value) {\n"
        "  u16 n = *(volatile u16*)0x150;\n"
        "  *(volatile u16*)(0x160 + n*2) = value;\n"
        "  *(volatile u16*)0x150 = n+1; return value;\n}\n"
        "u16 join(u16 a,u16 b,u16 c) { return (a+b)^c; }\n" + commonMain() +
        "  u16 first = nested(a,b,c);\n"
        "  *(volatile u16*)0x122 = first;\n"
        "  u16 quotient = (u16)((i16)a / (i16)(b | 1));\n"
        "  u16 rem = (u16)((i16)a % (i16)(b | 1));\n"
        "  *(volatile u16*)0x124 = quotient; *(volatile u16*)0x126 = rem;\n"
        "  u16 shifted = (u16)((i16)a >> sh);\n"
        "  *(volatile u16*)0x128 = shifted;\n"
        "  u16 casts = (u16)(i16)(i8)a + (u16)(u8)b + (u16)(i16)(i8)(~(u8)c) + (u16)(i16)(-(i8)b)"
        " + (u16)(-(i16)a) + (u16)((i16)a+(i16)b) + (u16)((i16)a*(i16)c);\n"
        "  *(volatile u16*)0x12a = casts;\n"
        "  u16 items[4] = {a,b,c,first}; u16 sum = 0;\n"
        "  for (u16 i=0;i<4;i++) { items[i] += a+i; if (i==2) continue; sum ^= items[i]; }\n"
        "  u16 array_result = sum + items[c & 3];\n"
        "  *(volatile u16*)0x12c = array_result;\n"
        "  u16 counter = 0; for (word n=" + std::to_string(iterations) + "; n>0; n=n-1) { counter += c; }\n"
        "  *(volatile u16*)0x132 = counter;\n"
        "  u16 branch; if ((i16)a < (i16)b) branch=first+a; else branch=first-b;\n"
        "  if (c & 1) branch ^= array_result; else branch += counter;\n"
        "  branch = choose(c & 3,branch,b); *(volatile u16*)0x12e = branch;\n"
        "  u16 ordered = join(mark(a),mark(b),mark(c));\n"
        "  *(volatile u16*)0x130 = ordered;\n"
        "  bool skipped = (a != a) && (mark(7) != 0);\n"
        "  bool picked = (c != 0) && (mark(c+1) != 0);\n"
        "  bool short_or = (b == b) || (mark(9) != 0);\n"
        "  u16 flags = (u16)skipped + ((u16)picked << 1) + ((u16)short_or << 2);\n"
        "  u16 result = first ^ quotient ^ rem ^ shifted ^ casts ^ array_result ^ branch ^ ordered ^ counter ^ flags;\n"
        "  *(volatile u16*)0x136 = result; *(volatile u16*)0x134 = 0xa55a; return result;\n}\n";
}

inline void faultProgram(Case& test) {
    const auto kind = test.index - ValidCases;
    Word a = 32768, b = 0;
    std::string operation;
    switch (kind) {
        case 0: operation = "a / b"; test.fault = 6; break;
        case 1: operation = "(u16)((i16)a / (i16)b)"; test.fault = 6; break;
        case 2: operation = "a % b"; test.fault = 6; break;
        case 3: operation = "(u16)((i16)a % (i16)b)"; test.fault = 6; break;
        case 4: operation = "a << b"; b = 16; test.fault = 5; break;
        case 5: operation = "(u16)((i16)a >> (i16)b)"; b = 65535; test.fault = 5; break;
        case 6: operation = "*(u16*)a"; a = 0x101; test.fault = 1; break;
        default: operation = "*(u16*)a"; a = 0; test.fault = 2; break;
    }
    test.input({0x100, a}); test.input({0x102, b}); test.read(0x100);
    test.source = "u16 main() {\n  u16 a = *(volatile u16*)0x100;\n";
    if (kind < 6) { test.source += "  u16 b = *(volatile u16*)0x102;\n"; test.read(0x102); }
    test.source += "  *(volatile u16*)0x120 = 7;\n  u16 result = " + operation + ";\n"
        "  *(volatile u16*)0x122 = 9; return result;\n}\n";
    test.store({0x120, 7});
}

inline Case makeCase(std::uint32_t seed, unsigned index) {
    Case result({seed, index});
    if (index < ValidCases) validProgram(result); else faultProgram(result);
    result.source = "// Differential seed=" + std::to_string(seed) + " case=" + std::to_string(index) + "\n" + result.source;
    return result;
}

} // namespace DifferentialCases
