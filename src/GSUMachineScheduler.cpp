#include "GSUMachineScheduler.hpp"
#include "GSUCodeLayout.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace {
constexpr std::size_t Missing = std::numeric_limits<std::size_t>::max();
struct Instruction {
    std::size_t old = Missing;
    std::vector<std::uint8_t> bytes;
    bool plain = false;
    bool delay_slot = false;
    unsigned alt = 4;
    unsigned source = 16, destination = 16;
    std::size_t target = Missing;
};
std::vector<Instruction> decode(const ObjectFile& object, std::set<std::size_t>& entries) {
    std::vector<Instruction> result;
    unsigned alt = 4, source = 16, destination = 16;
    bool with = false;
    // First decode boundaries/targets. Entry labels invalidate prefix knowledge
    // even when their incoming state happens to match the linear predecessor.
    for (std::size_t p = 0; p < object.code_section.size();) {
        const auto op = object.code_section[p];
        const std::size_t length = op >= 0xf0 ? 3 : (op >= 0xa0 && op <= 0xaf) || (op >= 5 && op <= 15) ? 2 : 1;
        if (length > object.code_section.size() - p) throw std::runtime_error("GSU scheduler: truncated compiler instruction.");
        Instruction i; i.old = p;
        i.bytes.assign(object.code_section.begin() + p, object.code_section.begin() + p + length);
        if (op >= 5 && op <= 15) {
            const auto distance = i.bytes[1] < 128 ? static_cast<int>(i.bytes[1]) : static_cast<int>(i.bytes[1]) - 256;
            const auto target = static_cast<std::int64_t>(p) + 2 + distance;
            if (target < 0 || static_cast<std::size_t>(target) > object.code_section.size())
                throw std::runtime_error("GSU scheduler: compiler branch outside CODE.");
            i.target = static_cast<std::size_t>(target); entries.insert(i.target);
        }
        result.push_back(std::move(i)); p += length;
    }
    for (auto& i : result) {
        if (entries.count(i.old)) { alt = 4; source = destination = 16; with = false; }
        i.plain = alt == 0 && source == 0 && destination == 0 && !with;
        i.alt = alt;
        i.source = source; i.destination = destination;
        const auto op = i.bytes[0];
        if (op >= 0x20 && op <= 0x2f) { source = destination = op & 15; with = true; }
        else if (op >= 0x10 && op <= 0x1f) {
            if (with) { alt = source = destination = 0; with = false; }
            else destination = op & 15;
        } else if (op >= 0xb0 && op <= 0xbf) {
            if (with) { alt = source = destination = 0; with = false; }
            else source = op & 15;
        } else if (op >= 0x3d && op <= 0x3f) {
            if (op == 0x3f) alt = 3;
            else if (alt != 4) alt |= op - 0x3c;
            with = false;
        }
        else if (!(op >= 5 && op <= 15)) { alt = source = destination = 0; with = false; }
    }
    return result;
}
bool transfer(std::uint8_t op) {
    return (op >= 5 && op <= 15) || op == 0xff || (op >= 0x98 && op <= 0x9d) || op == 0x3c;
}
bool freeRange(const std::vector<Instruction>& code, std::size_t begin, std::size_t end,
               const std::set<std::size_t>& protected_offsets) {
    for (auto n = begin; n < end; ++n) {
        const auto& i = code[n];
        const auto found = protected_offsets.lower_bound(i.old);
        if (found != protected_offsets.end() && *found < i.old + i.bytes.size()) return false;
    }
    return true;
}
bool increment(const Instruction& i) {
    const auto op = i.bytes[0];
    return i.bytes.size() == 1 && ((op >= 0xd0 && op <= 0xdd) || (op >= 0xe0 && op <= 0xed));
}
bool oneByte(const Instruction& i, std::uint8_t opcode) {
    return i.bytes.size() == 1 && i.bytes[0] == opcode;
}
enum ConditionFlag : unsigned { Sign = 1, Zero = 2, Carry = 4, Overflow = 8 };
unsigned branchFlags(std::uint8_t opcode) {
    if (opcode == 5) return 0;
    if (opcode <= 7) return Sign | Overflow;
    if (opcode <= 9) return Zero;
    if (opcode <= 11) return Sign;
    if (opcode <= 13) return Carry;
    return Overflow;
}
bool registerSlot(const Instruction& i, std::uint8_t branch) {
    if (i.bytes.size() != 1 || i.alt > 3 || i.source >= 14 || i.destination >= 14) return false;
    const auto op = i.bytes[0];
    unsigned writes;
    if ((op >= 0x50 && op <= 0x6f) || (op >= 0x71 && op <= 0x8f) || (op >= 0xc1 && op <= 0xcf)) {
        // SUB #imm is ALT2 only: ALT3 selects register CMP, unlike ADD/AND.
        const bool immediate = (i.alt & 2) && !(op >= 0x60 && op <= 0x6f && i.alt == 3);
        if (!immediate && (op & 15) >= 14) return false;
        writes = op <= 0x6f ? Sign | Zero | Carry | Overflow : Sign | Zero;
    } else if (op == 3 || op == 4 || op == 0x96 || op == 0x97) writes = Sign | Zero | Carry;
    else if (op == 0x4d || op == 0x4f || op == 0x95 || op == 0x9e || op == 0xc0) writes = Sign | Zero;
    else if (op == 0x4e) writes = 0; // COLOR/CMODE; executed on both paths, not speculated.
    else if (op == 0x4c && i.alt == 0) writes = 0; // PLOT includes R1's hardware increment.
    else return false;
    // Branch preserves selectors and does not modify NZCV. Its decision may
    // move before an operation only if that operation leaves its inputs intact.
    return (writes & branchFlags(branch)) == 0;
}
// Only fault-free register operations, with neither PC/ROM-pointer effects nor
// memory/volatile accesses. Prefixes are moved with their complete copy pair.
std::size_t independent(const std::vector<Instruction>& code, std::size_t at, unsigned forbidden) {
    if (at >= code.size() || !code[at].plain || code[at].delay_slot) return 0;
    const auto op = code[at].bytes[0];
    const unsigned reg = op & 15u;
    const auto blocked = [&](unsigned r) { return (forbidden & (1u << r)) != 0; };
    if (increment(code[at])) return blocked(reg) ? 0 : 1;
    if ((op >= 0xa0 && op <= 0xad) || (op >= 0xf0 && op <= 0xfd)) return blocked(reg) ? 0 : 1;
    if (op >= 0x20 && op <= 0x2d && at + 1 < code.size()) {
        const auto to = code[at + 1].bytes[0];
        if (to >= 0x10 && to <= 0x1d && !blocked(reg) && !blocked(to & 15u)) return 2;
        if (to == 0x3e && at + 2 < code.size() && !blocked(reg)) {
            const auto alu = code[at + 2].bytes[0];
            if ((alu >= 0x50 && alu <= 0x6f) || (alu >= 0x71 && alu <= 0x7f) || (alu >= 0xc1 && alu <= 0xcf)) return 3;
        }
    }
    if (blocked(0)) return 0;
    if (op == 3 || op == 4 || op == 0x4d || op == 0x4f || op == 0x95 || op == 0x96 ||
        op == 0x97 || op == 0x9e || op == 0xc0 || (op >= 0x50 && op <= 0x5d) ||
        (op >= 0x60 && op <= 0x6d) || (op >= 0x71 && op <= 0x7d)) {
        // Register ALU operands must not be the pending read's destination.
        if (op >= 0x50 && op <= 0x7d && blocked(reg)) return 0;
        return 1;
    }
    return 0;
}
bool assemble(const std::vector<Instruction>& code, ObjectFile& object) {
    std::vector<std::size_t> offsets(object.code_section.size() + 1, Missing);
    std::vector<std::uint8_t> bytes;
    for (const auto& i : code) {
        if (i.old != Missing) {
            // A removed target prefix leaves a zero-byte anchor: incoming
            // labels now name its successor, not the speculative slot.
            offsets.at(i.old) = bytes.size();
            for (std::size_t b = 0; b < i.bytes.size(); ++b) offsets.at(i.old + b) = bytes.size() + b;
        }
        bytes.insert(bytes.end(), i.bytes.begin(), i.bytes.end());
    }
    offsets.back() = bytes.size();
    for (const auto& i : code) if (i.target != Missing) {
        const auto here = offsets.at(i.old), target = offsets.at(i.target);
        if (here == Missing || target == Missing) throw std::runtime_error("GSU scheduler: lost branch boundary.");
        const auto distance = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(here + 2);
        if (distance < -128 || distance > 127) return false; // Padding must never undo branch relaxation.
        bytes.at(here + 1) = static_cast<std::uint8_t>(distance & 255);
    }
    for (auto& s : object.symbol_table) if (s.section == SymbolSection::CODE) {
        if (s.name == GSUCodeLayout::CacheAlignmentSymbol) continue;
        const auto offset = offsets.at(s.offset);
        if (offset == Missing) throw std::runtime_error("GSU scheduler: lost symbol boundary.");
        s.offset = static_cast<std::uint32_t>(offset);
    }
    for (auto& r : object.relocation_table) if (r.section_to_patch == SymbolSection::CODE) {
        const auto offset = offsets.at(r.patch_offset);
        if (offset == Missing) throw std::runtime_error("GSU scheduler: lost relocation boundary.");
        r.patch_offset = static_cast<std::uint32_t>(offset);
    }
    object.code_section = std::move(bytes); return true;
}
}

