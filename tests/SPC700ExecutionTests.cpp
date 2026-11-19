#include "SPC700InstructionModel.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using DiscoSPC700::Machine;
using Bytes = std::vector<std::uint8_t>;
namespace Flag = DiscoSPC700::Flag;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string hex(unsigned value) {
    std::ostringstream out;
    out << '$' << std::hex << std::uppercase << value;
    return out.str();
}

// Hand-encoded payloads at $0200 that end with JMP !$FFC0 (return to IPL).
const Bytes ReturnToIpl = {0x5F, 0xC0, 0xFF};

Bytes program(Bytes body) {
    body.insert(body.end(), ReturnToIpl.begin(), ReturnToIpl.end());
    return body;
}

void runProgram(Machine& machine, std::uint16_t entry = 0x0200) {
    machine.start(entry);
    machine.run(100000);
    require(machine.returnedToIpl(), "program did not return to the IPL");
}

void expectFailure(const Bytes& code, const std::string& expected, std::uint64_t limit = 100000) {
    Machine machine(code, 0x0200);
    machine.start(0x0200);
    try {
        machine.run(limit);
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(expected) != std::string::npos,
            "unexpected model failure: " + std::string(error.what()) + " (expected '" + expected + "')");
        return;
    }
    throw std::runtime_error("model unexpectedly succeeded; expected '" + expected + "'");
}

