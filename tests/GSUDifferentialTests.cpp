#include "differential/DifferentialCases.hpp"
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
using namespace DifferentialCases;
constexpr unsigned InitialStack = 0x3000;

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
