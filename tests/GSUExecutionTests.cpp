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
#include "GSUInstructionModel.hpp"

namespace {

using Machine = DiscoGSU::Machine;

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

void checkIncrementalTriangleEdges() {
    const int quarter[] = {0,6,12,19,24,30,36,41,45,49,53,56,59,61,63,64,64};
    const auto sine = [&](unsigned phase) {
        phase &= 63;
        const unsigned index = (phase & 16) ? 16 - (phase & 15) : phase & 15;
        return (phase & 32) ? -quarter[index] : quarter[index];
    };
    const auto floor = [](int value, int divisor) {
        return value / divisor - (value < 0 && value % divisor != 0 ? 1 : 0);
    };
    for (unsigned phase = 0; phase < 64; ++phase) {
        const int s = sine(phase), c = sine(phase + 16);
        const int horizontal[] = {-s,2*c+s,-2*c+s};
        const int vertical[] = {c,2*s-c,-2*s-c};
        const int boundary[] = {2048,3072,3072};
        for (unsigned edge = 0; edge < 3; ++edge) {
            const int a = horizontal[edge], d = a < 0 ? -a : a;
            const int initial = boundary[edge] + 56 * vertical[edge];
            const int delta = -vertical[edge];
            int q = d ? floor(initial,d) : initial;
            int r = d ? initial-q*d : 0;
            const int k = d ? floor(delta,d) : delta;
            const int step = d ? delta-k*d : 0;
            for (int y = 40; y < 152; ++y) {
                const int b = boundary[edge] - (y-96)*vertical[edge];
                require(b >= -32768 && b <= 32767 && q >= -32768 && q <= 32767,
                        "Triangle recurrence exceeded its signed-word proof");
                require(q == (d ? floor(b,d) : b), "Incremental edge changed floor rounding");
                require(!d || (r >= 0 && r < d && q*d+r == b), "Edge remainder lost Euclidean invariant");
                q += k;
                if (d) { r += step; if (r >= d) { r -= d; ++q; } }
            }
        }
    }
}

void selfTest() {
    checkIncrementalTriangleEdges();
    checkGraphicsAddressBoundaries();
    Machine highByte({0xf0, 0x00, 0x80, 0xc0, 0x0b, 0x03, 0x01, 0xd0, 0x01, 0, 1}, 0x8000);
    highByte.run();
    require(highByte.reg(0) == 128, "HIB must zero-extend and set sign from bit 7");
    Machine lowByte({0xf0, 0x80, 0x12, 0x9e, 0x0b, 0x03, 0x01, 0xd0, 0x01, 0, 1}, 0x8000);
    lowByte.run();
    require(lowByte.reg(0) == 128, "LOB must zero-extend and set sign from bit 7");
    Machine swap({0xf0, 0xcd, 0xab, 0x9e, 0x4d, 0, 1}, 0x8000);
    swap.run();
    require(swap.reg(0) == 0xcd00, "LOB/SWAP did not implement a modulo-16-bit shift by eight");
    // Independent encodings exercise the implicit latch and source selector.
    Machine sbk({0xf1, 0x00, 0x01, 0x41, 0xd0, 0x90, 0, 1}, 0x8000);
    sbk.seed(0x700100, 0xff, false); sbk.seed(0x700101, 0x12, false);
    sbk.run();
    require(sbk.word(0x700100) == 0x1300 && sbk.reg(1) == 0x100 &&
            sbk.metrics().store_backs == 1 && sbk.metrics().ram_loads == 1 &&
            sbk.metrics().stores == 1, "SBK did not store a word at the last LOAD address");
    Machine sbkByte({0xf1, 0x01, 0x01, 0x3d, 0x41, 0xf2, 0xcd, 0xab,
                     0xb2, 0x3d, 0x90, 0, 1}, 0x8000);
    sbkByte.run();
    require(sbkByte.byte(0x700101) == 0xcd && sbkByte.byte(0x700100) == 0xab &&
            sbkByte.reg(0) == 0, "SBK after LDB/ALT1 was incorrectly treated as a byte store");
    Machine sbkChanged({0xf1, 0x00, 0x01, 0x41, 0xf2, 0x04, 0x01, 0x42,
                        0xf3, 0x95, 0x12, 0x23, 0x90, 0, 1}, 0x8000);
    sbkChanged.run();
    require(sbkChanged.word(0x700100) == 0 && sbkChanged.word(0x700104) == 0x1295,
            "An intervening RAM load did not replace SBK's address");
    Machine sbkBank({0xf1, 0x00, 0x01, 0x41, 0xa0, 1, 0x3e, 0xdf,
                     0xf0, 0x95, 0x12, 0x90, 0, 1}, 0x8000);
    sbkBank.run();
    require(sbkBank.word(0x700100) == 0 && sbkBank.word(0x710100) == 0x1295,
            "SBK did not use the current RAM bank with the latched offset");
    // STB also replaces the latch; a following SBK writes both bytes. NZCV
    // from $8000-$0001 must survive SBK (BPL skips the failure increment).
    Machine sbkFlags({0xf1, 0x04, 0x01, 0xf0, 0, 0x80, 0x3d, 0x31,
                      0x3e, 0x61, 0x90, 0x0a, 2, 1, 0xd2, 0, 1}, 0x8000);
    sbkFlags.run();
    require(sbkFlags.word(0x700104) == 0x7fff && sbkFlags.reg(2) == 0,
            "SBK changed arithmetic flags or did not reuse the last STORE address");
    Machine copy({0xf0, 149, 0, 0x20, 0x11, 0, 1}, 0x8000);
    copy.run();
    require(copy.reg(1) == 149, "WITH R0 / TO R1 did not copy R0");
    Machine directSpill({0xf0, 149, 0, 0xf6, 53, 0, 0xf9, 0, 0x20,
                         0xa3, 0xfe, 0xb9, 0x13, 0x53, 0x33, 0, 1}, 0x8000);
    directSpill.run();
    require(directSpill.word(0x701ffe) == 149 && directSpill.reg(0) == 149 &&
            directSpill.reg(6) == 53,
            "Direct scalar spill clobbered R0 or parallel-copy scratch R6");
    Machine registerSpill({0xf0, 17, 0, 0xf5, 149, 0, 0xf6, 53, 0,
                           0xf9, 0, 0x20, 0xa3, 0xfc, 0xb9, 0x13, 0x53,
                           0x25, 0x33, 0, 1}, 0x8000);
    registerSpill.run();
    require(registerSpill.word(0x701ffc) == 149 && registerSpill.reg(0) == 17 &&
            registerSpill.reg(5) == 149 && registerSpill.reg(6) == 53,
            "Allocated-source PHI spill clobbered accumulator/source/cycle scratch");
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
    // Only the delayed opcode is buffered. Its immediate operands come from
    // the branch destination; $EE bytes beside the old opcode are NOT read.
    Machine splitIwt({5,3,0xf0,0xee,0xee,149,0,0,1},0x8000);
    splitIwt.run();
    require(splitIwt.reg(0) == 149 && splitIwt.metrics().instructions == 3,
            "Delayed IWT did not consume its two operands at the taken target");
    Machine splitIbt({5,3,0xa0,0xee,0xee,149,0,1},0x8000);
    splitIbt.run();
    require(splitIbt.reg(0) == 0xff95 && splitIbt.metrics().instructions == 3,
            "Delayed IBT did not consume/sign-extend the target's operand byte");
    Machine loop({0xfc, 3, 0, 0x2f, 0x1d, 0xd0, 0x3c, 0xd1, 0, 1}, 0x8000);
    loop.run();
    require(loop.reg(0) == 3 && loop.reg(1) == 3 && loop.reg(12) == 0 && loop.reg(13) == 0x8005,
            "PC-to-R13 / LOOP count or final/taken delay slot is incorrect");
    for (const auto count : {0u, 1u, 65535u}) {
        Machine boundaryLoop({0xfc, static_cast<std::uint8_t>(count), static_cast<std::uint8_t>(count >> 8),
                              0x2f, 0x1d, 0xd0, 0x3c, 0xd1, 0, 1}, 0x8000);
        boundaryLoop.run();
        // LOOP is do-while: zero wraps R12 and executes 65536 iterations.
        require(boundaryLoop.reg(0) == count && boundaryLoop.reg(1) == count && boundaryLoop.reg(12) == 0,
                "Hardware LOOP zero/one/65535 counter or delay-slot boundary changed");
    }
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
            bool report_metrics = false;
            for (int index = 3; index < argc;) {
                const std::string option(argv[index]);
                if (option == "--metrics") { report_metrics = true; ++index; continue; }
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
                    option != "--cbr" && option != "--cache-count" && option != "--sbk") throw std::runtime_error("unknown expectation");
                expectations.push_back({option, address, expected});
                index += 3;
            }
            if (expectations.empty()) throw std::runtime_error("at least one expectation is required");
            auto payload = readPayload(argv[1]);
            const auto payload_bytes = payload.size();
            Machine machine(std::move(payload), number(argv[2], 0xffffff), initial_sp, initial_ram_bank);
            if (report_metrics) machine.configureStackWindow(0x701000, 0x702000);
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
                    expectation.option == "--sbk" ? static_cast<unsigned>(machine.metrics().store_backs) :
                    expectation.option == "--register" ? machine.reg(expectation.address) : machine.graphicsState(expectation.option);
                if (actual != expectation.expected) {
                    throw std::runtime_error(expectation.option + " " + std::to_string(expectation.address) +
                        ": expected " + std::to_string(expectation.expected) + ", got " + std::to_string(actual));
                }
            }
            if (report_metrics) {
                const auto& m = machine.metrics();
                std::cout << "{\"payload_bytes\":" << payload_bytes << ",\"instructions\":" << m.instructions
                    << ",\"loads\":" << m.ram_loads << ",\"stores\":" << m.stores
                    << ",\"stack_loads\":" << m.stack_loads << ",\"stack_stores\":" << m.stack_stores
                    << ",\"branches\":" << m.branches << ",\"calls\":" << m.calls << "}\n";
                return 0;
            }
        }
        std::cout << "GSU execution checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
