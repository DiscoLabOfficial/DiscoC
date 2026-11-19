#pragma once

// Instruction-level SPC-700 regression model, not an APU emulator. It executes
// linked SPC700 payloads for toolchain tests: no cycle timing, no running
// timers, no S-DSP. Decoding (sizes, operand layout) comes from the shared
// SPC700Isa opcode map; the semantics below are written independently of the
// assembler. Anything the model does not support fails closed with an
// exception instead of guessing.
//
// The IPL ROM is not embedded. A jump to $FFC0 while the IPL is mapped ends a
// run ("returned to the IPL"), and the IPL's observable hand-off effects are
// applied when a run starts: SP = $EF, $0001-$00EF cleared and $0000/$0001
// holding the start address.
//
// Flag behavior follows the SNESdev SPC-700 instruction-set notes; the
// optional spc700_luna_differential test compares generated per-instruction
// results (including DIV, DAA/DAS and half-carry edge cases) with the Luna
// emulator.

#include "SPC700Isa.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace DiscoSPC700 {

namespace Flag {
constexpr std::uint8_t N = 0x80, V = 0x40, P = 0x20, B = 0x10, H = 0x08, I = 0x04, Z = 0x02, C = 0x01;
}

struct ExecutionMetrics {
    std::uint64_t instructions = 0;
    std::uint64_t loads = 0, stores = 0;
    std::uint64_t branches = 0, taken_branches = 0, calls = 0, returns = 0;
};

class Machine {
public:
    static constexpr std::uint16_t IplEntry = 0xFFC0;

    // `payload` is placed at `origin`; execution may only fetch from it.
    // `fill` initializes the rest of RAM so tests notice reads of state the
    // program never wrote.
    Machine(const std::vector<std::uint8_t>& payload, std::uint16_t origin, std::uint8_t fill = 0)
        : ram_{}, origin_(origin), size_(payload.size()) {
        if (payload.empty() || payload.size() > 0x10000u - origin)
            throw std::runtime_error("SPC700 payload does not fit in the 64 KiB address space");
        ram_.fill(fill);
        for (std::size_t index = 0; index < payload.size(); ++index) ram_[origin + index] = payload[index];
    }
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // Applies the IPL hand-off and starts executing at `entry`.
    void start(std::uint16_t entry) {
        for (unsigned address = 0x01; address <= 0xEF; ++address) ram_[address] = 0;
        ram_[0x00] = static_cast<std::uint8_t>(entry);
        ram_[0x01] = static_cast<std::uint8_t>(entry >> 8);
        a_ = 0xA5; x_ = 0x5A; y_ = 0xC3; // Not defined by the hand-off.
        sp_ = 0xEF;
        psw_ = 0;
        pc_ = entry;
        ipl_mapped_ = true;
        returned_ = false;
        out_[0] = 0xCC;
        out_[1] = 0xBB;
    }

    // Runs until the program returns to the IPL. Throws on any unsupported
    // operation or when `limit` instructions execute first.
    void run(std::uint64_t limit = 10000000) {
        if (limit == 0 || limit > 100000000) throw std::invalid_argument("SPC700 instruction limit must be in 1..100000000");
        for (std::uint64_t step = 0; step < limit; ++step) {
            if (ipl_mapped_ && pc_ == IplEntry) {
                returned_ = true;
                // The IPL announces itself again on ports 0/1.
                out_[0] = 0xAA;
                out_[1] = 0xBB;
                return;
            }
            execute();
            ++metrics_.instructions;
        }
        fail("execution exceeded the instruction limit");
    }

    bool returnedToIpl() const { return returned_; }
    std::uint8_t a() const { return a_; }
    std::uint8_t x() const { return x_; }
    std::uint8_t y() const { return y_; }
    std::uint8_t sp() const { return sp_; }
    std::uint8_t psw() const { return psw_; }
    std::uint16_t pc() const { return pc_; }
    std::uint8_t byte(std::uint16_t address) const { return ram_[address]; }
    std::uint16_t word(std::uint16_t address) const {
        return static_cast<std::uint16_t>(ram_[address] | (ram_[static_cast<std::uint16_t>(address + 1)] << 8));
    }
    void seed(std::uint16_t address, std::uint8_t value) { ram_[address] = value; }
    void setInputPort(unsigned port, std::uint8_t value) { in_.at(port) = value; }
    std::uint8_t outputPort(unsigned port) const { return out_.at(port); }
    std::uint8_t dspRegister(std::uint8_t index) const { return dsp_.at(index & 0x7F); }
    const ExecutionMetrics& metrics() const { return metrics_; }
    const std::array<std::uint8_t, 0x10000>& memory() const { return ram_; }