void selfTest() {
    {   // ADC: signed overflow, half carry, no carry.
        Machine m(program({0x60, 0xE8, 0x7F, 0x88, 0x01}), 0x0200);
        runProgram(m);
        require(m.a() == 0x80 && m.psw() == (Flag::N | Flag::V | Flag::H), "ADC flags");
    }
    {   // SBC with borrow out.
        Machine m(program({0x80, 0xE8, 0x00, 0xA8, 0x01}), 0x0200);
        runProgram(m);
        require(m.a() == 0xFF && m.psw() == Flag::N, "SBC borrow flags");
    }
    {   // DIV without overflow: $0123 / $10.
        Machine m(program({0x8D, 0x01, 0xE8, 0x23, 0xCD, 0x10, 0x9E}), 0x0200);
        runProgram(m);
        require(m.a() == 0x12 && m.y() == 0x03 && m.psw() == Flag::H, "DIV quotient/remainder");
    }
    {   // DIV with an overflowing quotient: $4000 / $10.
        Machine m(program({0x8D, 0x40, 0xE8, 0x00, 0xCD, 0x10, 0x9E}), 0x0200);
        runProgram(m);
        require(m.a() == 0xDD && m.y() == 0x30 && m.psw() == (Flag::N | Flag::V | Flag::H), "DIV overflow result");
    }
    {   // DIV by zero follows the same hardware formula.
        Machine m(program({0x8D, 0x12, 0xE8, 0x34, 0xCD, 0x00, 0x9E}), 0x0200);
        runProgram(m);
        require(m.a() == 0xED && m.y() == 0x34, "DIV by zero result");
    }
    {   // BCD: 15 + 27 = 42 after DAA.
        Machine m(program({0x60, 0xE8, 0x15, 0x88, 0x27, 0xDF}), 0x0200);
        runProgram(m);
        require(m.a() == 0x42 && !(m.psw() & Flag::C), "DAA adjustment");
    }
    {   // BCD: 42 - 15 = 27 after DAS.
        Machine m(program({0x80, 0xE8, 0x42, 0xA8, 0x15, 0xBE}), 0x0200);
        runProgram(m);
        require(m.a() == 0x27 && (m.psw() & Flag::C), "DAS adjustment");
    }
    {   // MUL sets N/Z from the high byte.
        Machine m(program({0x8D, 0x12, 0xE8, 0x34, 0xCF}), 0x0200);
        runProgram(m);
        require(m.a() == 0xA8 && m.y() == 0x03 && !(m.psw() & (Flag::N | Flag::Z)), "MUL product");
    }
    {   // [dp]+Y wraps the 16-bit address.
        Machine m(program({0x8F, 0xFF, 0x20, 0x8F, 0xFF, 0x21, 0x8D, 0x02, 0xE8, 0x5C, 0xD7, 0x20}), 0x0200);
        runProgram(m);
        require(m.byte(0x0001) == 0x5C, "[dp]+Y address wrap");
    }
    {   // SETP moves direct page to $0100.
        Machine m(program({0x40, 0x8F, 0x77, 0x10, 0x20}), 0x0200);
        runProgram(m);
        require(m.byte(0x0110) == 0x77 && m.byte(0x0010) == 0x00, "direct page 1");
    }
    {   // dp+X wraps inside the direct page.
        Machine m(program({0xCD, 0x20, 0xE8, 0x99, 0xD4, 0xF0}), 0x0200);
        runProgram(m);
        require(m.byte(0x0010) == 0x99, "dp+X wrap");
    }
    {   // DBNZ Y loop.
        Machine m(program({0x8D, 0x05, 0xE8, 0x00, 0xBC, 0xFE, 0xFD}), 0x0200);
        runProgram(m);
        require(m.a() == 5 && m.y() == 0, "DBNZ Y loop");
    }
    {   // CALL/RET restore the hardware stack.
        Machine m({0x3F, 0x06, 0x02, 0x5F, 0xC0, 0xFF, 0xE8, 0x42, 0x6F}, 0x0200);
        runProgram(m);
        require(m.a() == 0x42 && m.sp() == 0xEF && m.metrics().calls == 1, "CALL/RET");
    }
    {   // TSET1 sets bits and flags from A - memory.
        Machine m(program({0xE8, 0xF0, 0xC5, 0x00, 0x03, 0xE8, 0x0F, 0x0E, 0x00, 0x03}), 0x0200);
        runProgram(m);
        require(m.byte(0x0300) == 0xFF && !(m.psw() & (Flag::N | Flag::Z)), "TSET1");
    }
    {   // Memory bit operations.
        Machine m(program({0xE8, 0x01, 0xC5, 0x00, 0x03, 0xAA, 0x00, 0x03, 0xEA, 0x00, 0x03, 0xCA, 0x01, 0xE3}), 0x0200);
        runProgram(m);
        require(m.byte(0x0300) == 0x00 && m.byte(0x0301) == 0x80 && (m.psw() & Flag::C), "MOV1/NOT1");
    }
    {   // ADDW: signed overflow into bit 15.
        Machine m(program({0x8F, 0x01, 0x20, 0x8F, 0x00, 0x21, 0xE8, 0xFF, 0x8D, 0x7F, 0x7A, 0x20}), 0x0200);
        runProgram(m);
        require(m.a() == 0x00 && m.y() == 0x80 && m.psw() == (Flag::N | Flag::V | Flag::H), "ADDW flags");
    }
    {   // SUBW and CMPW.
        Machine m(program({0x8F, 0x01, 0x20, 0x8F, 0x00, 0x21, 0xE8, 0x00, 0x8D, 0x00, 0x9A, 0x20, 0x5A, 0x20}), 0x0200);
        runProgram(m);
        // SUBW: 0 - 1 = $FFFF; CMPW $FFFF - 1 sets C and N.
        require(m.a() == 0xFF && m.y() == 0xFF && (m.psw() & Flag::C) && (m.psw() & Flag::N), "SUBW/CMPW");
    }
    {   // Ports: outputs are latched, inputs come from the host side.
        Machine m(program({0x8F, 0x12, 0xF6, 0xE4, 0xF4}), 0x0200);
        m.setInputPort(0, 0x77);
        runProgram(m);
        require(m.outputPort(2) == 0x12 && m.a() == 0x77 && m.outputPort(0) == 0xAA && m.outputPort(1) == 0xBB,
            "port latches");
    }
    {   // The IPL hand-off clears $0001-$00EF on every start.
        Machine m(program({0x8F, 0x55, 0x30}), 0x0200, 0xEE);
        m.start(0x0200);
        require(m.byte(0x0030) == 0x00 && m.byte(0x00F8) == 0xEE && m.word(0x0000) == 0x0200, "IPL hand-off state");
        m.run(100);
        require(m.byte(0x0030) == 0x55, "first run");
        m.start(0x0200);
        require(m.byte(0x0030) == 0x00, "second start clears page 0");
    }
    {   // CBNE and BBS branch on memory.
        Machine m(program({0x8F, 0x04, 0x20, 0xE8, 0x04, 0x2E, 0x20, 0x02, 0xE8, 0x11, 0x43, 0x20, 0x02, 0xE8, 0x22}), 0x0200);
        runProgram(m);
        require(m.a() == 0x11, "CBNE falls through, BBS bit 2 taken");
    }
    {   // MOV (X)+ post-increments X.
        Machine m(program({0xCD, 0x30, 0xE8, 0x66, 0xAF, 0xAF}), 0x0200);
        runProgram(m);
        require(m.byte(0x0030) == 0x66 && m.byte(0x0031) == 0x66 && m.x() == 0x32, "MOV (X)+");
    }

    expectFailure({0x2F, 0x10}, "outside the loaded payload");
    expectFailure(program({0xE5, 0xD0, 0xFF}), "while the IPL ROM is mapped");
    expectFailure(program({0x8F, 0x00, 0xF0}), "unsupported register $00F0");
    expectFailure(program({0xE4, 0xF1}), "write-only or unsupported register $00F1");
    expectFailure({0xEF}, "sleep halts the processor");
    expectFailure({0x2F, 0xFE}, "exceeded the instruction limit", 100);
    {   // Unmapping the IPL turns $FFC0 into ordinary RAM: jumping there no
        // longer returns, and fetching outside the payload fails closed.
        expectFailure({0x8F, 0x00, 0xF1, 0x5F, 0xC0, 0xFF}, "outside the loaded payload");
    }
    bool rejected = false;
    try {
        Machine m({0x00}, 0xFFFF);
        Machine too_big(Bytes(2, 0), 0xFFFF);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "payload past $FFFF must be rejected");
}

