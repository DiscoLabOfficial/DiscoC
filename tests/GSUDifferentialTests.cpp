#include "differential/IntegerOracle.hpp"
#include "GSUInstructionModel.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace O = DifferentialOracle;
using O::Word;
constexpr unsigned ValidCases = 24;
constexpr unsigned CaseCount = ValidCases + 8;
constexpr unsigned WindowStart = 0x100;
constexpr unsigned WindowBytes = 256;
constexpr unsigned InitialStack = 0x3000;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string hex(std::uint32_t value) {
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
Expression expression(Random& random, const std::array<Word, 3>& inputs, unsigned depth) {
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

std::string commonMain() {
    return "u16 main() {\n"
        "  u16 a = *(volatile u16*)0x100;\n"
        "  u16 b = *(volatile u16*)0x102;\n"
        "  u16 c = *(volatile u16*)0x104;\n"
        "  u16 sh = *(volatile u16*)0x106;\n"
        "  *(volatile u16*)0x120 = 7;\n";
}

void validProgram(Case& test) {
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

void faultProgram(Case& test) {
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

Case makeCase(std::uint32_t seed, unsigned index) {
    Case result({seed, index});
    if (index < ValidCases) validProgram(result); else faultProgram(result);
    result.source = "// Differential seed=" + std::to_string(seed) + " case=" + std::to_string(index) + "\n" + result.source;
    return result;
}

void check(const Case& test, const DiscoGSU::Machine& machine) {
    require(machine.reg(6) == test.fault, "fault R6: expected " + std::to_string(test.fault) + ", got " + std::to_string(machine.reg(6)));
    if (!test.fault) {
        require(machine.reg(0) == test.result, "return R0: expected " + std::to_string(test.result) + ", got " + std::to_string(machine.reg(0)));
        require(machine.reg(10) == InitialStack - 4, "main/callee stack restoration changed");
    }
    require(machine.ramBank() == test.bank, "near RAM bank was not restored");
    const std::uint32_t base = 0x700000u + static_cast<std::uint32_t>(test.bank) * 65536u;
    const std::uint32_t other = 0x700000u + static_cast<std::uint32_t>(test.bank ^ 1u) * 65536u;
    for (const auto null_address : {base, other})
        require(!machine.accesses(null_address, false) && !machine.accesses(null_address, true),
                "illegal near-null RAM access at " + hex(null_address));
    for (unsigned n = 0; n < WindowBytes; ++n) {
        const auto address = base + WindowStart + n;
        require(machine.byte(address) == test.expected[n], "RAM " + hex(address) + ": expected " +
            std::to_string(test.expected[n]) + ", got " + std::to_string(machine.byte(address)));
        require(machine.accesses(address, false) == test.reads[n], "read count at " + hex(address) + ": expected " +
            std::to_string(test.reads[n]) + ", got " + std::to_string(machine.accesses(address, false)));
        require(machine.accesses(address, true) == test.writes[n], "write count at " + hex(address) + ": expected " +
            std::to_string(test.writes[n]) + ", got " + std::to_string(machine.accesses(address, true)));
        require(machine.byte(other + WindowStart + n) == test.other_bank[n] &&
            !machine.accesses(other + WindowStart + n, false) && !machine.accesses(other + WindowStart + n, true),
            "foreign-bank sentinel/access changed at " + hex(other + WindowStart + n));
    }
}

void seedMachine(const Case& test, DiscoGSU::Machine& machine) {
    for (unsigned n = 0; n < WindowBytes; ++n) {
        machine.seed(0x700000u + static_cast<std::uint32_t>(test.bank) * 65536u + WindowStart + n, test.initial[n], false);
        machine.seed(0x700000u + static_cast<std::uint32_t>(test.bank ^ 1u) * 65536u + WindowStart + n, test.other_bank[n], false);
    }
}

std::ofstream openArtifact(const std::string& path) {
    std::ofstream out;
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.open(path, std::ios::binary);
    return out;
}
void emit(const Case& test, const std::string& source, const std::string& expectation) {
    auto source_file = openArtifact(source);
    source_file << test.source;
    source_file.close();
    std::ostringstream out;
    out << "seed=" << test.seed << "\ncase=" << test.index << "\norigin=" << hex(test.origin)
        << "\nram_bank=" << static_cast<unsigned>(test.bank) << "\nexpected_fault_R6=" << test.fault << '\n';
    if (!test.fault) out << "expected_return_R0=" << test.result << "\nexpected_SP=" << hex(InitialStack - 4) << '\n';
    out << "# address initial-byte final-byte read-count write-count\n";
    const auto base = 0x700000u + static_cast<std::uint32_t>(test.bank) * 65536u;
    for (unsigned n = 0; n < WindowBytes; ++n)
        out << hex(base + WindowStart + n) << ' ' << static_cast<unsigned>(test.initial[n]) << ' '
            << static_cast<unsigned>(test.expected[n]) << ' ' << test.reads[n] << ' ' << test.writes[n] << '\n';
    auto expectation_file = openArtifact(expectation);
    expectation_file << out.str();
    expectation_file.close();
}

std::uint32_t number(const char* text, std::uint32_t maximum) {
    const std::string value(text);
    require(!value.empty() && value.front() != '-' && value.front() != '+', "invalid numeric argument");
    std::size_t consumed = 0;
    const auto decoded = std::stoull(value, &consumed, 0);
    require(consumed == value.size() && decoded <= maximum, "numeric argument out of range");
    return static_cast<std::uint32_t>(decoded);
}
std::vector<std::uint8_t> readPayload(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    require(static_cast<bool>(in), "cannot read differential payload: " + path);
    const auto length = in.tellg();
    require(length > 0 && length <= 65536, "invalid differential payload size");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    in.seekg(0);
    require(static_cast<bool>(in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))),
            "truncated differential payload");
    return bytes;
}

void selfTest() {
    require(O::add(65535, 1) == 0 && O::subtract(0, 1) == 65535 && O::multiply(65535, 65535) == 1 &&
        O::negate(32768) == 32768 && O::negate(1) == 65535,
        "oracle modular arithmetic anchors");
    require(O::signedWord(32768) == -32768 && O::signedWord(65535) == -1, "oracle signed decoding anchors");
    require(O::divide(32768, 65535, true) == 32768 && O::remainder(32768, 65535, true) == 0 &&
        O::divide(65529, 3, true) == 65534 && O::remainder(65529, 3, true) == 65535 &&
        O::divide(7, 65533, true) == 65534 && O::remainder(7, 65533, true) == 1 &&
        O::divide(65535, 2, false) == 32767, "oracle quotient/remainder anchors");
    require(O::arithmeticRight(32769, 0) == 32769 && O::arithmeticRight(32769, 15) == 65535 &&
        O::arithmeticRight(65533, 1) == 65534 && O::logicalRight(32768, 15) == 1 &&
        O::leftShift(32769, 1) == 2, "oracle shift anchors");
    require(O::signedByte(128) == 65408 && O::signedByte(255) == 65535 && O::byte(65408) == 128 &&
        O::signedByte(O::negateByte(128)) == 65408 && O::signedByte(O::complementByte(0)) == 65535,
        "oracle cast/unary-byte anchors");
    unsigned caught = 0;
    try { O::divide(1, 0, false); } catch (const std::domain_error&) { ++caught; }
    try { O::remainder(1, 0, true); } catch (const std::domain_error&) { ++caught; }
    try { O::leftShift(1, 16); } catch (const std::domain_error&) { ++caught; }
    try { O::arithmeticRight(1, 65535); } catch (const std::domain_error&) { ++caught; }
    require(caught == 4, "oracle must reject checked arithmetic faults");
    for (unsigned index = 0; index < CaseCount; ++index) {
        const auto a = makeCase(0xd15c0c32u, index), b = makeCase(0xd15c0c32u, index);
        require(a.source == b.source && a.initial == b.initial && a.expected == b.expected &&
            a.reads == b.reads && a.writes == b.writes && a.result == b.result && a.fault == b.fault,
            "generator is nondeterministic");
    }
    require(makeCase(1, 0).source != makeCase(2, 0).source, "seed does not affect generated programs");
    // A real hand-encoded STOP payload anchors the checker and deliberate
    // corruptions prove that a mismatch cannot silently count as success.
    Case correct({1, 0});
    correct.result = 42;
    DiscoGSU::Machine machine({0xf0, 42, 0, 0xfa, 0xfc, 0x2f, 0, 1}, correct.origin);
    seedMachine(correct, machine);
    machine.run();
    check(correct, machine);
    unsigned mismatches = 0;
    for (unsigned kind = 0; kind < 6; ++kind) {
        auto wrong = correct;
        if (kind == 0) wrong.result = 41;
        else if (kind == 1) wrong.fault = 6;
        else if (kind == 2) wrong.expected[32] ^= 1;
        else if (kind == 3) wrong.reads[0] = 1;
        else if (kind == 4) wrong.writes[0] = 1;
        else wrong.other_bank[0] ^= 1;
        try { check(wrong, machine); } catch (const std::runtime_error&) { ++mismatches; }
    }
    require(mismatches == 6, "checker accepted deliberately corrupted expectations");
    DiscoGSU::Machine null_read({0xf1, 0, 0, 0x41, 0xf0, 42, 0, 0xfa, 0xfc, 0x2f, 0, 1}, correct.origin);
    seedMachine(correct, null_read);
    null_read.run();
    bool detected_null = false;
    try { check(correct, null_read); }
    catch (const std::runtime_error& error) { detected_null = std::string(error.what()).find("near-null") != std::string::npos; }
    require(detected_null, "checker accepted a near-null read before STOP");
    std::cout << "Independent integer oracle, deterministic generation and mismatch checks passed\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--self-test")) { selfTest(); return 0; }
        if (argc == 6 && std::string(argv[1]) == "--emit") {
            const auto test = makeCase(number(argv[4], std::numeric_limits<std::uint32_t>::max()), number(argv[5], CaseCount - 1));
            emit(test, argv[2], argv[3]);
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--check") {
            const auto test = makeCase(number(argv[3], std::numeric_limits<std::uint32_t>::max()), number(argv[4], CaseCount - 1));
            DiscoGSU::Machine machine(readPayload(argv[2]), test.origin, 0x9000, static_cast<std::uint8_t>(test.bank ^ 1u));
            seedMachine(test, machine);
            machine.run(); // Returns only after STOP, with a hard instruction budget.
            check(test, machine);
            std::cout << "seed=" << test.seed << " case=" << test.index << " origin=" << hex(test.origin)
                << " RAMBR=" << static_cast<unsigned>(test.bank) << " STOP/RAM/volatile/return/fault passed\n";
            return 0;
        }
        throw std::runtime_error("usage: --self-test | --emit source.dc expected.txt seed case | --check payload.bin seed case");
    } catch (const std::exception& error) {
        std::cerr << "Differential failure: " << error.what() << '\n';
        return 1;
    }
}