    // Test hooks for hand-encoded vectors.
    void setRegisters(std::uint8_t a, std::uint8_t x, std::uint8_t y, std::uint8_t sp, std::uint8_t psw) {
        a_ = a; x_ = x; y_ = y; sp_ = sp; psw_ = psw;
    }

private:
    using Pattern = SPC700Isa::OperandPattern;

    std::array<std::uint8_t, 0x10000> ram_;
    std::array<std::uint8_t, 128> dsp_{};
    std::array<std::uint8_t, 4> in_{}, out_{};
    std::uint16_t origin_;
    std::size_t size_;
    std::uint8_t a_ = 0, x_ = 0, y_ = 0, sp_ = 0xEF, psw_ = 0;
    std::uint16_t pc_ = 0;
    std::uint8_t dsp_address_ = 0;
    bool ipl_mapped_ = true;
    bool returned_ = false;
    ExecutionMetrics metrics_;

    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error("SPC700 model at $" + hex(pc_) + ": " + message);
    }
    static std::string hex(unsigned value) {
        static const char* digits = "0123456789ABCDEF";
        std::string text(4, '0');
        for (int index = 3; index >= 0; --index, value >>= 4) text[static_cast<std::size_t>(index)] = digits[value & 15];
        return text;
    }

    bool flag(std::uint8_t mask) const { return (psw_ & mask) != 0; }
    void setFlag(std::uint8_t mask, bool on) { psw_ = static_cast<std::uint8_t>(on ? psw_ | mask : psw_ & ~mask); }
    void setNZ(std::uint8_t value) { setFlag(Flag::N, (value & 0x80) != 0); setFlag(Flag::Z, value == 0); }

    // ---- Memory and I/O -------------------------------------------------

    std::uint8_t read(std::uint16_t address) {
        ++metrics_.loads;
        if (address >= 0xF0 && address <= 0xFF) return readIo(address);
        if (ipl_mapped_ && address >= IplEntry) fail("read of $" + hex(address) + " while the IPL ROM is mapped");
        return ram_[address];
    }
    void write(std::uint16_t address, std::uint8_t value) {
        ++metrics_.stores;
        if (address >= 0xF0 && address <= 0xFF) {
            writeIo(address, value);
            return;
        }
        ram_[address] = value; // Writes under the IPL ROM reach RAM.
    }
    std::uint8_t readIo(std::uint16_t address) {
        switch (address) {
            case 0xF2: return dsp_address_;
            case 0xF3: return dsp_[dsp_address_ & 0x7F];
            case 0xF4: case 0xF5: case 0xF6: case 0xF7: return in_[address - 0xF4];
            case 0xF8: case 0xF9: return ram_[address];
            case 0xFD: case 0xFE: case 0xFF: return 0; // Timers never tick in this model.
            default: fail("read of write-only or unsupported register $" + hex(address));
        }
    }
    void writeIo(std::uint16_t address, std::uint8_t value) {
        switch (address) {
            case 0xF1:
                ipl_mapped_ = (value & 0x80) != 0;
                if (value & 0x10) in_[0] = in_[1] = 0;
                if (value & 0x20) in_[2] = in_[3] = 0;
                return;
            case 0xF2: dsp_address_ = value; return;
            case 0xF3:
                if (dsp_address_ < 0x80) dsp_[dsp_address_] = value;
                return;
            case 0xF4: case 0xF5: case 0xF6: case 0xF7: out_[address - 0xF4] = value; return;
            case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: ram_[address] = value; return;
            case 0xFD: case 0xFE: case 0xFF: return; // Counters are read-only.
            default: fail("write to unsupported register $" + hex(address));
        }
    }
    std::uint16_t dp(unsigned offset) const {
        return static_cast<std::uint16_t>((flag(Flag::P) ? 0x100 : 0) | (offset & 0xFF));
    }
    std::uint16_t readDpWord(unsigned offset) {
        const auto low = read(dp(offset));
        return static_cast<std::uint16_t>(low | (read(dp(offset + 1)) << 8));
    }
    void writeDpWord(unsigned offset, std::uint16_t value) {
        write(dp(offset), static_cast<std::uint8_t>(value));
        write(dp(offset + 1), static_cast<std::uint8_t>(value >> 8));
    }
    void push(std::uint8_t value) {
        write(static_cast<std::uint16_t>(0x100 | sp_), value);
        --sp_;
    }
    std::uint8_t pop() {
        ++sp_;
        return read(static_cast<std::uint16_t>(0x100 | sp_));
    }
    void pushPc() {
        push(static_cast<std::uint8_t>(pc_ >> 8));
        push(static_cast<std::uint8_t>(pc_));
    }
    void popPc() {
        const auto low = pop();
        pc_ = static_cast<std::uint16_t>(low | (pop() << 8));
    }
    std::uint8_t fetchByte(std::uint16_t address) const {
        if (address < origin_ || static_cast<std::size_t>(address - origin_) >= size_)
            fail("instruction fetch at $" + hex(address) + " is outside the loaded payload");
        return ram_[address];
    }

    // ---- ALU ------------------------------------------------------------

    std::uint8_t adc(std::uint8_t left, std::uint8_t right) {
        const unsigned result = left + right + (flag(Flag::C) ? 1u : 0u);
        setFlag(Flag::C, result > 0xFF);
        setFlag(Flag::H, ((left ^ right ^ result) & 0x10) != 0);
        setFlag(Flag::V, (~(left ^ right) & (left ^ result) & 0x80) != 0);
        setNZ(static_cast<std::uint8_t>(result));
        return static_cast<std::uint8_t>(result);
    }
    std::uint8_t sbc(std::uint8_t left, std::uint8_t right) { return adc(left, static_cast<std::uint8_t>(~right)); }
    void compare(std::uint8_t left, std::uint8_t right) {
        const int result = left - right;
        setFlag(Flag::C, result >= 0);
        setNZ(static_cast<std::uint8_t>(result));
    }

    // ---- Operand locations ----------------------------------------------

    enum class Kind { Register, Memory, Immediate };
    struct Location {
        Kind kind = Kind::Immediate;
        Pattern reg = Pattern::None;
        std::uint16_t address = 0;
        std::uint8_t value = 0;
    };

    std::uint8_t& registerRef(Pattern reg) {
        switch (reg) {
            case Pattern::A: return a_;
            case Pattern::X: return x_;
            case Pattern::Y: return y_;
            case Pattern::SP: return sp_;
            case Pattern::PSW: return psw_;
            default: fail("operand is not an 8-bit register");
        }
    }
    std::uint8_t get(const Location& location) {
        switch (location.kind) {
            case Kind::Register: return registerRef(location.reg);
            case Kind::Memory: return read(location.address);
            case Kind::Immediate: return location.value;
        }
        return 0;
    }
    void put(const Location& location, std::uint8_t value) {
        if (location.kind == Kind::Register) registerRef(location.reg) = value;
        else if (location.kind == Kind::Memory) write(location.address, value);
        else fail("cannot store to an immediate operand");
    }

    // Raw operand value (8- or 16-bit) as encoded, in source order.
    struct Decoded {
        const SPC700Isa::InstructionInfo* info = nullptr;
        unsigned count = 0;
        std::uint16_t raw[2] = {0, 0};
        std::uint16_t next = 0;
    };

    Decoded decode() {
        Decoded decoded;
        const auto opcode = fetchByte(pc_);
        decoded.info = &SPC700Isa::instruction(opcode);
        const auto& info = *decoded.info;
        decoded.count = info.operands[0] == Pattern::None ? 0u : info.operands[1] == Pattern::None ? 1u : 2u;
        std::uint16_t position[2] = {static_cast<std::uint16_t>(pc_ + 1), static_cast<std::uint16_t>(pc_ + 1)};
        if (decoded.count == 2) {
            const auto first = SPC700Isa::operandBytes(info.operands[0]);
            const auto second = SPC700Isa::operandBytes(info.operands[1]);
            if (info.layout == SPC700Isa::OperandLayout::Reversed) position[0] = static_cast<std::uint16_t>(pc_ + 1 + second);
            else position[1] = static_cast<std::uint16_t>(pc_ + 1 + first);
        }
        for (unsigned index = 0; index < decoded.count; ++index) {
            const auto bytes = SPC700Isa::operandBytes(info.operands[index]);
            if (bytes == 1) decoded.raw[index] = fetchByte(position[index]);
            else if (bytes == 2)
                decoded.raw[index] = static_cast<std::uint16_t>(fetchByte(position[index]) |
                    (fetchByte(static_cast<std::uint16_t>(position[index] + 1)) << 8));
        }
        decoded.next = static_cast<std::uint16_t>(pc_ + info.size);
        return decoded;
    }

    // Effective location of a data operand. `(X)+` increments X afterwards,
    // through `post_increment_x`.
    Location locate(Pattern pattern, std::uint16_t raw, bool& post_increment_x) {
        Location location;
        location.kind = Kind::Memory;
        switch (pattern) {
            case Pattern::A: case Pattern::X: case Pattern::Y: case Pattern::SP: case Pattern::PSW:
                location.kind = Kind::Register;
                location.reg = pattern;
                return location;
            case Pattern::Imm:
                location.kind = Kind::Immediate;
                location.value = static_cast<std::uint8_t>(raw);
                return location;
            case Pattern::Dp: location.address = dp(raw); return location;
            case Pattern::DpX: location.address = dp(raw + x_); return location;
            case Pattern::DpY: location.address = dp(raw + y_); return location;
            case Pattern::Abs: location.address = raw; return location;
            case Pattern::AbsX: location.address = static_cast<std::uint16_t>(raw + x_); return location;
            case Pattern::AbsY: location.address = static_cast<std::uint16_t>(raw + y_); return location;
            case Pattern::IndX: location.address = dp(x_); return location;
            case Pattern::IndXInc: location.address = dp(x_); post_increment_x = true; return location;
            case Pattern::IndY: location.address = dp(y_); return location;
            case Pattern::DpXInd: location.address = readDpWord(raw + x_); return location;
            case Pattern::DpIndY: location.address = static_cast<std::uint16_t>(readDpWord(raw) + y_); return location;
            default: fail("operand pattern is not a data operand");
        }
    }

    std::uint16_t branchTarget(const Decoded& decoded) const {
        const auto raw = decoded.raw[decoded.count - 1];
        const int displacement = raw < 0x80 ? static_cast<int>(raw) : static_cast<int>(raw) - 256;
        return static_cast<std::uint16_t>(decoded.next + displacement);
    }
    void branch(bool taken, const Decoded& decoded) {
        ++metrics_.branches;
        pc_ = decoded.next;
        if (taken) {
            ++metrics_.taken_branches;
            pc_ = branchTarget(decoded);
        }
    }
    // mem.bit operand: 13-bit address, bit index in the top three bits.
    static std::uint16_t memBitAddress(std::uint16_t raw) { return static_cast<std::uint16_t>(raw & 0x1FFF); }
    static unsigned memBitIndex(std::uint16_t raw) { return raw >> 13; }

    // ---- Execution ------------------------------------------------------

    void execute() {
        const auto decoded = decode();
        const auto& info = *decoded.info;
        const std::string mnemonic = info.mnemonic;
        bool increment_x = false;

        // Control flow first: these set PC themselves.
        if (mnemonic == "bra") return branch(true, decoded);
        if (mnemonic == "bpl") return branch(!flag(Flag::N), decoded);
        if (mnemonic == "bmi") return branch(flag(Flag::N), decoded);
        if (mnemonic == "bvc") return branch(!flag(Flag::V), decoded);
        if (mnemonic == "bvs") return branch(flag(Flag::V), decoded);
        if (mnemonic == "bcc") return branch(!flag(Flag::C), decoded);
        if (mnemonic == "bcs") return branch(flag(Flag::C), decoded);
        if (mnemonic == "bne") return branch(!flag(Flag::Z), decoded);
        if (mnemonic == "beq") return branch(flag(Flag::Z), decoded);
        if (mnemonic == "bbs" || mnemonic == "bbc") {
            const auto value = read(dp(decoded.raw[0]));
            const bool set = (value >> info.fixed) & 1;
            return branch(mnemonic == "bbs" ? set : !set, decoded);
        }
        if (mnemonic == "cbne") {
            const auto address = info.operands[0] == Pattern::DpX ? dp(decoded.raw[0] + x_) : dp(decoded.raw[0]);
            return branch(read(address) != a_, decoded);
        }
        if (mnemonic == "dbnz") {
            if (info.operands[0] == Pattern::Y) {
                --y_;
                return branch(y_ != 0, decoded);
            }
            const auto address = dp(decoded.raw[0]);
            const auto value = static_cast<std::uint8_t>(read(address) - 1);
            write(address, value);
            return branch(value != 0, decoded);
        }
        if (mnemonic == "jmp") {
            if (info.operands[0] == Pattern::AbsXInd) {
                const auto pointer = static_cast<std::uint16_t>(decoded.raw[0] + x_);
                const auto low = read(pointer);
                pc_ = static_cast<std::uint16_t>(low | (read(static_cast<std::uint16_t>(pointer + 1)) << 8));
            } else {
                pc_ = decoded.raw[0];
            }
            return;
        }
        if (mnemonic == "call" || mnemonic == "pcall" || mnemonic == "tcall") {
            ++metrics_.calls;
            pc_ = decoded.next;
            pushPc();
            if (mnemonic == "call") pc_ = decoded.raw[0];
            else if (mnemonic == "pcall") pc_ = static_cast<std::uint16_t>(0xFF00 | decoded.raw[0]);
            else {
                const auto vector = static_cast<std::uint16_t>(0xFFDE - 2 * info.fixed);
                const auto low = read(vector);
                pc_ = static_cast<std::uint16_t>(low | (read(static_cast<std::uint16_t>(vector + 1)) << 8));
            }
            return;
        }
        if (mnemonic == "brk") {
            pc_ = decoded.next;
            pushPc();
            push(psw_);
            setFlag(Flag::B, true);
            setFlag(Flag::I, false);
            const auto low = read(0xFFDE);
            pc_ = static_cast<std::uint16_t>(low | (read(0xFFDF) << 8));
            return;
        }
        if (mnemonic == "ret") { ++metrics_.returns; popPc(); return; }
        if (mnemonic == "reti") { ++metrics_.returns; psw_ = pop(); popPc(); return; }
        if (mnemonic == "sleep" || mnemonic == "stop") fail(mnemonic + " halts the processor and is not modeled");

        pc_ = decoded.next;

        if (mnemonic == "nop") return;
        if (mnemonic == "clrp") return setFlag(Flag::P, false);
        if (mnemonic == "setp") return setFlag(Flag::P, true);
        if (mnemonic == "clrc") return setFlag(Flag::C, false);
        if (mnemonic == "setc") return setFlag(Flag::C, true);
        if (mnemonic == "ei") return setFlag(Flag::I, true);
        if (mnemonic == "di") return setFlag(Flag::I, false);
        if (mnemonic == "clrv") { setFlag(Flag::V, false); setFlag(Flag::H, false); return; }
        if (mnemonic == "notc") return setFlag(Flag::C, !flag(Flag::C));

        if (mnemonic == "push") return push(registerRef(info.operands[0]));
        if (mnemonic == "pop") { registerRef(info.operands[0]) = pop(); return; }

        if (mnemonic == "set1" || mnemonic == "clr1") {
            const auto address = dp(decoded.raw[0]);
            const auto mask = static_cast<std::uint8_t>(1u << info.fixed);
            const auto value = read(address);
            write(address, static_cast<std::uint8_t>(mnemonic == "set1" ? value | mask : value & ~mask));
            return;
        }
        if (mnemonic == "tset1" || mnemonic == "tclr1") {
            const auto value = read(decoded.raw[0]);
            setNZ(static_cast<std::uint8_t>(a_ - value));
            write(decoded.raw[0], static_cast<std::uint8_t>(mnemonic == "tset1" ? value | a_ : value & ~a_));
            return;
        }
        if (mnemonic == "or1" || mnemonic == "and1" || mnemonic == "eor1" || mnemonic == "mov1" || mnemonic == "not1") {
            return bitInstruction(mnemonic, info, decoded);
        }

        if (mnemonic == "movw") {
            if (info.operands[0] == Pattern::YA) {
                const auto value = readDpWord(decoded.raw[1]);
                a_ = static_cast<std::uint8_t>(value);
                y_ = static_cast<std::uint8_t>(value >> 8);
                setFlag(Flag::N, (value & 0x8000) != 0);
                setFlag(Flag::Z, value == 0);
            } else {
                writeDpWord(decoded.raw[0], static_cast<std::uint16_t>(a_ | (y_ << 8)));
            }
            return;
        }
        if (mnemonic == "incw" || mnemonic == "decw") {
            const auto value = static_cast<std::uint16_t>(readDpWord(decoded.raw[0]) + (mnemonic == "incw" ? 1 : -1));
            writeDpWord(decoded.raw[0], value);
            setFlag(Flag::N, (value & 0x8000) != 0);
            setFlag(Flag::Z, value == 0);
            return;
        }
        if (mnemonic == "addw" || mnemonic == "subw" || mnemonic == "cmpw") {
            const auto operand = readDpWord(decoded.raw[1]);
            const auto ya = static_cast<std::uint16_t>(a_ | (y_ << 8));
            if (mnemonic == "cmpw") {
                const int result = ya - operand;
                setFlag(Flag::C, result >= 0);
                setFlag(Flag::N, (result & 0x8000) != 0);
                setFlag(Flag::Z, static_cast<std::uint16_t>(result) == 0);
                return;
            }
            // Two chained 8-bit operations: carry starts clear for ADDW and
            // set for SUBW; N, V, H and C come from the high byte.
            const bool add = mnemonic == "addw";
            setFlag(Flag::C, !add);
            const auto low = add ? adc(static_cast<std::uint8_t>(ya), static_cast<std::uint8_t>(operand))
                                 : sbc(static_cast<std::uint8_t>(ya), static_cast<std::uint8_t>(operand));
            const auto high = add ? adc(static_cast<std::uint8_t>(ya >> 8), static_cast<std::uint8_t>(operand >> 8))
                                  : sbc(static_cast<std::uint8_t>(ya >> 8), static_cast<std::uint8_t>(operand >> 8));
            a_ = low;
            y_ = high;
            setFlag(Flag::Z, (low | high) == 0);
            return;
        }
        if (mnemonic == "mul") {
            const auto product = static_cast<std::uint16_t>(y_ * a_);
            a_ = static_cast<std::uint8_t>(product);
            y_ = static_cast<std::uint8_t>(product >> 8);
            setNZ(y_);
            return;
        }
        if (mnemonic == "div") return divide();
        if (mnemonic == "daa") {
            if (flag(Flag::C) || a_ > 0x99) { a_ = static_cast<std::uint8_t>(a_ + 0x60); setFlag(Flag::C, true); }
            if (flag(Flag::H) || (a_ & 15) > 9) a_ = static_cast<std::uint8_t>(a_ + 0x06);
            setNZ(a_);
            return;
        }
        if (mnemonic == "das") {
            if (!flag(Flag::C) || a_ > 0x99) { a_ = static_cast<std::uint8_t>(a_ - 0x60); setFlag(Flag::C, false); }
            if (!flag(Flag::H) || (a_ & 15) > 9) a_ = static_cast<std::uint8_t>(a_ - 0x06);
            setNZ(a_);
            return;
        }
        if (mnemonic == "xcn") {
            a_ = static_cast<std::uint8_t>((a_ >> 4) | (a_ << 4));
            setNZ(a_);
            return;
        }

        // Remaining forms read/modify data operands.
        Location first = locate(info.operands[0], decoded.raw[0], increment_x);
        if (decoded.count == 1) {
            const auto value = get(first);
            std::uint8_t result = value;
            if (mnemonic == "asl") { setFlag(Flag::C, (value & 0x80) != 0); result = static_cast<std::uint8_t>(value << 1); }
            else if (mnemonic == "lsr") { setFlag(Flag::C, (value & 1) != 0); result = static_cast<std::uint8_t>(value >> 1); }
            else if (mnemonic == "rol") {
                result = static_cast<std::uint8_t>((value << 1) | (flag(Flag::C) ? 1 : 0));
                setFlag(Flag::C, (value & 0x80) != 0);
            } else if (mnemonic == "ror") {
                result = static_cast<std::uint8_t>((value >> 1) | (flag(Flag::C) ? 0x80 : 0));
                setFlag(Flag::C, (value & 1) != 0);
            } else if (mnemonic == "inc") result = static_cast<std::uint8_t>(value + 1);
            else if (mnemonic == "dec") result = static_cast<std::uint8_t>(value - 1);
            else fail("unsupported instruction '" + mnemonic + "'");
            setNZ(result);
            put(first, result);
            if (increment_x) ++x_;
            return;
        }

        Location second = locate(info.operands[1], decoded.raw[1], increment_x);
        if (mnemonic == "mov") {
            const auto value = get(second);
            put(first, value);
            // Register destinations set N/Z, except MOV SP,X.
            if (first.kind == Kind::Register && first.reg != Pattern::SP) setNZ(value);
        } else if (mnemonic == "cmp") {
            compare(get(first), get(second));
        } else {
            const auto right = get(second);
            const auto left = get(first);
            std::uint8_t result = 0;
            if (mnemonic == "or") { result = static_cast<std::uint8_t>(left | right); setNZ(result); }
            else if (mnemonic == "and") { result = static_cast<std::uint8_t>(left & right); setNZ(result); }
            else if (mnemonic == "eor") { result = static_cast<std::uint8_t>(left ^ right); setNZ(result); }
            else if (mnemonic == "adc") result = adc(left, right);
            else if (mnemonic == "sbc") result = sbc(left, right);
            else fail("unsupported instruction '" + mnemonic + "'");
            put(first, result);
        }
        if (increment_x) ++x_;
    }

    void bitInstruction(const std::string& mnemonic, const SPC700Isa::InstructionInfo& info, const Decoded& decoded) {
        const bool to_memory = info.operands[0] == Pattern::MemBit;
        const auto raw = to_memory ? decoded.raw[0] : decoded.raw[1];
        const auto address = memBitAddress(raw);
        const auto index = memBitIndex(raw);
        const auto value = read(address);
        bool bit = ((value >> index) & 1) != 0;
        if (mnemonic == "mov1" && to_memory) {
            const auto mask = static_cast<std::uint8_t>(1u << index);
            write(address, static_cast<std::uint8_t>(flag(Flag::C) ? value | mask : value & ~mask));
            return;
        }
        if (mnemonic == "not1") {
            write(address, static_cast<std::uint8_t>(value ^ (1u << index)));
            return;
        }
        if (info.operands[1] == Pattern::NotMemBit) bit = !bit;
        if (mnemonic == "or1") setFlag(Flag::C, flag(Flag::C) || bit);
        else if (mnemonic == "and1") setFlag(Flag::C, flag(Flag::C) && bit);
        else if (mnemonic == "eor1") setFlag(Flag::C, flag(Flag::C) != bit);
        else setFlag(Flag::C, bit); // mov1 c,mem.bit
    }

    // DIV YA,X: 9-bit quotient behavior of the hardware divider, including
    // results for divisors that overflow an 8-bit quotient and X = 0.
    void divide() {
        const unsigned ya = static_cast<unsigned>(a_ | (y_ << 8));
        const unsigned divisor = x_;
        setFlag(Flag::H, (y_ & 15) >= (x_ & 15));
        setFlag(Flag::V, y_ >= x_);
        if (y_ < (divisor << 1)) {
            a_ = static_cast<std::uint8_t>(ya / divisor);
            y_ = static_cast<std::uint8_t>(ya % divisor);
        } else {
            a_ = static_cast<std::uint8_t>(255 - (ya - (divisor << 9)) / (256 - divisor));
            y_ = static_cast<std::uint8_t>(divisor + (ya - (divisor << 9)) % (256 - divisor));
        }
        setNZ(a_);
    }
};

} // namespace DiscoSPC700
