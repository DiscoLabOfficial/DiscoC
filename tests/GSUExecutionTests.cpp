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
#include "GSUGraphicsModel.hpp"

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
        } else if (opcode == 0x3c) {
            // LOOP: decrement R12, then branch to R13 while it is nonzero.
            // Like other branches, the following opcode is a delay slot.
            setZeroSign(--registers_[12]);
            if (!zero_) write(15, registers_[13]);
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
        } else if (opcode == 0xdf) {
            if (!alt2_) {
                const auto address = (static_cast<std::uint32_t>(rom_bank_) << 16) | registers_[14];
                ++reads_[address]; graphics_.color(rom_.at(romIndex(address)), true);
            } else if (alt1_) rom_bank_ = static_cast<std::uint8_t>(registers_[source_] & 0x7fu);
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
    GSUGraphicsModel graphics_;
    std::map<std::uint32_t, unsigned> reads_, writes_;
    std::uint8_t ram_bank_ = 0;
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

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void checkGraphicsAddressBoundaries() {
    struct Screen { std::uint8_t mode, base, last_y; };
    // 8bpp bitmap and OBJ configurations ending at the last cartridge RAM
    // byte. Check logical colors and independent raw bitplane expectations.
    const Screen screens[] = {{0x23, 80, 191}, {0x27, 64, 255}};
    for (const auto& screen : screens) {
        std::vector<std::uint8_t> ram(128 * 1024, 0);
        GSUGraphicsModel graphics(ram);
        graphics.configure(screen.mode, screen.base);
        graphics.options(1);
        graphics.color(255);
        graphics.plot(255, screen.last_y);
        require(graphics.read(255, screen.last_y) == 255 &&
                ram.back() == 1 && ram.front() == 0,
                "Graphics address calculation missed the last RAM bitplane byte");

        GSUGraphicsModel outside(ram);
        outside.configure(screen.mode, static_cast<std::uint8_t>(screen.base + 1));
        bool rejected = false;
        try {
            outside.read(255, screen.last_y);
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        require(rejected, "Graphics read outside cartridge RAM was not rejected");
    }
}

void selfTest() {
    checkGraphicsAddressBoundaries();
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
    Machine loop({0xf0, 0, 0, 0xfc, 3, 0, 0x2f, 0x1d, 0xd0, 0x3c, 0xd1, 0, 1}, 0x8000);
    loop.run();
    require(loop.reg(13) == 0x8008 && loop.reg(12) == 0 && loop.reg(0) == 3 && loop.reg(1) == 3,
            "MOVE R13,R15 / LOOP did not repeat the body with one delay slot");
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
    // Literal opcodes verify the graphics oracle independently of DiscoC's
    // instruction selector: PLOT increments X, RPIX flushes without doing so.
    Machine pixel({0xf1, 0, 0, 0xf2, 1, 0, 0xa0, 1, 0x3d, 0x4e,
                   0xa0, 3, 0x4e, 0x4c, 0x10, 0x3d, 0x4c, 0, 1}, 0x008000);
    pixel.run();
    require(pixel.reg(1) == 1 && pixel.reg(2) == 1 && pixel.reg(0) == 0 &&
            pixel.byte(0x706002) == 128 && pixel.byte(0x706003) == 128,
            "PLOT/RPIX cursor or bitplane behavior is incorrect");
    Machine getc({0xa0, 0xa2, 0x4e, 0xa0, 4, 0x3d, 0x4e,
                  0xfe, 0x00, 0x90, 0xdf, 0, 1}, 0x008000);
    getc.seed(0x009000, 0xb4, true); getc.run();
    require(getc.graphicsState("--colr") == 0xab && getc.reg(0) == 4 &&
            getc.graphicsState("--getc") == 1,
            "GETC did not read R14's ROM buffer and apply COLOR transformation");
    // CACHE sees the prefetched PC. Placing it at $600F tests the next-line
    // base and selector/ALT reset without relying on compiler code generation.
    Machine cacheSelectors({0xf0, 5, 0, 0xf1, 7, 0, 0x21, 0x3f, 0x02, 0x50, 0, 1}, 0x706007);
    cacheSelectors.run();
    require(cacheSelectors.reg(0) == 10 && cacheSelectors.reg(1) == 7 &&
            cacheSelectors.cacheBase() == 0x6010 && cacheSelectors.cacheRequests() == 1,
            "CACHE did not align the prefetched PC and reset register/ALT selectors");
    // A second CACHE with the same CBR must preserve the cached STOP even
    // after RAM was changed to an unsupported opcode at that address.
    Machine cachePreserved({0x02, 0xf0, 0x0f, 0x60, 0xf1, 0x9f, 0,
                            0x21, 0x3d, 0x30, 0x02, 0xff, 0x0f, 0x60, 1, 0, 1}, 0x706000);
    cachePreserved.run();
    require(cachePreserved.byte(0x70600f) == 0x9f && cachePreserved.cacheBase() == 0x6000 &&
            cachePreserved.cacheRequests() == 2,
            "CACHE incorrectly invalidated a line when CBR was unchanged");
    Machine cacheRebased({0x02, 0xf0, 0x0f, 0x60, 0xf1, 0x9f, 0,
                          0x21, 0x3d, 0x30, 0xff, 0x10, 0x60, 1, 1, 1,
                          0x02, 0xf0, 149, 0, 0, 1}, 0x706000);
    cacheRebased.run();
    require(cacheRebased.reg(0) == 149 && cacheRebased.cacheBase() == 0x6010 &&
            cacheRebased.cacheRequests() == 2,
            "CACHE did not invalidate old lines when CBR changed");
    std::vector<std::uint8_t> outsideCache{0x02, 0xf0, 0, 0x62, 0xf1, 0x9f, 0,
                                         0x21, 0x3d, 0x30, 0xff, 0, 0x62, 1};
    outsideCache.resize(0x202, 1);
    outsideCache.at(0x200) = 0;
    bool ram_fallback = false;
    try {
        Machine cacheBoundary(std::move(outsideCache), 0x706000);
        cacheBoundary.run();
    } catch (const std::runtime_error& error) {
        ram_fallback = std::string(error.what()).find("unsupported GSU opcode") != std::string::npos;
    }
    require(ram_fallback, "The byte immediately outside the cache window did not execute from RAM");
    bool cache_bounds_checked = false;
    try {
        Machine cacheBounds({0x02, 0xff, 0x10, 0x60, 1, 0, 1}, 0x706000);
        cacheBounds.run();
    } catch (const std::runtime_error& error) {
        cache_bounds_checked = std::string(error.what()).find("outside the linked payload") != std::string::npos;
    }
    require(cache_bounds_checked, "Cached instruction fetch bypassed the linked payload bounds");
    bool self_modified = false;
    try {
        Machine modification({0xf0, 0x0f, 0x60, 0xf1, 0x9f, 0, 0x21, 0x3d, 0x30,
                              0xff, 0x0f, 0x60, 1, 1, 1, 0, 1}, 0x706000);
        modification.run();
    } catch (const std::runtime_error& error) {
        self_modified = std::string(error.what()).find("unsupported GSU opcode") != std::string::npos;
    }
    require(self_modified, "RAM data writes did not modify subsequent instruction fetches");
    bool rom_seed_visible = false;
    try {
        Machine seededProgram({0xf0, 149, 0, 0, 1}, 0x008000);
        seededProgram.seed(0x008003, 0x9f, true);
        seededProgram.run();
    } catch (const std::runtime_error& error) {
        rom_seed_visible = std::string(error.what()).find("unsupported GSU opcode") != std::string::npos;
    }
    require(rom_seed_visible, "ROM instruction fetch and data reads used inconsistent physical storage");
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
            std::uint8_t screen_mode = 1, screen_base = 24;
            for (int index = 3; index < argc;) {
                const std::string option(argv[index]);
                if (option == "--screen-mode" || option == "--screen-base") {
                    if (index + 1 >= argc) throw std::runtime_error("missing screen configuration value");
                    const auto value = static_cast<std::uint8_t>(number(argv[index + 1], 255));
                    if (option == "--screen-mode") screen_mode = value; else screen_base = value;
                    index += 2; continue;
                }
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
                    option != "--rambr" && option != "--rombr" && option != "--reads" && option != "--writes" &&
                    option != "--colr" && option != "--por" && option != "--plots" && option != "--rpix" &&
                    option != "--getc" && option != "--color" && option != "--cmode" &&
                    option != "--cbr" && option != "--cache-count") throw std::runtime_error("unknown expectation");
                expectations.push_back({option, address, expected});
                index += 3;
            }
            if (expectations.empty()) throw std::runtime_error("at least one expectation is required");
            Machine machine(readPayload(argv[1]), number(argv[2], 0xffffff), initial_sp, initial_ram_bank);
            machine.configureScreen(screen_mode, screen_base);
            for (const auto& seed : seeds) machine.seed(seed.address, static_cast<std::uint8_t>(seed.expected), seed.option == "--rom-byte");
            machine.run();
            for (const auto& expectation : expectations) {
                const unsigned actual = expectation.option == "--word" ? machine.word(expectation.address) :
                    expectation.option == "--reads" ? machine.accesses(expectation.address, false) :
                    expectation.option == "--writes" ? machine.accesses(expectation.address, true) :
                    expectation.option == "--byte" ? machine.byte(expectation.address) :
                    expectation.option == "--rambr" ? machine.ramBank() :
                    expectation.option == "--rombr" ? machine.romBank() :
                    expectation.option == "--cbr" ? machine.cacheBase() :
                    expectation.option == "--cache-count" ? machine.cacheRequests() :
                    expectation.option == "--register" ? machine.reg(expectation.address) : machine.graphicsState(expectation.option);
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