// ---- Deterministic instruction torture program ---------------------------
//
// Each case seeds scratch memory and registers, executes one instruction and
// records A, X, Y, PSW, the eight direct-page scratch bytes and five absolute
// scratch bytes into a table at TortureTable. The same payload runs in this
// model and on an independent emulator; the tables must match byte for byte.
// The program loads at TortureOrigin, above the absolute scratch bytes at
// $0400 (mem.bit operands can only address $0000-$1FFF) and below the table.

constexpr unsigned TortureOrigin = 0x2000;
constexpr unsigned TortureTable = 0xC000;
constexpr unsigned TortureRecord = 17;

struct Random {
    std::uint32_t state;
    std::uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    unsigned below(unsigned limit) { return next() % limit; }
};

bool tortureOpcode(unsigned opcode) {
    if ((opcode & 0x0F) == 0x01) return false; // TCALL reads IPL vectors.
    switch (opcode) {
        case 0x0F: case 0x1F: case 0x3F: case 0x4F: case 0x5F: case 0x6F: case 0x7F: // flow
        case 0xEF: case 0xFF: case 0xBD:                                           // halt, SP
        case 0x0D: case 0x2D: case 0x4D: case 0x6D: case 0x8E: case 0xAE: case 0xCE: case 0xEE: // stack
            return false;
        default: return true;
    }
}

std::string byteHex(unsigned value) {
    static const char* digits = "0123456789ABCDEF";
    return std::string("$") + digits[(value >> 4) & 15] + digits[value & 15];
}

std::string wordHex(unsigned value) {
    return "$" + byteHex(value >> 8).substr(1) + byteHex(value & 0xFF).substr(1);
}

// Cases [first, first + count) of the sequence for `seed`. The sequence walks
// every allowed opcode once, then chooses at random; one chunk must stay below
// TortureMaxCases so the program and its table fit the host's upload bank.
constexpr unsigned TortureMaxCases = 128;

