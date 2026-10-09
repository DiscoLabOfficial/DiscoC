#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "GSUGraphicsModel.hpp"

namespace DiscoGSU {

struct ExecutionMetrics {
    std::uint64_t instructions = 0;
    std::uint64_t ram_loads = 0, rom_reads = 0, stores = 0;
    std::uint64_t stack_loads = 0, stack_stores = 0;
    std::uint64_t branches = 0, taken_branches = 0, jumps = 0, calls = 0;
    std::uint64_t store_backs = 0;
};

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
          ram_(131072, 0), rom_(2097152, 0), graphics_(ram_), ram_bank_(initial_ram_bank), program_bank_(origin >> 16),
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

    // The graphics model borrows ram_: copying/moving Machine would leave
    // that reference pointing at the old owner.
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;
    Machine(Machine&&) = delete;
    Machine& operator=(Machine&&) = delete;

    const ExecutionMetrics& metrics() const { return metrics_; }
    void configureStackWindow(std::uint32_t begin, std::uint32_t end) {
        if (begin < 0x700000 || end > 0x720000 || begin >= end)
            throw std::runtime_error("invalid benchmark stack window");
        stack_begin_ = begin;
        stack_end_ = end;
    }
    std::uint16_t reg(std::size_t index) const { return registers_.at(index); }
    std::uint8_t ramBank() const { return ram_bank_; }
    std::uint8_t romBank() const { return rom_bank_; }
    std::uint16_t cacheBase() const { return cache_base_; }
    unsigned cacheRequests() const { return cache_requests_; }
    void configureScreen(std::uint8_t mode, std::uint8_t base) { graphics_.configure(mode, base); }
    unsigned graphicsState(const std::string& option) const { return graphics_.state(option); }
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
            // The initial pipeline NOP is synthetic. Real prefixes, delay
            // slots and STOP count as executed opcodes; operand bytes do not.
            if (step != 0) ++metrics_.instructions;
            if (opcode == 0) return; // STOP
            execute(opcode);
            if (pc_written_ && !(opcode >= 5 && opcode <= 15)) ++metrics_.jumps;
            if (!pc_written_) ++registers_[15];
        }
        throw std::runtime_error("GSU execution exceeded the instruction limit");
    }

