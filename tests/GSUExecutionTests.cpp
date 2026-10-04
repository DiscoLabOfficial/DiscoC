#include <array>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
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
    Machine(std::vector<std::uint8_t> code, std::uint32_t origin,
            std::uint16_t initial_sp = 0x2000, std::uint8_t initial_ram_bank = 0)
        : code_(std::move(code)), origin_(static_cast<std::uint16_t>(origin)),
          ram_(131072, 0), rom_(2097152, 0), ram_bank_(initial_ram_bank), program_bank_(origin >> 16),
          rom_bank_(program_bank_ <= 0x5f ? static_cast<std::uint8_t>(program_bank_) : 0) {
        if (code_.empty() || code_.size() > 65536u - origin_) {
            throw std::runtime_error("payload does not fit in the program bank");
        }
        registers_[9] = 0x4444;
        registers_[10] = initial_sp;
        registers_[11] = 0x5555;
        registers_[15] = origin_;
        if (program_bank_ == 0x70 || program_bank_ == 0x71) {
            const auto base = static_cast<std::size_t>(program_bank_ - 0x70) * 65536 + origin_;
            std::copy(code_.begin(), code_.end(), ram_.begin() + base);
        } else for (std::size_t index = 0; index < code_.size(); ++index)
            rom_.at(romIndex((program_bank_ << 16) | (origin_ + static_cast<std::uint32_t>(index)))) = code_[index];
    }

    std::uint16_t reg(std::size_t index) const { return registers_.at(index); }
    std::uint8_t ramBank() const { return ram_bank_; }
    std::uint8_t romBank() const { return rom_bank_; }
    unsigned accesses(std::uint32_t address, bool store) const {
        const auto& counts = store ? writes_ : reads_;
        const auto found = counts.find(address);
        return found == counts.end() ? 0 : found->second;
    }
    std::uint8_t byte(std::uint32_t address) const {
        if (address < 0x700000 || address > 0x71ffff) throw std::runtime_error("invalid RAM byte address");
        return ram_.at(address - 0x700000);
    }
    void seed(std::uint32_t address, std::uint8_t value, bool rom) {
        if (rom) rom_.at(romIndex(address)) = value;
        else {
            if (address < 0x700000 || address > 0x71ffff) throw std::runtime_error("invalid RAM seed address");
            ram_.at(address - 0x700000) = value;
        }
    }
    std::uint16_t word(std::uint32_t address) const {
        const auto bank = address > 65535 ? (address >> 16) - 0x70 : ram_bank_;
        if (bank > 1) throw std::runtime_error("invalid RAM bank in expectation");
        const auto index = static_cast<std::size_t>(bank) * 65536 + (address & 65535);
        return static_cast<std::uint16_t>(ram_.at(index) |
            (static_cast<std::uint16_t>(ram_.at(index ^ 1u)) << 8));
    }

    void run() {
        for (std::size_t step = 0; step < 10000000; ++step) {
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
    static std::size_t romIndex(std::uint32_t address) {
        const auto bank = address >> 16, offset = address & 65535;
        if (bank < 0x40 && offset >= 0x8000) return bank * 32768u + offset - 0x8000;
        if (bank >= 0x40 && bank <= 0x5f) return (bank - 0x40) * 65536u + offset;
        throw std::runtime_error("invalid GSU-visible ROM data address");
    }
    std::uint8_t fetch() const {
        const auto pc = registers_[15];
        if (pc < origin_ || static_cast<std::size_t>(pc - origin_) >= code_.size()) {
            throw std::runtime_error("GSU fetched outside the linked payload at PC=" + std::to_string(pc));
        }
        if (program_bank_ == 0x70 || program_bank_ == 0x71)
            return ram_.at(static_cast<std::size_t>(program_bank_ - 0x70) * 65536 + pc);
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
            const auto address = static_cast<std::size_t>(ram_bank_) * 65536 + registers_[index];
            ++writes_[static_cast<std::uint32_t>(0x700000 + address)];
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
            ++reads_[0x700000u + static_cast<std::uint32_t>(ram_bank_) * 65536u + address];
            write(destination_, alt1_ ? ram_.at(static_cast<std::size_t>(ram_bank_) * 65536 + address) : word(address));
            resetSelectors();
        } else if (opcode == 0x9f && alt1_) {
            const auto signedWord = [](std::uint16_t bits) -> std::int32_t {
                return bits < 32768 ? bits : static_cast<std::int32_t>(bits) - 65536;
            };
            const auto product = static_cast<std::uint32_t>(signedWord(registers_[source_]) * signedWord(registers_[6]));
            registers_[4] = static_cast<std::uint16_t>(product);
            write(destination_, static_cast<std::uint16_t>(product >> 16));
            setZeroSign(registers_[destination_]); resetSelectors();
        } else if (opcode == 0x9e) {
            const auto value = static_cast<std::uint16_t>(registers_[source_] & 255u);
            write(destination_, value); setZeroSign(value); resetSelectors();
        } else if (opcode == 0x95) {
            const auto low = static_cast<std::uint16_t>(registers_[source_] & 255u);
            const auto value = static_cast<std::uint16_t>(low < 128 ? low : low | 0xff00u);
            write(destination_, value);
            setZeroSign(value);
            resetSelectors();
        } else if (opcode == 0x4f) {
            const auto value = static_cast<std::uint16_t>(~registers_[source_]);
            write(destination_, value); setZeroSign(value); resetSelectors();
        } else if (opcode == 0x03 || opcode == 0x96) {
            const auto source = registers_[source_];
            const auto value = static_cast<std::uint16_t>((source >> 1) | (opcode == 0x96 ? source & 0x8000u : 0));
            carry_ = (source & 1u) != 0;
            write(destination_, value); setZeroSign(value); resetSelectors();
        } else if (opcode >= 0xc1 && opcode <= 0xcf) {
            const auto right = alt2_ ? static_cast<std::uint16_t>(index) : registers_[index];
            const auto value = static_cast<std::uint16_t>(alt1_ ? registers_[source_] ^ right : registers_[source_] | right);
            write(destination_, value); setZeroSign(value); resetSelectors();
        } else if (opcode >= 0x71 && opcode <= 0x7f && !alt1_) {
            const auto right = alt2_ ? static_cast<std::uint16_t>(index) : registers_[index];
            const auto value = static_cast<std::uint16_t>(registers_[source_] & right);
            write(destination_, value); setZeroSign(value); resetSelectors();
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
        } else if (opcode >= 0xa0 && opcode <= 0xaf && !alt1_ && !alt2_) {
            const auto byte = operand();
            write(index, static_cast<std::uint16_t>(byte < 128 ? byte : byte | 0xff00u));
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
        } else if (opcode == 0xdf && alt2_) {
            if (alt1_) rom_bank_ = static_cast<std::uint8_t>(registers_[source_] & 0x7fu);
            else ram_bank_ = static_cast<std::uint8_t>(registers_[source_] & 1u);
            resetSelectors();
        } else if (opcode == 0xef) {
            ++reads_[(static_cast<std::uint32_t>(rom_bank_) << 16) | registers_[14]];
            const auto fetched = rom_.at(romIndex((static_cast<std::uint32_t>(rom_bank_) << 16) | registers_[14]));
            std::uint16_t value = fetched;
            if (alt1_ && alt2_) value = static_cast<std::uint16_t>(fetched < 128 ? fetched : fetched | 0xff00);
            else if (alt1_) value = static_cast<std::uint16_t>((registers_[source_] & 255) | (static_cast<std::uint16_t>(fetched) << 8));
            else if (alt2_) value = static_cast<std::uint16_t>((registers_[source_] & 0xff00) | fetched);
            write(destination_, value); resetSelectors();
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
    std::vector<std::uint8_t> rom_;
    std::map<std::uint32_t, unsigned> reads_, writes_;
    std::uint8_t ram_bank_ = 0;
    std::uint32_t program_bank_ = 0;
    std::uint8_t rom_bank_ = 0;
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
    Machine bank({0xf0, 1, 0, 0x3e, 0xdf, 0xf0, 0, 1, 0xf1, 149, 0, 0x21, 0x30, 0, 1}, 0x706000);
    bank.run();
    require(bank.word(0x710100) == 149 && bank.word(0x700100) == 0,
            "RAMB did not select the independent RAM data bank");
    Machine bankMask({0xa0, 0x70, 0x3e, 0xdf, 0xf0, 0, 1, 0xf1, 149, 0, 0x21, 0x30, 0, 1},
                     0x716000, 0x7777, 1);
    bankMask.run();
    require(bankMask.word(0x700100) == 149 && bankMask.word(0x710100) == 0,
            "RAMB must use bit zero, not the program bank");
    Machine rom({0xf4, 2, 0, 0xb4, 0x3f, 0xdf, 0xfe, 0, 0x80, 0xef, 0, 1}, 0x008000);
    rom.seed(0x028000, 149, true);
    rom.run();
    require(rom.reg(0) == 149 && rom.romBank() == 2, "ROMB/GETB used the wrong bank");
    // The two ROM views are aliases of the same physical cartridge bytes.
    Machine alias({0xf4, 0x41, 0, 0xb4, 0x3f, 0xdf, 0xfe, 0, 0, 0xef, 0, 1}, 0x008000);
    alias.seed(0x028000, 149, true); alias.run();
    require(alias.reg(0) == 149, "Full-bank ROM view did not alias LoROM");
    bool self_modified = false;
    try {
        Machine modification({0xf0, 0x0f, 0x60, 0xf1, 0x9f, 0, 0x21, 0x3d, 0x30,
                              0xff, 0x0f, 0x60, 1, 1, 1, 0, 1}, 0x706000);
        modification.run();
    } catch (const std::runtime_error& error) {
        self_modified = std::string(error.what()).find("unsupported GSU opcode") != std::string::npos;
    }
    require(self_modified, "RAM data writes did not modify subsequent instruction fetches");
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
            if (argc < 6) {
                throw std::runtime_error("usage: runner payload origin [--initial-sp value] [--initial-ram-bank 0|1] [--word address expected | --register index expected]...");
            }
            struct Expectation { std::string option; unsigned address; unsigned expected; };
            std::vector<Expectation> expectations;
            std::vector<Expectation> seeds;
            std::uint16_t initial_sp = 0x2000;
            std::uint8_t initial_ram_bank = 0;
            for (int index = 3; index < argc;) {
                const std::string option(argv[index]);
                if (option == "--initial-sp" || option == "--initial-ram-bank") {
                    if (index + 1 >= argc) throw std::runtime_error("missing initial state value");
                    if (option == "--initial-sp") initial_sp = static_cast<std::uint16_t>(number(argv[index + 1], 65535));
                    else initial_ram_bank = static_cast<std::uint8_t>(number(argv[index + 1], 1));
                    index += 2;
                    continue;
                }
                if (index + 2 >= argc) throw std::runtime_error("missing expectation values");
                const auto address = number(argv[index + 1], option == "--register" ? 15 : 0x71ffff);
                const auto expected = number(argv[index + 2], 65535);
                if (option == "--rom-byte" || option == "--ram-byte") {
                    if (expected > 255) throw std::runtime_error("seed byte exceeds 8 bits");
                    seeds.push_back({option, address, expected});
                    index += 3; continue;
                }
                if (option != "--word" && option != "--register" && option != "--byte" &&
                    option != "--rambr" && option != "--rombr" && option != "--reads" && option != "--writes") throw std::runtime_error("unknown expectation");
                expectations.push_back({option, address, expected});
                index += 3;
            }
            if (expectations.empty()) throw std::runtime_error("at least one expectation is required");
            Machine machine(readPayload(argv[1]), number(argv[2], 0xffffff), initial_sp, initial_ram_bank);
            for (const auto& seed : seeds) machine.seed(seed.address, static_cast<std::uint8_t>(seed.expected), seed.option == "--rom-byte");
            machine.run();
            for (const auto& expectation : expectations) {
                const unsigned actual = expectation.option == "--word" ? machine.word(expectation.address) :
                    expectation.option == "--reads" ? machine.accesses(expectation.address, false) :
                    expectation.option == "--writes" ? machine.accesses(expectation.address, true) :
                    expectation.option == "--byte" ? machine.byte(expectation.address) :
                    expectation.option == "--rambr" ? machine.ramBank() :
                    expectation.option == "--rombr" ? machine.romBank() : machine.reg(expectation.address);
                if (actual != expectation.expected) {
                    throw std::runtime_error(expectation.option + " " + std::to_string(expectation.address) +
                        ": expected " + std::to_string(expectation.expected) + ", got " + std::to_string(actual));
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
