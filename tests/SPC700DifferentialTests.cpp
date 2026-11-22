// SPC700 side of the integer/control-flow/volatile differential tests. The
// programs and their independent expectations come from the generator shared
// with the GSU runner; on SPC700 the observation window moves from $0100 (the
// hardware stack page) to $C000, which only changes addresses in the source.

#include "differential/DifferentialCases.hpp"
#include "SPC700InstructionModel.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace DifferentialCases;

constexpr unsigned SpcWindow = 0xC000;
constexpr std::uint16_t Origin = 0x0200;
constexpr std::uint16_t StackTop = 0xFFC0;

// Window addresses appear in generated source only as 0x1XX literals.
std::string relocate(const std::string& source) {
    static const std::regex window("0x1([0-9a-f]{2})\\b");
    return std::regex_replace(source, window, "0xc0$1");
}

Case spcCase(std::uint32_t seed, unsigned index) {
    auto test = makeCase(seed, index);
    test.source = relocate(test.source);
    return test;
}

void seedMachine(const Case& test, DiscoSPC700::Machine& machine) {
    for (unsigned n = 0; n < WindowBytes; ++n)
        machine.seed(static_cast<std::uint16_t>(SpcWindow + n), test.initial[n]);
}

void check(const Case& test, const DiscoSPC700::Machine& machine) {
    if (test.fault) {
        require(machine.stopped() && machine.outputPort(0) == 0xFA,
                "expected fault " + std::to_string(test.fault) + ", but the program did not stop through the fault channel");
        require(machine.outputPort(1) == test.fault, "fault code: expected " + std::to_string(test.fault) + ", got " +
                std::to_string(machine.outputPort(1)));
    } else {
        require(machine.returnedToIpl(), "program did not return to the IPL");
        const auto ya = static_cast<unsigned>(machine.a() | (machine.y() << 8));
        require(ya == test.result, "return YA: expected " + std::to_string(test.result) + ", got " + std::to_string(ya));
        require(machine.word(0x0000) == StackTop && machine.word(0x0002) == StackTop && machine.sp() == 0xFF,
                "software stack, frame pointer or hardware stack not restored");
    }
    // The generator observes aligned words only. A GSU word access is one
    // access counted at the even address; the 8-bit SPC700 performs it as
    // two byte accesses, so both bytes must show the word's count.
    for (unsigned n = 0; n < WindowBytes; ++n) {
        const auto address = static_cast<std::uint16_t>(SpcWindow + n);
        const auto word = n & ~1u;
        require(machine.byte(address) == test.expected[n], "RAM " + hex(address) + ": expected " +
            std::to_string(test.expected[n]) + ", got " + std::to_string(machine.byte(address)));
        require(machine.reads(address) == test.reads[word], "read count at " + hex(address) + ": expected " +
            std::to_string(test.reads[word]) + ", got " + std::to_string(machine.reads(address)));
        require(machine.writes(address) == test.writes[word], "write count at " + hex(address) + ": expected " +
            std::to_string(test.writes[word]) + ", got " + std::to_string(machine.writes(address)));
    }
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    require(static_cast<bool>(out), "cannot write " + path);
}

void emit(const Case& test, const std::string& source, const std::string& expectation) {
    writeFile(source, test.source);
    std::ostringstream out;
    out << "seed=" << test.seed << "\ncase=" << test.index << "\norigin=" << hex(Origin)
        << "\nexpected_fault=" << test.fault << '\n';
    if (!test.fault) out << "expected_return_YA=" << test.result << '\n';
    out << "# address initial-byte final-byte read-count write-count\n";
    for (unsigned n = 0; n < WindowBytes; ++n)
        out << hex(SpcWindow + n) << ' ' << static_cast<unsigned>(test.initial[n]) << ' '
            << static_cast<unsigned>(test.expected[n]) << ' ' << test.reads[n] << ' ' << test.writes[n] << '\n';
    writeFile(expectation, out.str());
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
    std::ifstream in(path, std::ios::binary);
    require(static_cast<bool>(in), "cannot read differential payload: " + path);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    require(!bytes.empty() && bytes.size() <= 0x10000u - Origin, "invalid differential payload size");
    return bytes;
}

void selfTest() {
    for (unsigned index = 0; index < CaseCount; ++index) {
        const auto test = spcCase(0xd15c0c32u, index);
        require(test.source == spcCase(0xd15c0c32u, index).source, "relocated generation is nondeterministic");
        require(!std::regex_search(test.source, std::regex("0x1[0-9a-f]{2}\\b")),
                "window address left unrelocated in case " + std::to_string(index));
        require(test.source.find("0xc0") != std::string::npos, "relocated case has no window access");
    }
    // A hand-encoded program that returns 42 anchors the checker, and
    // deliberate corruptions prove mismatches are reported.
    Case correct({1, 0});
    correct.result = 42;
    const std::vector<std::uint8_t> payload = {
        0x8F, 0xC0, 0x00, 0x8F, 0xFF, 0x01, 0x8F, 0xC0, 0x02, 0x8F, 0xFF, 0x03, // SSP/FP = $FFC0
        0xCD, 0xFF, 0xBD,                                                       // SP = $FF
        0xE8, 0x2A, 0x8D, 0x00, 0x5F, 0xC0, 0xFF};                              // YA = 42; return
    DiscoSPC700::Machine machine(payload, Origin, 0xEE);
    seedMachine(correct, machine);
    machine.start(Origin);
    machine.run(1000);
    check(correct, machine);
    unsigned mismatches = 0;
    for (unsigned kind = 0; kind < 5; ++kind) {
        auto wrong = correct;
        if (kind == 0) wrong.result = 41;
        else if (kind == 1) wrong.fault = 6;
        else if (kind == 2) wrong.expected[32] ^= 1;
        else if (kind == 3) wrong.reads[0] = 1;
        else wrong.writes[0] = 1;
        try { check(wrong, machine); } catch (const std::runtime_error&) { ++mismatches; }
    }
    require(mismatches == 5, "checker accepted deliberately corrupted expectations");
    std::cout << "SPC700 differential relocation, determinism and mismatch checks passed\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--self-test")) { selfTest(); return 0; }
        if (argc == 6 && std::string(argv[1]) == "--emit") {
            emit(spcCase(number(argv[4], std::numeric_limits<std::uint32_t>::max()), number(argv[5], CaseCount - 1)),
                 argv[2], argv[3]);
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--check") {
            const auto test = spcCase(number(argv[3], std::numeric_limits<std::uint32_t>::max()), number(argv[4], CaseCount - 1));
            DiscoSPC700::Machine machine(readPayload(argv[2]), Origin, 0xEE);
            seedMachine(test, machine);
            machine.start(Origin);
            machine.run(50000000);
            check(test, machine);
            std::cout << "seed=" << test.seed << " case=" << test.index
                      << " result/fault/RAM/volatile passed on SPC700\n";
            return 0;
        }
        throw std::runtime_error("usage: --self-test | --emit source.dc expected.txt seed case | --check payload.bin seed case");
    } catch (const std::exception& error) {
        std::cerr << "SPC700 differential failure: " << error.what() << '\n';
        return 1;
    }
}