std::string emitTorture(std::uint32_t seed, unsigned first, unsigned count) {
    if (count == 0 || count > TortureMaxCases) throw std::runtime_error("torture chunk must hold 1..128 cases");
    std::ostringstream out;
    out << "; Generated SPC700 torture program, seed " << seed << ", cases " << first << ".." << (first + count - 1) << ".\n"
        << ".setcpu \"SPC700\"\n.define __DISCO_CODE_START_ADDRESS " << wordHex(TortureOrigin) << "\n.segment \"CODE\"\n";
    std::vector<unsigned> opcodes;
    for (unsigned opcode = 0; opcode < 256; ++opcode)
        if (tortureOpcode(opcode)) opcodes.push_back(opcode);
    // Each case draws from its own generator so chunks are independent.
    for (unsigned case_index = first; case_index < first + count; ++case_index) {
        Random random{(seed ? seed : 1u) ^ (0x9E3779B9u * (case_index + 1))};
        for (int warm = 0; warm < 4; ++warm) random.next();
        const auto index = case_index - first;
        const auto opcode = case_index < opcodes.size() ? opcodes[case_index] :
            opcodes[random.below(static_cast<unsigned>(opcodes.size()))];
        const auto& info = SPC700Isa::instruction(static_cast<std::uint8_t>(opcode));
        const bool page1 = random.below(4) == 0;
        const unsigned page = page1 ? 0x100u : 0u;
        unsigned a = random.below(256), x = random.below(256), y = random.below(256);
        unsigned psw = random.below(256) & ~0x20u;
        if (page1) psw |= 0x20u;

        std::string operands;
        const auto count = info.operands[0] == SPC700Isa::OperandPattern::None ? 0u :
            info.operands[1] == SPC700Isa::OperandPattern::None ? 1u : 2u;
        const std::string label = "skip" + std::to_string(case_index);
        for (unsigned slot = 0; slot < count; ++slot) {
            using P = SPC700Isa::OperandPattern;
            std::string text;
            switch (info.operands[slot]) {
                case P::A: text = "a"; break;
                case P::X: text = "x"; break;
                case P::Y: text = "y"; break;
                case P::YA: text = "ya"; break;
                case P::SP: text = "sp"; break;
                case P::PSW: text = "psw"; break;
                case P::C: text = "c"; break;
                case P::Imm: text = "#" + byteHex(random.below(256)); break;
                case P::Dp: text = byteHex(0x20 + random.below(8)); break;
                case P::DpX: text = "$20+x"; x = random.below(4); break;
                case P::DpY: text = "$20+y"; y = random.below(4); break;
                case P::Abs: text = "!" + wordHex(0x0400 + random.below(4)); break;
                case P::AbsX: text = "!$0400+x"; x = random.below(4); break;
                case P::AbsY: text = "!$0400+y"; y = random.below(4); break;
                case P::IndX: text = "(x)"; x = 0x20 + random.below(8); break;
                case P::IndXInc: text = "(x)+"; x = 0x20 + random.below(7); break;
                case P::IndY: text = "(y)"; y = 0x20 + random.below(8); break;
                case P::DpXInd: text = "[$28+x]"; x = 2 * random.below(2); break;
                case P::DpIndY: text = "[$28]+y"; y = random.below(4); break;
                case P::DpBit: text = byteHex(0x20 + random.below(8)) + "." + std::to_string(info.fixed); break;
                case P::MemBit: text = wordHex(0x0400 + random.below(4)) + "." + std::to_string(random.below(8)); break;
                case P::NotMemBit: text = "/" + wordHex(0x0400 + random.below(4)) + "." + std::to_string(random.below(8)); break;
                case P::Rel: text = label; break;
                default: throw std::runtime_error("torture: unexpected operand pattern");
            }
            operands += (slot ? "," : " ") + text;
        }

        out << "; case " << case_index << ": " << info.mnemonic << operands << '\n';
        const auto seedByte = [&](unsigned address, unsigned value) {
            out << "    mov a,#" << byteHex(value) << "\n    mov !" << wordHex(address) << ",a\n";
        };
        for (unsigned offset = 0; offset < 8; ++offset) seedByte(page + 0x20 + offset, random.below(256));
        seedByte(page + 0x28, 0x00); seedByte(page + 0x29, 0x04); // [$28] -> $0400
        seedByte(page + 0x2A, 0x02); seedByte(page + 0x2B, 0x04); // [$2A] -> $0402
        for (unsigned offset = 0; offset < 4; ++offset) seedByte(0x0400 + offset, random.below(256));
        seedByte(0x0404, 0x00);
        out << "    mov x,#" << byteHex(x) << "\n    mov y,#" << byteHex(y) << '\n'
            << "    mov a,#" << byteHex(psw) << "\n    push a\n    mov a,#" << byteHex(a) << "\n    pop psw\n"
            << "    " << info.mnemonic << operands << '\n';
        if (count && info.operands[count - 1] == SPC700Isa::OperandPattern::Rel)
            out << "    inc !$0404 ; counts not-taken branches\n" << label << ":\n";
        const auto record = TortureTable + index * TortureRecord;
        out << "    mov !" << wordHex(record) << ",a\n    mov !" << wordHex(record + 1) << ",x\n"
            << "    mov !" << wordHex(record + 2) << ",y\n    push psw\n    pop a\n    mov !" << wordHex(record + 3) << ",a\n";
        for (unsigned offset = 0; offset < 8; ++offset)
            out << "    mov a,!" << wordHex(page + 0x20 + offset) << "\n    mov !" << wordHex(record + 4 + offset) << ",a\n";
        for (unsigned offset = 0; offset < 5; ++offset)
            out << "    mov a,!" << wordHex(0x0400 + offset) << "\n    mov !" << wordHex(record + 12 + offset) << ",a\n";
    }
    out << "    clrp\n    mov $F1,#$80 ; keep the IPL ROM mapped\n    jmp !$FFC0\n";
    return out.str();
}