GSUMachineScheduler::Statistics GSUMachineScheduler::run(ObjectFile& object) {
    Statistics stats;
    // Layout/scheduling uses bounded per-byte maps. Bigger compiler objects are
    // left for the linker's existing one-program-bank diagnostic.
    if (object.config.target != TargetKind::GSU || object.code_section.size() > 65536) return stats;
    std::set<std::size_t> entries, protected_offsets, relocation_offsets;
    for (const auto& s : object.symbol_table) if (s.section == SymbolSection::CODE) entries.insert(s.offset);
    auto code = decode(object, entries);
    for (std::size_t n = 1; n < code.size(); ++n)
        code[n].delay_slot = transfer(code[n - 1].bytes[0]);
    protected_offsets = entries;
    for (const auto& r : object.relocation_table) if (r.section_to_patch == SymbolSection::CODE)
        for (unsigned b = 0; b < (r.type == RelocationType::ADDR24_BANK ? 2u : 3u); ++b) {
            protected_offsets.insert(r.patch_offset + b);
            relocation_offsets.insert(r.patch_offset + b);
        }

    // Hide ROM-buffer latency with work already following the read. GETC has no
    // register/condition-code result; plain GETB writes only its selected DREG.
    // Never cross a label, relocation, bank switch, load/store, trap or branch.
    for (std::size_t n = 0; n < code.size(); ++n) {
        const auto op = code[n].bytes[0];
        if (code[n].delay_slot) continue;
        std::size_t read = n, end = n + 1;
        unsigned destination = 16;
        if (op == 0xdf && code[n].plain) { /* GETC */ }
        else if (op >= 0x10 && op <= 0x1d && code[n].plain && n + 1 < code.size() &&
                 code[n + 1].bytes[0] == 0xef && code[n + 1].alt == 0) {
            read = n + 1; end = n + 2; destination = op & 15;
        } else continue;
        unsigned forbidden = destination == 16 ? 0 : 1u << destination;
        auto work = independent(code, end, forbidden);
        std::size_t gap = 0;
        if (!work && end < code.size() && code[end].bytes[0] == 0x4c && code[end].plain) {
            // A PLOT depends on the new COLR, but pure following counter work
            // may move before both. Preserve R1/R2 reads AND R1 auto-increment.
            gap = 1; forbidden |= 6u; work = independent(code, end + gap, forbidden);
        }
        if (!work || !freeRange(code, n, end + gap + work, protected_offsets)) continue;
        // A consumer resets selectors; hence the following operation's original
        // plain state is also valid before the consumer's complete prefix group.
        std::rotate(code.begin() + n, code.begin() + end + gap, code.begin() + end + gap + work);
        ++stats.rom_operations;
        n = read + work + gap;
    }

    for (std::size_t n = 1; n + 1 < code.size(); ++n) {
        const auto candidate = code[n - 1];
        // A useful slot is still a slot, not a free predecessor instruction.
        // Moving it again would put a multi-byte transfer in the earlier slot.
        if (candidate.delay_slot) continue;
        const auto op = code[n].bytes[0];
        if (op >= 5 && op <= 15 && registerSlot(candidate, op) && oneByte(code[n + 1], 1) &&
            freeRange(code, n - 1, n + 2, entries) && freeRange(code, n - 1, n, protected_offsets) &&
            freeRange(code, n + 1, n + 2, protected_offsets)) {
            // Leave complete source/destination/ALT prefixes in place. Only
            // the final physical opcode executes in the old-flow delay slot.
            code[n + 1] = candidate; code[n + 1].delay_slot = true; code.erase(code.begin() + n - 1);
            ++stats.delay_slots; if (n > 1) --n; continue;
        }
        const auto copy_opcode = candidate.bytes[0];
        const bool copy_to = copy_opcode >= 0x10 && copy_opcode <= 0x1d;
        const bool copy_from = copy_opcode >= 0xb0 && copy_opcode <= 0xbd && (op == 5 || op == 12 || op == 13);
        if (n >= 2 && op >= 5 && op <= 15 && oneByte(code[n + 1], 1) &&
            code[n - 2].bytes[0] >= 0x20 && code[n - 2].bytes[0] <= 0x2d &&
            (copy_to || copy_from) &&
            freeRange(code, n - 2, n + 2, entries) && freeRange(code, n - 2, n, protected_offsets) &&
            freeRange(code, n + 1, n + 2, protected_offsets)) {
            // BRA/Bcc preserve WITH's copy prefix. TO changes no NZCV; FROM's
            // MOVES writes N/Z/V, so only BRA and carry branches may precede it.
            // The one-byte copy clears selectors before either successor.
            // IWT/JMP/LINK/LOOP clear that prefix, so this rule excludes them.
            code[n + 1] = candidate; code[n + 1].delay_slot = true; code.erase(code.begin() + n - 1);
            ++stats.delay_slots; if (n > 1) --n; continue;
        }
        if (!candidate.plain || (!increment(candidate) && candidate.bytes[0] != 0x4c)) continue;
        std::size_t slot = n + 1;
        bool legal = false;
        if (op == 5) legal = increment(candidate); // BRA: does not inspect NZCV.
        else if (op >= 6 && op <= 15) {
            // INC/DEC write N/Z. Only C/V branches may move them after testing.
            legal = increment(candidate) && op >= 12;
        } else if (op == 0xff && code[n].alt == 0) legal = increment(candidate);
        else if (op >= 0x98 && op <= 0x9d && code[n].alt == 0)
            legal = increment(candidate) && (candidate.bytes[0] & 15) != (op & 15);
        else if (op == 0x94 && n + 2 < code.size() && code[n + 1].bytes[0] == 0xff && code[n + 1].alt == 0) {
            slot = n + 2; legal = increment(candidate) && (candidate.bytes[0] & 15) != 11;
        } else if (op == 0x3c) legal = candidate.bytes[0] == 0x4c; // PLOT keeps LOOP's N/Z and R12/R13 intact.
        if (!legal || slot >= code.size() || code[slot].bytes[0] != 1 ||
            !freeRange(code, n - 1, slot + 1, entries) || !freeRange(code, slot, slot + 1, protected_offsets) ||
            !freeRange(code, n - 1, n, protected_offsets)) continue;
        // Only ONE physical opcode byte in the slot. IWT/IBT operands would be
        // fetched at the taken destination, not beside the old-flow opcode.
        code[slot] = candidate;
        code[slot].delay_slot = true;
        code.erase(code.begin() + n - 1);
        ++stats.delay_slots;
        if (n > 1) --n;
    }
    // A taken-edge selector may occupy Bcc's slot across the canonical fault
    // path: Bcc / NOP / literal error code / STOP / NOP / selector / continuation.
    // WITH/TO/FROM have no register/flag/memory effect in plain state, but change selectors on BOTH
    // paths. The untaken literal must ignore/reset them before STOP. A unique
    // private target has no other incoming path that still needs the prefix.
    std::map<std::size_t, unsigned> incoming;
    std::set<std::size_t> external_targets;
    std::map<std::string, std::size_t> symbols;
    for (const auto& i : code) if (i.target != Missing) ++incoming[i.target];
    for (const auto& s : object.symbol_table) if (s.section == SymbolSection::CODE) {
        symbols.emplace(s.name, s.offset);
        if (!s.name.empty() && s.name.front() != '\x01') external_targets.insert(s.offset);
    }
    for (const auto& r : object.relocation_table) {
        const auto target = symbols.find(r.target_symbol_name);
        if (target != symbols.end()) external_targets.insert(target->second);
    }
    for (std::size_t n = 0; n + 6 < code.size(); ++n) {
        if (code[n].bytes.empty()) continue;
        const auto op = code[n].bytes[0];
        if (op < 6 || op > 15 || !code[n].plain || code[n].delay_slot ||
            !oneByte(code[n + 1], 1)) continue;
        const auto& reset = code[n + 2];
        if (reset.bytes.empty()) continue;
        const auto literal = reset.bytes[0];
        if (!((literal >= 0xa0 && literal <= 0xad && reset.bytes.size() == 2) ||
              (literal >= 0xf0 && literal <= 0xfd && reset.bytes.size() == 3)) ||
            !oneByte(code[n + 3], 0) || !oneByte(code[n + 4], 1)) continue;
        auto& prefix = code[n + 5];
        if (prefix.bytes.size() != 1) continue;
        const auto selector = prefix.bytes[0];
        if (!((selector >= 0x10 && selector <= 0x1d) || (selector >= 0x20 && selector <= 0x2d) ||
              (selector >= 0xb0 && selector <= 0xbd)) ||
            code[n].target != prefix.old || incoming[prefix.old] != 1 || external_targets.count(prefix.old) ||
            !freeRange(code, n + 1, n + 2, protected_offsets) || relocation_offsets.count(prefix.old)) continue;
        code[n + 1].bytes = prefix.bytes;
        code[n + 1].delay_slot = true;
        prefix.bytes.clear(); // Preserve its old label/branch boundary as an anchor.
        ++stats.delay_slots;
    }
    // Scheduling is size non-increasing. If a boundary somehow loses range,
    // preserve the original object rather than producing unchecked code.
    // Bcc exit / NOP / BRA body / NOP / exit becomes inverse-Bcc body /
    // NOP / exit. No useful slot or independently reachable entry is removed;
    // private, unreferenced labels retain zero-byte anchors for serialization.
    for (std::size_t n = 0; n + 4 < code.size(); ++n) {
        if (code[n].bytes.empty()) continue;
        const auto op = code[n].bytes[0];
        if (op < 6 || op > 15 || code[n].delay_slot || !oneByte(code[n + 1], 1) ||
            code[n + 2].bytes.size() != 2 || code[n + 2].bytes[0] != 5 ||
            !oneByte(code[n + 3], 1) || code[n].target != code[n + 4].old ||
            code[n + 2].target == Missing) continue;
        bool protected_entry = false;
        for (std::size_t p = n + 1; p <= n + 3; ++p)
            protected_entry = protected_entry || incoming[code[p].old] || external_targets.count(code[p].old);
        if (protected_entry || !freeRange(code, n, n + 4, relocation_offsets)) continue;
        code[n].bytes[0] = static_cast<std::uint8_t>(op ^ 1u);
        code[n].target = code[n + 2].target;
        code[n + 2].bytes.clear(); code[n + 2].target = Missing;
        code[n + 3].bytes.clear();
        ++stats.compact_branches;
    }
    auto scheduled = object;
    if (!assemble(code, scheduled)) return Statistics{};
    object = std::move(scheduled);

    // Size policy keeps explicit CACHE opcodes but adds no alignment bytes or
    // linker padding hints. Delay-slot/ROM scheduling above is non-increasing.
    if (object.config.optimization == OptimizationLevel::Size) return stats;

    // Align function-entry CACHE's *prefetched next byte*, not its opcode.
    // Padding is outside the entry symbol, hence not paid on calls. Deliberately
    // do not pad loop CACHE directives: measured rotation timing regressed when
    // those NOPs executed on each scanline. Hot loop layout is handled in IR.
    entries.clear();
    for (const auto& s : object.symbol_table) if (s.section == SymbolSection::CODE) entries.insert(s.offset);
    code = decode(object, entries);
    std::set<std::size_t> cache_entries;
    for (const auto& s : object.symbol_table)
        if (s.section == SymbolSection::CODE && !s.name.empty() && s.name.front() != '\x01' &&
            s.offset < object.code_section.size() && object.code_section[s.offset] == 2) cache_entries.insert(s.offset);
    std::vector<Instruction> aligned;
    std::size_t bytes = 0;
    unsigned padding = 0;
    for (auto i : code) {
        if (cache_entries.count(i.old)) while ((bytes + 1) % 16) {
            Instruction nop; nop.bytes = {1}; aligned.push_back(std::move(nop)); ++bytes; ++padding;
        }
        bytes += i.bytes.size(); aligned.push_back(std::move(i));
    }
    auto candidate = object;
    if (!cache_entries.empty() && bytes <= 65536 && assemble(aligned, candidate)) {
        if (GSUCodeLayout::alignment(candidate) != 16)
            candidate.symbol_table.push_back({GSUCodeLayout::CacheAlignmentSymbol, SymbolSection::CODE, 0});
        object = std::move(candidate); stats.cache_padding = padding;
    }
    return stats;
}
