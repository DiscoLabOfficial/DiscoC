#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// Instruction-level regression model, not a SNES emulator. Models the GSU
// prefetch pipeline and register selectors independently of compiler helpers.
// Unsupported instructions and reads outside the payload fail closed.
// Reference: SourMesen/Mesen2, Core/SNES/Coprocessors/GSU/Gsu.cpp and
// Gsu.Instructions.cpp (ReadOpCode, ReadOperand, ResetFlags, TO, WITH, LINK).
class Machine {
public:
    Machine(std::vector<std::uint8_t> code, std::uint16_t origin)
        : code_(std::move(code)), origin_(origin), ram_(65536, 0) {
        if (code_.empty() || code_.size() > 65536u - origin_) {
            throw std::runtime_error("payload does not fit in the program bank");
        }
        registers_[9] = 0x4444;
        registers_[10] = 0x2000;
        registers_[11] = 0x5555;
        registers_[15] = origin_;
    }

    std::uint16_t reg(std::size_t index) const { return registers_.at(index); }
    std::uint16_t word(std::uint16_t address) const {
        return static_cast<std::uint16_t>(ram_.at(address) |
            (static_cast<std::uint16_t>(ram_.at(address ^ 1u)) << 8));
    }

    void run() {
        for (std::size_t step = 0; step < 100000; ++step) {
            const auto opcode = pipeline_;
            pipeline_ = fetch();
            pc_written_ = false;
            if (opcode == 0) return; // STOP
            execute(opcode);
            if (!pc_written_) ++registers_[15];
        }
        throw std::runtime_error("GSU execution exceeded the instruction limit");
    }

private:
    std::uint8_t fetch() const {
        const auto pc = registers_[15];
        if (pc < origin_ || static_cast<std::size_t>(pc - origin_) >= code_.size()) {
            throw std::runtime_error("GSU fetched outside the linked payload at PC=" + std::to_string(pc));
        }
        return code_.at(pc - origin_);
    }
    std::uint8_t operand() {
        const auto value = pipeline_;
        ++registers_[15];
        pipeline_ = fetch();
        return value;
    }
    void write(std::size_t index, std::uint16_t value) {
        registers_.at(index) = value;
        if (index == 15) pc_written_ = true;
    }
    void resetSelectors() {
        source_ = destination_ = 0;
        prefix_ = alt1_ = alt2_ = false;
    }
    void setZeroSign(std::uint16_t value) {
        zero_ = value == 0;
        sign_ = (value & 0x8000u) != 0;
    }
    void execute(std::uint8_t opcode) {
        const auto index = static_cast<std::size_t>(opcode & 15u);
        if (opcode == 1) {
            resetSelectors();
        } else if (opcode >= 5 && opcode <= 15) {
            const auto raw_offset = operand();
            const int offset = raw_offset < 128 ? raw_offset : static_cast<int>(raw_offset) - 256;
            bool taken = false;
            switch (opcode) {
                case 5: taken = true; break;
                case 6: taken = sign_ == overflow_; break;
                case 7: taken = sign_ != overflow_; break;
                case 8: taken = !zero_; break;
                case 9: taken = zero_; break;
                case 10: taken = !sign_; break;
                case 11: taken = sign_; break;
                case 12: taken = !carry_; break;
                case 13: taken = carry_; break;
                case 14: taken = !overflow_; break;
                case 15: taken = overflow_; break;
            }
            if (taken) write(15, static_cast<std::uint16_t>(registers_[15] + offset));
            // Branches preserve selectors, including the ALT and B flags.
        } else if (opcode >= 0x10 && opcode <= 0x1f) {
            if (prefix_) {
                write(index, registers_[source_]);
                resetSelectors();
            } else {
                destination_ = index;
            }
        } else if (opcode >= 0x20 && opcode <= 0x2f) {
            source_ = destination_ = index;
            prefix_ = true;
        } else if (opcode >= 0x30 && opcode <= 0x3b) {
            const auto address = registers_[index];
            const auto value = registers_[source_];
            ram_.at(address) = static_cast<std::uint8_t>(value);
            if (!alt1_) ram_.at(address ^ 1u) = static_cast<std::uint8_t>(value >> 8);
            resetSelectors();
        } else if (opcode >= 0x3d && opcode <= 0x3f) {
            if (opcode != 0x3e) alt1_ = true;
            if (opcode != 0x3d) alt2_ = true;
            prefix_ = false;
        } else if (opcode >= 0x40 && opcode <= 0x4b) {
            const auto address = registers_[index];
            write(destination_, alt1_ ? ram_.at(address) : word(address));
            resetSelectors();
        } else if (opcode == 0x95) {
            const auto low = static_cast<std::uint16_t>(registers_[source_] & 255u);
            const auto value = static_cast<std::uint16_t>(low < 128 ? low : low | 0xff00u);
            write(destination_, value);
            setZeroSign(value);
            resetSelectors();
        } else if (opcode >= 0x50 && opcode <= 0x6f) {
            const auto left = registers_[source_];
            const bool immediate = alt2_ && (opcode < 0x60 || !alt1_);
            const auto right = immediate ? static_cast<std::uint16_t>(index) : registers_[index];
            std::uint16_t value;
            if (opcode < 0x60) {
                const std::uint32_t sum = static_cast<std::uint32_t>(left) + right +
                    (alt1_ && carry_ ? 1u : 0u);
                value = static_cast<std::uint16_t>(sum);
                carry_ = sum > 65535;
                overflow_ = ((~(left ^ right) & (left ^ value)) & 0x8000u) != 0;
            } else {
                const int difference = static_cast<int>(left) - right -
                    (alt1_ && !alt2_ && !carry_ ? 1 : 0);
                value = static_cast<std::uint16_t>(difference);
                carry_ = difference >= 0;
                overflow_ = (((left ^ right) & (left ^ value)) & 0x8000u) != 0;
            }
            setZeroSign(value);
            if (opcode < 0x60 || !(alt1_ && alt2_)) write(destination_, value);
            resetSelectors();
        } else if (opcode >= 0x91 && opcode <= 0x94) {
            registers_[11] = static_cast<std::uint16_t>(registers_[15] + opcode - 0x90);
            resetSelectors();
        } else if (opcode >= 0x98 && opcode <= 0x9d && !alt1_) {
            write(15, registers_[index]);
            resetSelectors();
        } else if (opcode >= 0xb0 && opcode <= 0xbf) {
            if (prefix_) {
                const auto value = registers_[index];
                write(destination_, value);
                setZeroSign(value);
                overflow_ = (value & 0x80u) != 0;
                resetSelectors();
            } else {
                source_ = index;
            }
        } else if ((opcode >= 0xd0 && opcode <= 0xde) ||
                   (opcode >= 0xe0 && opcode <= 0xee)) {
            const int adjustment = opcode < 0xe0 ? 1 : -1;
            write(index, static_cast<std::uint16_t>(registers_[index] + adjustment));
            setZeroSign(registers_[index]);
            resetSelectors();
        } else if (opcode >= 0xf0 && !alt1_ && !alt2_) {
            const auto low = operand();
            const auto high = operand();
            write(index, static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high) << 8)));
            resetSelectors();
        } else {
            throw std::runtime_error("unsupported GSU opcode in execution regression: " +
                                     std::to_string(opcode));
        }
    }

    std::vector<std::uint8_t> code_;
    std::uint16_t origin_;
    std::vector<std::uint8_t> ram_;
    std::array<std::uint16_t, 16> registers_{};
    std::uint8_t pipeline_ = 1;
    std::size_t source_ = 0, destination_ = 0;
    bool pc_written_ = false, prefix_ = false, alt1_ = false, alt2_ = false;
    bool zero_ = false, sign_ = false, carry_ = false, overflow_ = false;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void selfTest() {
    Machine copy({0xf0, 149, 0, 0x20, 0x11, 0, 1}, 0x8000);
    copy.run();
    require(copy.reg(1) == 149, "WITH R0 / TO R1 did not copy R0");
    Machine selector({0xf0, 149, 0, 0x11, 0, 1}, 0x8000);
    selector.run();
    require(selector.reg(1) == 0, "TO alone incorrectly copied R0");
    Machine call({0x94, 0xff, 8, 0x80, 1, 0, 1, 1, 0xf0, 149, 0, 0x9b, 1}, 0x8000);
    call.run();
    require(call.reg(0) == 149 && call.reg(11) == 0x8005,
            "LINK / IWT / return pipeline is incorrect");
    Machine load({0xf0, 0, 1, 0xf1, 149, 0, 0x21, 0x30, 0x12, 0x40, 0, 1}, 0x8000);
    load.run();
    require(load.reg(2) == 149 && load.word(0x100) == 149, "load/store selectors are incorrect");
    Machine delay({0xf0, 0, 0, 0x05, 3, 0xd0, 0xd0, 0xd0, 0, 1}, 0x8000);
    delay.run();
    require(delay.reg(0) == 1, "taken branch did not execute exactly one delay slot");
    try {
        Machine unsupported({0x9f, 0, 1}, 0x8000);
        unsupported.run();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("unsupported instruction was silently accepted");
}

unsigned number(const char* text, unsigned maximum) {
    const std::string value(text);
    std::size_t consumed = 0;
    const auto result = std::stoul(value, &consumed, 0);
    if (consumed != value.size() || result > maximum) throw std::runtime_error("invalid test argument");
    return static_cast<unsigned>(result);
}

std::vector<std::uint8_t> readPayload(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open execution payload");
    const auto length = input.tellg();
    if (length <= 0 || length > 65536) throw std::runtime_error("invalid execution payload size");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("truncated execution payload");
    }
    return bytes;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") {
            selfTest();
        } else {
            if (argc < 6 || (argc - 3) % 3 != 0) {
                throw std::runtime_error("usage: runner payload origin [--word address expected | --register index expected]...");
            }
            Machine machine(readPayload(argv[1]), static_cast<std::uint16_t>(number(argv[2], 65535)));
            machine.run();
            for (int index = 3; index < argc; index += 3) {
                const std::string option(argv[index]);
                const auto address = number(argv[index + 1], option == "--register" ? 15 : 65535);
                const auto expected = number(argv[index + 2], 65535);
                if (option != "--word" && option != "--register") throw std::runtime_error("unknown expectation");
                const auto actual = option == "--word"
                    ? machine.word(static_cast<std::uint16_t>(address)) : machine.reg(address);
                if (actual != expected) {
                    throw std::runtime_error(option + " " + std::to_string(address) +
                        ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
                }
            }
        }
        std::cout << "GSU execution checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
