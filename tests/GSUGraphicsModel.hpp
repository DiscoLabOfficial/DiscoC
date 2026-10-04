#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Bounded instruction-level graphics oracle, not a cycle-accurate emulator.
// `ram` is borrowed from the owning Machine and outlives this model.
// Independent hardware reference: ares/sfc/coprocessor/superfx/core.cpp,
// color(), plot(), rpix() and flushPixelCache(). Opcode cursor behavior is
// checked separately against ares/component/processor/gsu/instructions.cpp.
class GSUGraphicsModel {
public:
    explicit GSUGraphicsModel(std::vector<std::uint8_t>& ram) : ram_(ram) {}
    void configure(std::uint8_t mode, std::uint8_t base) { mode_ = mode; base_ = base; }
    void options(std::uint8_t value) { ++counts_["--cmode"]; por_ = value & 31; }
    void color(std::uint8_t value, bool rom = false) {
        ++counts_[rom ? "--getc" : "--color"];
        if (por_ & 4) colr_ = static_cast<std::uint8_t>((colr_ & 0xf0) | (value >> 4));
        else if (por_ & 8) colr_ = static_cast<std::uint8_t>((colr_ & 0xf0) | (value & 15));
        else colr_ = value;
    }
    unsigned state(const std::string& option) const {
        if (option == "--colr") return colr_;
        if (option == "--por") return por_;
        const auto found = counts_.find(option);
        return found == counts_.end() ? 0 : found->second;
    }
    void plot(std::uint8_t x, std::uint8_t y) {
        ++counts_["--plots"];
        auto value = colr_;
        // POR bit zero enables plotting zero; clear means transparent zero.
        if (!(por_ & 1) && ((depth() == 8 && !(por_ & 8)) ? value == 0 : (value & 15) == 0)) return;
        if ((por_ & 2) && depth() != 8) value = static_cast<std::uint8_t>(((x ^ y) & 1) ? value >> 4 : value & 15);
        const unsigned group = y * 32u + x / 8u;
        if (cache_[0].group != group) {
            flush(cache_[1]); cache_[1] = cache_[0];
            cache_[0] = Cache{}; cache_[0].group = group;
        }
        cache_[0].pixels[x % 8] = value;
        cache_[0].pending |= static_cast<std::uint8_t>(1u << (x % 8));
        if (cache_[0].pending == 255) {
            flush(cache_[1]); cache_[1] = cache_[0]; cache_[0].pending = 0;
        }
    }
    std::uint8_t read(std::uint8_t x, std::uint8_t y) {
        ++counts_["--rpix"]; flush(cache_[1]); flush(cache_[0]);
        const auto start = address(x, y);
        unsigned value = 0;
        for (unsigned plane = 0; plane < depth(); ++plane)
            value |= ((ram_.at(start + (plane / 2) * 16 + plane % 2) >> (7 - x % 8)) & 1u) << plane;
        return static_cast<std::uint8_t>(value);
    }
private:
    struct Cache { unsigned group = 65536; std::uint8_t pending = 0; std::array<std::uint8_t, 8> pixels{}; };
    unsigned depth() const { return (mode_ & 3) == 3 ? 8 : (mode_ & 3) == 0 ? 2 : 4; }
    std::size_t address(unsigned x, unsigned y) const {
        unsigned tile = 0;
        if ((por_ & 16) || (mode_ & 0x24) == 0x24)
            tile = (y / 128) * 512 + (x / 128) * 256 + ((y % 128) / 8) * 16 + (x % 128) / 8;
        else {
            const unsigned height = mode_ & 0x20 ? 192 : mode_ & 4 ? 160 : 128;
            tile = (x / 8) * (height / 8) + y / 8;
        }
        return static_cast<std::size_t>(base_) * 1024 + tile * depth() * 8 + (y & 7) * 2;
    }
    void flush(Cache& cache) {
        if (!cache.pending) return;
        const auto start = address((cache.group % 32) * 8, cache.group / 32);
        for (unsigned plane = 0; plane < depth(); ++plane) {
            const auto offset = start + (plane / 2) * 16 + plane % 2;
            auto bits = ram_.at(offset);
            for (unsigned pixel = 0; pixel < 8; ++pixel) if (cache.pending & (1u << pixel)) {
                const auto bit = static_cast<std::uint8_t>(1u << (7 - pixel));
                bits = static_cast<std::uint8_t>((bits & ~bit) | ((cache.pixels[pixel] & (1u << plane)) ? bit : 0));
            }
            ram_.at(offset) = bits;
        }
        cache.pending = 0;
    }
    std::vector<std::uint8_t>& ram_;
    std::array<Cache, 2> cache_{};
    std::map<std::string, unsigned> counts_;
    std::uint8_t colr_ = 0, por_ = 0, mode_ = 1, base_ = 24;
};
