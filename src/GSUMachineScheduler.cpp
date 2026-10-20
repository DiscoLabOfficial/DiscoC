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
    bool operand_fragment = false;
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
        // run() caps CODE at 65536 bytes before decode, including the endpoint.
        const auto begin = object.code_section.begin() + static_cast<std::ptrdiff_t>(p);
        i.bytes.assign(begin, begin + static_cast<std::ptrdiff_t>(length));
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
    return !i.operand_fragment && i.bytes.size() == 1 && i.bytes[0] == opcode;
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
    // COLOR/CMODE and plain PLOT preserve flags, but keep their state effects.
    // PLOT still includes R1's hardware increment.
    else if (op == 0x4e || (op == 0x4c && i.alt == 0)) writes = 0;
    else return false;
    // Branch preserves selectors and does not modify NZCV. Its decision may
    // move before an operation only if that operation leaves its inputs intact.
    return (writes & branchFlags(branch)) == 0;
}
// Only fault-free register operations, with neither PC/ROM-pointer effects nor
// memory/volatile accesses. Prefixes are moved with their complete copy pair.
struct SchedulingPoint { std::size_t index; unsigned forbidden_registers; };
std::size_t independent(const std::vector<Instruction>& code, SchedulingPoint point) {
    const auto at = point.index;
    const auto forbidden = point.forbidden_registers;
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
bool publicCodeSymbol(const ObjectFile& object, std::size_t offset) {
    for (const auto& symbol : object.symbol_table) {
        if (symbol.section != SymbolSection::CODE || symbol.offset != offset) continue;
        if (symbol.name == GSUCodeLayout::CacheAlignmentSymbol) continue;
        if (symbol.name.empty() || symbol.name.front() != '\x01') return true;
    }
    return false;
}
bool unconditionalExit(const Instruction& instruction) {
    if (instruction.bytes.empty()) return false;
    const auto op = instruction.bytes[0];
    // BRA and STOP never fall through. JMP and IWT R15 do so only as those
    // opcodes; an unknown or ALT-prefixed encoding is a different instruction.
    if ((op == 0 && instruction.bytes.size() == 1) || (op == 5 && instruction.bytes.size() == 2)) return true;
    if (instruction.alt != 0) return false;
    return (op >= 0x98 && op <= 0x9d && instruction.bytes.size() == 1) ||
        (op == 0xff && instruction.bytes.size() == 3);
}
bool splitImmediate(const Instruction& instruction) {
    if (instruction.delay_slot || instruction.bytes.size() < 2) return false;
    const auto op = instruction.bytes[0];
    return (op >= 0xa0 && op <= 0xae && instruction.bytes.size() == 2) ||
        (op >= 0xf0 && op <= 0xfe && instruction.bytes.size() == 3);
}
struct SchedulingBoundaries {
    const std::set<std::size_t>& entries;
    const std::set<std::size_t>& relocation_offsets;
    const std::set<std::size_t>& protected_offsets;
};
void planEntryIncrements(std::vector<Instruction>& code, const SchedulingBoundaries& boundaries,
                         const ObjectFile& object,
                         const std::map<std::size_t, std::size_t>& jump_targets,
                         GSUMachineScheduler::Statistics& stats) {
    std::map<std::size_t, unsigned> incoming;
    for (const auto& instruction : code) if (instruction.target != Missing) ++incoming[instruction.target];
    for (const auto& jump : jump_targets) ++incoming[jump.second];
    for (std::size_t n = 1; n + 1 < code.size(); ++n) {
        auto& previous = code[n - 1];
        auto& branch = code[n];
        auto& slot = code[n + 1];
        // The ordinary filler cannot cross an entry. A one-byte INC/DEC that
        // is itself the entry becomes the following BRA's slot, and the
        // zero-byte anchor keeps the label on the BRA.
        if (previous.bytes.empty() || previous.delay_slot || !increment(previous)) continue;
        if (branch.bytes.size() != 2 || branch.bytes[0] != 5 || !oneByte(slot, 1)) continue;
        if (!boundaries.entries.count(previous.old) || boundaries.entries.count(branch.old) || incoming[branch.old]) continue;
        if (publicCodeSymbol(object, previous.old) || publicCodeSymbol(object, branch.old)) continue;
        if (!freeRange(code, n - 1, n, boundaries.relocation_offsets) ||
            !freeRange(code, n + 1, n + 2, boundaries.protected_offsets)) continue;
        slot.bytes = previous.bytes;
        slot.delay_slot = true;
        previous.bytes.clear();
        previous.target = Missing;
        ++stats.delay_slots;
        ++stats.entry_slots;
    }
}
void planImmediateSlots(std::vector<Instruction>& code, const ObjectFile& object,
                        const std::map<std::size_t, std::size_t>& jump_targets,
                        const std::set<std::size_t>& relocation_offsets,
                        const std::set<std::size_t>& protected_offsets,
                        GSUMachineScheduler::Statistics& stats) {
    std::map<std::size_t, std::size_t> at;
    std::map<std::size_t, unsigned> incoming;
    for (std::size_t n = 0; n < code.size(); ++n) {
        const auto& instruction = code[n];
        if (instruction.old != Missing && !instruction.bytes.empty()) at[instruction.old] = n;
        if (instruction.target != Missing) ++incoming[instruction.target];
        else if (instruction.bytes.size() == 3 && instruction.bytes[0] == 0xff) {
            const auto jump = jump_targets.find(instruction.old);
            if (jump != jump_targets.end()) ++incoming[jump->second];
        }
    }
    for (std::size_t n = 0; n + 1 < code.size(); ++n) {
        const auto& branch = code[n];
        if (branch.delay_slot || !oneByte(code[n + 1], 1)) continue;
        if (n && !code[n - 1].bytes.empty()) {
            const auto link = code[n - 1].bytes[0];
            if (link >= 0x91 && link <= 0x94) continue;
        }
        std::size_t destination = Missing;
        if (branch.bytes.size() == 2 && branch.bytes[0] == 5 && branch.plain) destination = branch.target;
        else if (branch.bytes.size() == 3 && branch.bytes[0] == 0xff && branch.alt == 0) {
            const auto jump = jump_targets.find(branch.old);
            if (jump != jump_targets.end()) destination = jump->second;
        }
        if (destination == Missing || incoming[destination] != 1) continue;
        const auto found = at.find(destination);
        if (found == at.end() || !splitImmediate(code[found->second])) continue;
        const auto target = found->second;
        if (publicCodeSymbol(object, code[target].old)) continue;
        if (!freeRange(code, target, target + 1, relocation_offsets) ||
            !freeRange(code, n + 1, n + 2, protected_offsets)) continue;
        std::size_t predecessor = target;
        while (predecessor > 0 && code[predecessor - 1].bytes.empty()) --predecessor;
        const bool slot_precedes = predecessor > 0 && predecessor - 1 == n + 1;
        if (predecessor > 0 && !slot_precedes && !unconditionalExit(code[predecessor - 1])) continue;
        // The branch preserves clear selectors. Only its opcode byte executes
        // in the slot; the immediate stays at the unique target PC.
        code[n + 1].bytes = {code[target].bytes[0]};
        code[n + 1].delay_slot = true;
        code[target].bytes.erase(code[target].bytes.begin());
        // The remaining byte(s) are fetched by the opcode in the old-flow
        // slot. Their numeric value is not an executable CACHE/STOP/etc.
        code[target].operand_fragment = true;
        ++stats.immediate_slots;
    }
}
void dropNestedCaches(std::vector<Instruction>& code, const ObjectFile& object,
                      const std::set<std::size_t>& entries,
                      GSUMachineScheduler::Statistics& stats) {
    // Without a required final alignment, two different PCs can cross a
    // cache-line boundary after relinking. Entry-CACHE padding is applied
    // later, so it also prevents an exact position proof in this pass.
    if (GSUCodeLayout::alignment(object) != 16) return;
    for (const auto& instruction : code)
        if (oneByte(instruction, 2) && publicCodeSymbol(object, instruction.old)) return;
    bool have_window = false;
    bool selectors_clear = false;
    std::size_t anchor = 0, position = 0;
    for (auto& instruction : code) {
        // A linear predecessor's CACHE need not execute on another entry.
        // Calls may also replace CBR before returning. Restrict the proof to
        // a straight-line basic-block segment, including zero-byte anchors.
        if (entries.count(instruction.old)) { have_window = false; selectors_clear = false; }
        const auto size = instruction.bytes.size();
        if (instruction.bytes.empty() || instruction.operand_fragment) { position += size; continue; }
        const auto op = instruction.bytes[0];
        if (instruction.delay_slot) {
            // CACHE here observes the transfer's effective PC, not this
            // byte's linear position. Keep it and lose CBR knowledge unless
            // both paths' effective windows are independently proved.
            have_window = false;
            selectors_clear = false;
            position += size;
            continue;
        }
        if (transfer(op) || op == 0 || (op >= 0x91 && op <= 0x94)) {
            have_window = false;
            selectors_clear = false;
            position += size;
            continue;
        }
        if (!oneByte(instruction, 2)) {
            // Conservatively forget even a complete WITH/TO copy pair. An
            // ordinary consumer/literal resets all selectors; prefixes do not.
            selectors_clear = !((op >= 0x10 && op <= 0x2f) || (op >= 0xb0 && op <= 0xbf) ||
                (op >= 0x3d && op <= 0x3f));
            position += size;
            continue;
        }
        const auto next_pc = position + size;
        // Equality of CBR, not just membership in its 512-byte window, keeps
        // all later coverage unchanged. CACHE also resets ALT/WITH/FROM/TO.
        if (have_window && selectors_clear && (next_pc & ~std::size_t{15}) == (anchor & ~std::size_t{15})) {
            instruction.bytes.clear(); instruction.target = Missing;
            ++stats.nested_caches;
        } else {
            anchor = next_pc;
            have_window = true;
            position += size;
        }
        selectors_clear = true;
    }
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

    // A private one-PLOT loop may put PLOT in LOOP's always-executed slot,
    // including its first/final iteration and the raw zero-count wrap. Keep
    // the old PLOT entry anchor on the new LOOP; generic scheduling cannot
    // erase that entry or infer its incoming selector state from linear code.
    std::map<std::string, std::size_t> pixel_symbols;
    std::set<std::size_t> private_pixels, blocked_pixels, r13_literals;
    for (const auto& i : code) {
        if (i.bytes.size() == 3 && i.bytes[0] == 0xfd && i.alt == 0 && !i.delay_slot)
            r13_literals.insert(i.old);
        if (i.target != Missing) blocked_pixels.insert(i.target);
    }
    for (const auto& s : object.symbol_table) if (s.section == SymbolSection::CODE) {
        pixel_symbols.emplace(s.name, s.offset);
        if (!s.name.empty() && s.name.front() == '\x01') private_pixels.insert(s.offset);
        else blocked_pixels.insert(s.offset);
    }
    std::set<std::size_t> r13_pixels;
    for (const auto& r : object.relocation_table) {
        const auto target = pixel_symbols.find(r.target_symbol_name);
        if (target == pixel_symbols.end()) continue;
        const auto patch = static_cast<std::size_t>(r.patch_offset);
        if (r.section_to_patch == SymbolSection::CODE && r.type == RelocationType::ADDR16_IWT &&
            r13_literals.count(patch))
            r13_pixels.insert(target->second);
        else blocked_pixels.insert(target->second);
    }
    for (std::size_t n = 1; n + 2 < code.size(); ++n) {
        const auto entry = code[n].old;
        if (!oneByte(code[n], 0x4c) || !oneByte(code[n + 1], 0x3c) || !oneByte(code[n + 2], 1) ||
            code[n].delay_slot || !private_pixels.count(entry) || !r13_pixels.count(entry) || blocked_pixels.count(entry) ||
            !freeRange(code, n + 1, n + 3, entries) || !freeRange(code, n, n + 3, relocation_offsets)) continue;
        const auto& before = code[n - 1];
        // LOOP resets prefixes on every backedge. The only remaining entry
        // is fallthrough, which must also be a known reset, never LINK's or
        // another transfer's delay slot, WITH, FROM, TO or ALT.
        if (before.delay_slot || !(oneByte(before, 1) || oneByte(before, 2) ||
            (before.bytes.size() == 3 && before.bytes[0] == 0xfd && before.alt == 0))) continue;
        code[n].bytes = {0x3c}; code[n].plain = true; code[n].alt = code[n].source = code[n].destination = 0;
        code[n + 1].bytes = {0x4c}; code[n + 1].delay_slot = true;
        code[n + 1].plain = true; code[n + 1].alt = code[n + 1].source = code[n + 1].destination = 0;
        code.erase(code.begin() + static_cast<std::ptrdiff_t>(n + 2));
        ++stats.delay_slots; ++stats.private_pixel_loops;
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
        auto work = independent(code, {end, forbidden});
        std::size_t gap = 0;
        if (!work && end < code.size() && code[end].bytes[0] == 0x4c && code[end].plain) {
            // A PLOT depends on the new COLR, but pure following counter work
            // may move before both. Preserve R1/R2 reads AND R1 auto-increment.
            gap = 1; forbidden |= 6u; work = independent(code, {end + gap, forbidden});
        }
        if (!work || !freeRange(code, n, end + gap + work, protected_offsets)) continue;
        // A consumer resets selectors; hence the following operation's original
        // plain state is also valid before the consumer's complete prefix group.
        // Every decoded instruction occupies at least one byte in the bounded
        // CODE image, so all these instruction indices fit ptrdiff_t.
        std::rotate(code.begin() + static_cast<std::ptrdiff_t>(n),
                    code.begin() + static_cast<std::ptrdiff_t>(end + gap),
                    code.begin() + static_cast<std::ptrdiff_t>(end + gap + work));
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
            code[n + 1] = candidate; code[n + 1].delay_slot = true;
            code.erase(code.begin() + static_cast<std::ptrdiff_t>(n - 1));
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
            code[n + 1] = candidate; code[n + 1].delay_slot = true;
            code.erase(code.begin() + static_cast<std::ptrdiff_t>(n - 1));
            ++stats.delay_slots; if (n > 1) --n; continue;
        }
        if (!candidate.plain || (!increment(candidate) && candidate.bytes[0] != 0x4c)) continue;
        std::size_t slot = n + 1;
        bool legal = false;
        // BRA and plain IWT R15 do not inspect NZCV.
        if (op == 5 || (op == 0xff && code[n].alt == 0)) legal = increment(candidate);
        else if (op >= 6 && op <= 15) {
            // INC/DEC write N/Z. Only C/V branches may move them after testing.
            legal = increment(candidate) && op >= 12;
        } else if (op >= 0x98 && op <= 0x9d && code[n].alt == 0)
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
        code.erase(code.begin() + static_cast<std::ptrdiff_t>(n - 1));
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
    // Relax symbol-resolved, non-call PC literals after useful-slot/layout
    // compaction. Never infer a destination from an unrelocated numeric word.
    // Plain selectors make BRA's preserved prefix state identical to IWT's
    // reset state, including for a useful opcode in the existing delay slot.
    std::map<std::size_t, std::size_t> jump_targets;
    for (const auto& r : object.relocation_table) {
        const auto target = symbols.find(r.target_symbol_name);
        if (r.section_to_patch == SymbolSection::CODE && r.type == RelocationType::ADDR16_IWT && target != symbols.end())
            jump_targets.emplace(r.patch_offset, target->second);
    }
    std::set<std::size_t> shortened;
    for (unsigned round = 0; round < 8 && !jump_targets.empty(); ++round) {
        std::vector<std::size_t> positions(object.code_section.size() + 1, Missing);
        std::size_t length = 0;
        for (const auto& i : code) {
            if (i.old != Missing) positions.at(i.old) = length;
            length += i.bytes.size();
        }
        positions.back() = length;
        bool changed = false;
        for (std::size_t n = 0; n < code.size(); ++n) {
            auto& i = code[n];
            const auto target = jump_targets.find(i.old);
            if (target == jump_targets.end() || i.bytes.size() != 3 || i.bytes[0] != 0xff ||
                !i.plain || i.delay_slot || (n && code[n - 1].bytes.size() == 1 &&
                code[n - 1].bytes[0] >= 0x91 && code[n - 1].bytes[0] <= 0x94)) continue;
            if (entries.count(i.old + 1) || entries.count(i.old + 2) ||
                relocation_offsets.count(i.old + 3)) continue;
            const auto here = positions.at(i.old), destination = positions.at(target->second);
            if (here == Missing || destination == Missing) continue;
            // This round's distances can only decrease as other jumps shrink.
            const auto distance = static_cast<std::int64_t>(destination) -
                static_cast<std::int64_t>(here + 2) - (destination > here ? 1 : 0);
            if (distance < -128 || distance > 127) continue;
            i.bytes = {5, 0}; i.target = target->second;
            shortened.insert(i.old); changed = true; ++stats.short_jumps;
        }
        if (!changed) break;
    }
    // Slot planning stays on this instruction list. Re-decoding afterwards
    // would treat a split immediate opcode as a full instruction.
    const auto without_plans = code;
    const auto without_plan_stats = stats;
    const SchedulingBoundaries boundaries{entries, relocation_offsets, protected_offsets};
    planEntryIncrements(code, boundaries, object, jump_targets, stats);
    planImmediateSlots(code, object, jump_targets, relocation_offsets, protected_offsets, stats);
    if (object.config.optimization != OptimizationLevel::Size)
        dropNestedCaches(code, object, entries, stats);

    const auto publish = [&](const std::vector<Instruction>& seq, bool hint, unsigned padding) {
        auto candidate = object;
        candidate.relocation_table.erase(std::remove_if(candidate.relocation_table.begin(), candidate.relocation_table.end(),
            [&](const RelocationEntry& r) { return r.section_to_patch == SymbolSection::CODE && shortened.count(r.patch_offset); }),
            candidate.relocation_table.end());
        if (!assemble(seq, candidate)) return false;
        if (hint && GSUCodeLayout::alignment(candidate) != 16)
            candidate.symbol_table.push_back({GSUCodeLayout::CacheAlignmentSymbol, SymbolSection::CODE, 0});
        object = std::move(candidate);
        if (hint) stats.cache_padding = padding;
        return true;
    };
    std::set<std::size_t> cache_entries;
    // Size policy keeps explicit CACHE opcodes but adds no alignment bytes or
    // linker padding hints. Align a function-entry CACHE's prefetched next
    // byte, not its opcode. Do not pad loop CACHE directives.
    if (object.config.optimization != OptimizationLevel::Size) {
        for (const auto& symbol : object.symbol_table)
            if (symbol.section == SymbolSection::CODE && !symbol.name.empty() && symbol.name.front() != '\x01')
                cache_entries.insert(symbol.offset);
    }
    const auto layout = [&](const std::vector<Instruction>& seq) {
        unsigned padding = 0;
        std::vector<Instruction> aligned;
        std::size_t bytes = 0;
        bool can_hint = false;
        if (!cache_entries.empty()) {
            for (const auto& instruction : seq) {
                if (oneByte(instruction, 2) && cache_entries.count(instruction.old)) {
                    can_hint = true;
                    while ((bytes + 1) % 16) {
                        Instruction nop; nop.bytes = {1}; aligned.push_back(std::move(nop)); ++bytes; ++padding;
                    }
                }
                bytes += instruction.bytes.size();
                aligned.push_back(instruction);
            }
            if (bytes > 65536) can_hint = false;
        }
        if (can_hint && publish(aligned, true, padding)) return true;
        return publish(seq, false, 0);
    };
    if (!layout(code)) {
        stats = without_plan_stats;
        if (!layout(without_plans)) return Statistics{};
    }
    return stats;
}