private:
    void memoryAccess(std::uint32_t address, bool store, bool byte_access) {
        if (store) ++metrics_.stores;
        else ++metrics_.ram_loads;
        const auto in_stack = [&](std::uint32_t location) {
            return location >= stack_begin_ && location < stack_end_;
        };
        // Count a word operation once, including its XOR-1 hardware byte.
        // Classification is address-based, so R9/R0 frame accesses count too.
        if (in_stack(address) || (!byte_access && in_stack(address ^ 1u))) {
            if (store) ++metrics_.stack_stores;
            else ++metrics_.stack_loads;
        }
    }
    static std::size_t romIndex(std::uint32_t address) {
        const auto bank = address >> 16, offset = address & 65535;
        if (bank < 0x40 && offset >= 0x8000) return bank * 32768u + offset - 0x8000;
        if (bank >= 0x40 && bank <= 0x5f) return (bank - 0x40) * 65536u + offset;
        throw std::runtime_error("invalid GSU-visible ROM data address");
    }
    std::uint8_t programByte(std::uint16_t address) const {
        if (program_bank_ == 0x70 || program_bank_ == 0x71)
            return ram_.at(static_cast<std::size_t>(program_bank_ - 0x70) * 65536 + address);
        return rom_.at(romIndex((program_bank_ << 16) | address));
    }
    std::uint8_t fetch() {
        const auto pc = registers_[15];
        if (pc < origin_ || static_cast<std::size_t>(pc - origin_) >= code_.size()) {
            throw std::runtime_error("GSU fetched outside the linked payload at PC=" + std::to_string(pc));
        }
        const auto cache_offset = static_cast<std::uint16_t>(pc - cache_base_);
        if (cache_offset < instruction_cache_.size()) {
            const auto line = static_cast<std::size_t>(cache_offset / 16);
            if (!cache_valid_.at(line)) {
                // Cache fills can include padding outside CODE, but actual
                // instruction/operand fetches still obey the payload check.
                const auto first_address = static_cast<std::uint16_t>(pc & 0xfff0u);
                for (std::size_t byte = 0; byte < 16; ++byte)
                    instruction_cache_.at(line * 16 + byte) = programByte(
                        static_cast<std::uint16_t>(first_address + byte));
                cache_valid_.at(line) = true;
            }
            return instruction_cache_.at(cache_offset);
        }
        return programByte(pc);
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
        } else if (opcode == 2) {
            const auto next_base = static_cast<std::uint16_t>(registers_[15] & 0xfff0u);
            if (cache_base_ != next_base) {
                cache_base_ = next_base;
                cache_valid_.fill(false);
            }
            ++cache_requests_;
            resetSelectors();
        } else if (opcode >= 5 && opcode <= 15) {
            ++metrics_.branches;
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
            if (taken) {
                ++metrics_.taken_branches;
                write(15, static_cast<std::uint16_t>(registers_[15] + offset));
            }
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
            ram_address_ = registers_[index];
            const auto address = static_cast<std::size_t>(ram_bank_) * 65536 + registers_[index];
            memoryAccess(0x700000u + static_cast<std::uint32_t>(address), true, alt1_);
            ++writes_[static_cast<std::uint32_t>(0x700000 + address)];
            const auto value = registers_[source_];
            ram_.at(address) = static_cast<std::uint8_t>(value);
            if (!alt1_) ram_.at(address ^ 1u) = static_cast<std::uint8_t>(value >> 8);
            resetSelectors();
        } else if (opcode == 0x3c) {
            registers_[12] = static_cast<std::uint16_t>(registers_[12] - 1);
            setZeroSign(registers_[12]);
            if (!zero_) write(15, registers_[13]);
            resetSelectors();
        } else if (opcode >= 0x3d && opcode <= 0x3f) {
            if (opcode != 0x3e) alt1_ = true;
            if (opcode != 0x3d) alt2_ = true;
            prefix_ = false;
        } else if (opcode >= 0x40 && opcode <= 0x4b) {
            const auto address = registers_[index];
            ram_address_ = address;
            memoryAccess(0x700000u + static_cast<std::uint32_t>(ram_bank_) * 65536u + address, false, alt1_);
            ++reads_[0x700000u + static_cast<std::uint32_t>(ram_bank_) * 65536u + address];
            write(destination_, alt1_ ? ram_.at(static_cast<std::size_t>(ram_bank_) * 65536 + address) : word(address));
            resetSelectors();
        } else if (opcode == 0x4c) {
            if (alt1_) {
                const auto value = graphics_.read(static_cast<std::uint8_t>(registers_[1]), static_cast<std::uint8_t>(registers_[2]));
                write(destination_, value); setZeroSign(value);
            } else {
                graphics_.plot(static_cast<std::uint8_t>(registers_[1]), static_cast<std::uint8_t>(registers_[2]));
                registers_[1] = static_cast<std::uint16_t>(registers_[1] + 1);
            }
            resetSelectors();
        } else if (opcode == 0x4e) {
            if (alt1_) graphics_.options(static_cast<std::uint8_t>(registers_[source_]));
            else graphics_.color(static_cast<std::uint8_t>(registers_[source_]));
            resetSelectors();
        } else if (opcode == 0x9f && alt1_) {
            const auto signedWord = [](std::uint16_t bits) -> std::int32_t {
                return bits < 32768 ? bits : static_cast<std::int32_t>(bits) - 65536;
            };
            const auto product = static_cast<std::uint32_t>(signedWord(registers_[source_]) * signedWord(registers_[6]));
            registers_[4] = static_cast<std::uint16_t>(product);
            write(destination_, static_cast<std::uint16_t>(product >> 16));
            setZeroSign(registers_[destination_]); resetSelectors();
        } else if (opcode == 0x9e || opcode == 0xc0) {
            const auto value = static_cast<std::uint16_t>((opcode == 0x9e ? registers_[source_] : registers_[source_] >> 8) & 255u);
            // LOB/HIB zero-extend the byte, but S is bit 7 (unlike word ALU).
            write(destination_, value); zero_ = value == 0; sign_ = (value & 0x80u) != 0; resetSelectors();
        } else if (opcode == 0x4d) {
            const auto source = registers_[source_];
            const auto value = static_cast<std::uint16_t>((source >> 8) | (source << 8));
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
        } else if (opcode == 0x03 || opcode == 0x04 || opcode == 0x96 || opcode == 0x97) {
            const auto source = registers_[source_];
            std::uint16_t value;
            if (opcode == 0x04) {
                value = static_cast<std::uint16_t>((source << 1) | (carry_ ? 1u : 0u));
                carry_ = (source & 0x8000u) != 0;
            } else {
                value = static_cast<std::uint16_t>((source >> 1) |
                    (opcode == 0x96 ? source & 0x8000u : opcode == 0x97 && carry_ ? 0x8000u : 0));
                if (opcode == 0x96 && alt1_ && source == 0xffffu) ++value; // DIV2 rounds -1 to zero.
                carry_ = (source & 1u) != 0;
            }
            write(destination_, value); setZeroSign(value); resetSelectors();
        } else if (opcode >= 0xc1 && opcode <= 0xcf) {
            const auto right = alt2_ ? static_cast<std::uint16_t>(index) : registers_[index];
            const auto value = static_cast<std::uint16_t>(alt1_ ? registers_[source_] ^ right : registers_[source_] | right);
            write(destination_, value); setZeroSign(value); resetSelectors();
        } else if (opcode >= 0x71 && opcode <= 0x7f) {
            const auto right = alt2_ ? static_cast<std::uint16_t>(index) : registers_[index];
            const auto value = static_cast<std::uint16_t>(registers_[source_] & (alt1_ ? ~right : right));
            write(destination_, value); setZeroSign(value); resetSelectors();
        } else if (opcode >= 0x80 && opcode <= 0x8f) {
            const auto right = alt2_ ? static_cast<std::uint16_t>(index) : registers_[index];
            const auto left_byte = registers_[source_] & 255u, right_byte = right & 255u;
            const auto signedByte = [](unsigned byte) { return byte < 128 ? static_cast<int>(byte) : static_cast<int>(byte) - 256; };
            const auto product = alt1_ ? static_cast<int>(left_byte * right_byte) : signedByte(left_byte) * signedByte(right_byte);
            const auto value = static_cast<std::uint16_t>(product);
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
        } else if (opcode == 0x90) {
            // SBK always writes a word using the most recent RAM address,
            // even after LDB/STB or ALT1. It does not change NZCV or the latch.
            const auto address = static_cast<std::size_t>(ram_bank_) * 65536 + ram_address_;
            const auto value = registers_[source_];
            memoryAccess(0x700000u + static_cast<std::uint32_t>(address), true, false);
            ++writes_[0x700000u + static_cast<std::uint32_t>(address)];
            ++metrics_.store_backs;
            ram_.at(address) = static_cast<std::uint8_t>(value);
            ram_.at(address ^ 1u) = static_cast<std::uint8_t>(value >> 8);
            resetSelectors();
        } else if (opcode >= 0x91 && opcode <= 0x94) {
            ++metrics_.calls;
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
        } else if (opcode == 0xdf) {
            if (!alt2_) {
                ++metrics_.rom_reads;
                const auto address = (static_cast<std::uint32_t>(rom_bank_) << 16) | registers_[14];
                ++reads_[address]; graphics_.color(rom_.at(romIndex(address)), true);
            } else if (alt1_) rom_bank_ = static_cast<std::uint8_t>(registers_[source_] & 0x7fu);
            else ram_bank_ = static_cast<std::uint8_t>(registers_[source_] & 1u);
            resetSelectors();
        } else if (opcode == 0xef) {
            ++metrics_.rom_reads;
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
    GSUGraphicsModel graphics_;
    std::map<std::uint32_t, unsigned> reads_, writes_;
    ExecutionMetrics metrics_;
    std::uint32_t stack_begin_ = 0, stack_end_ = 0;
    std::uint8_t ram_bank_ = 0;
    std::uint16_t ram_address_ = 0;
    std::uint32_t program_bank_ = 0;
    std::uint8_t rom_bank_ = 0;
    // Functional cache contents/CBR only: no bus stalls or cycle timings.
    std::array<std::uint8_t, 512> instruction_cache_{};
    std::array<bool, 32> cache_valid_{};
    std::uint16_t cache_base_ = 0;
    unsigned cache_requests_ = 0;
    std::array<std::uint16_t, 16> registers_{};
    std::uint8_t pipeline_ = 1;
    std::size_t source_ = 0, destination_ = 0;
    bool pc_written_ = false, prefix_ = false, alt1_ = false, alt2_ = false;
    bool zero_ = false, sign_ = false, carry_ = false, overflow_ = false;
};

} // namespace DiscoGSU