// ---- Generic runner -------------------------------------------------------

std::uint32_t parseNumber(const std::string& text, std::uint32_t maximum) {
    std::size_t consumed = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(text, &consumed, 0);
    } catch (const std::exception&) {
        throw std::runtime_error("invalid number: " + text);
    }
    if (consumed != text.size() || value > maximum) throw std::runtime_error("number out of range: " + text);
    return static_cast<std::uint32_t>(value);
}

Bytes readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + path);
    return Bytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

int runGeneric(const std::vector<std::string>& args) {
    if (args.size() < 2) throw std::runtime_error("expected <payload> <origin>");
    const auto payload = readFile(args[0]);
    const auto origin = static_cast<std::uint16_t>(parseNumber(args[1], 0xFFFF));
    std::uint16_t entry = origin;
    std::uint8_t fill = 0;
    std::uint64_t limit = 10000000;
    unsigned runs = 1;
    std::string dump;
    std::vector<std::pair<std::uint16_t, std::uint8_t>> seeds;
    std::vector<std::pair<unsigned, std::uint8_t>> inputs;
    std::vector<std::function<void(const Machine&)>> checks;
    const auto need = [&](std::size_t& index, std::size_t count) {
        if (index + count >= args.size()) throw std::runtime_error(args[index] + " needs " + std::to_string(count) + " value(s)");
    };
    for (std::size_t index = 2; index < args.size(); ++index) {
        const auto& option = args[index];
        if (option == "--entry") { need(index, 1); entry = static_cast<std::uint16_t>(parseNumber(args[++index], 0xFFFF)); }
        else if (option == "--fill") { need(index, 1); fill = static_cast<std::uint8_t>(parseNumber(args[++index], 0xFF)); }
        else if (option == "--limit") { need(index, 1); limit = parseNumber(args[++index], 100000000); }
        else if (option == "--runs") { need(index, 1); runs = parseNumber(args[++index], 16); if (!runs) throw std::runtime_error("--runs must be positive"); }
        else if (option == "--dump-aram") { need(index, 1); dump = args[++index]; }
        else if (option == "--ram") {
            need(index, 2);
            const auto address = static_cast<std::uint16_t>(parseNumber(args[index + 1], 0xFFFF));
            seeds.emplace_back(address, static_cast<std::uint8_t>(parseNumber(args[index + 2], 0xFF)));
            index += 2;
        } else if (option == "--input-port") {
            need(index, 2);
            inputs.emplace_back(parseNumber(args[index + 1], 3), static_cast<std::uint8_t>(parseNumber(args[index + 2], 0xFF)));
            index += 2;
        } else if (option == "--byte" || option == "--word") {
            need(index, 2);
            const auto address = static_cast<std::uint16_t>(parseNumber(args[index + 1], 0xFFFF));
            const bool word = option == "--word";
            const auto expected = parseNumber(args[index + 2], word ? 0xFFFF : 0xFF);
            checks.push_back([=](const Machine& m) {
                const unsigned actual = word ? m.word(address) : m.byte(address);
                require(actual == expected, option + " " + hex(address) + ": expected " + hex(expected) + ", got " + hex(actual));
            });
            index += 2;
        } else if (option == "--register") {
            need(index, 2);
            const auto name = args[index + 1];
            const auto expected = parseNumber(args[index + 2], name == "ya" ? 0xFFFF : 0xFF);
            checks.push_back([=](const Machine& m) {
                unsigned actual = 0;
                if (name == "a") actual = m.a();
                else if (name == "x") actual = m.x();
                else if (name == "y") actual = m.y();
                else if (name == "sp") actual = m.sp();
                else if (name == "psw") actual = m.psw();
                else if (name == "ya") actual = static_cast<unsigned>(m.a() | (m.y() << 8));
                else throw std::runtime_error("unknown register " + name);
                require(actual == expected, "register " + name + ": expected " + hex(expected) + ", got " + hex(actual));
            });
            index += 2;
        } else if (option == "--port") {
            need(index, 2);
            const auto port = parseNumber(args[index + 1], 3);
            const auto expected = parseNumber(args[index + 2], 0xFF);
            checks.push_back([=](const Machine& m) {
                require(m.outputPort(port) == expected, "port " + std::to_string(port) + ": expected " + hex(expected) +
                    ", got " + hex(m.outputPort(port)));
            });
            index += 2;
        } else {
            throw std::runtime_error("unknown option " + option);
        }
    }
    if (checks.empty() && dump.empty()) throw std::runtime_error("at least one expectation or --dump-aram is required");
    Machine machine(payload, origin, fill);
    for (const auto& seed : seeds) machine.seed(seed.first, seed.second);
    // Every run restarts from the IPL hand-off; RAM outside page 0 persists,
    // so later runs observe what earlier runs left behind.
    for (unsigned run = 0; run < runs; ++run) {
        machine.start(entry);
        for (const auto& input : inputs) machine.setInputPort(input.first, input.second);
        machine.run(limit);
        for (const auto& check : checks) check(machine);
    }
    if (!dump.empty()) {
        std::ofstream output(dump, std::ios::binary);
        output.write(reinterpret_cast<const char*>(machine.memory().data()),
                     static_cast<std::streamsize>(machine.memory().size()));
        if (!output) throw std::runtime_error("cannot write " + dump);
    }
    std::cout << "SPC700 execution checks passed\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const std::vector<std::string> args(argv + 1, argv + argc);
        if (args.size() == 1 && args[0] == "--self-test") {
            selfTest();
            std::cout << "SPC700 model self-test passed\n";
            return 0;
        }
        if (args.size() == 4 && args[0] == "--emit-torture") {
            std::cout << emitTorture(parseNumber(args[1], 0xFFFFFFFFu), parseNumber(args[2], 100000),
                                     parseNumber(args[3], TortureMaxCases));
            return 0;
        }
        if (args.empty()) throw std::runtime_error(
            "usage: disco_spc700_execution_tests --self-test | --emit-torture <seed> <first> <count> | "
            "<payload> <origin> [options] <expectations>");
        return runGeneric(args);
    } catch (const std::exception& error) {
        std::cerr << "SPC700 execution test failed: " << error.what() << '\n';
        return 1;
    }
}
