#include "GSUMachineScheduler.hpp"
#include "GSUCodeLayout.hpp"
#include "Opcodes.hpp"

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
    // `plain` when only real control-flow entries (public symbols, relocation
    // targets and branch targets) reset selector knowledge. A private symbol
    // nothing refers to is a layout anchor, not an entry.
    bool reachable_plain = false;
    bool reachable_entry = false; // Starts at such a real control-flow entry.
    // Selector state before this instruction under that same entry notion.
    unsigned reachable_alt = 4, reachable_source = 16, reachable_destination = 16;
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
    // Non-owning: run() keeps these offset sets alive through every helper call.
    const std::set<std::size_t>& entries;
    const std::set<std::size_t>& relocation_offsets;
    const std::set<std::size_t>& protected_offsets;
    const std::set<std::size_t>& symbol_offsets;
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

std::vector<std::size_t> positions(const std::vector<Instruction>& code) {
    std::vector<std::size_t> result(code.size() + 1, 0);
    for (std::size_t n = 0; n < code.size(); ++n) result[n + 1] = result[n] + code[n].bytes.size();
    return result;
}
// Every current branch target, symbol and relocated jump destination.
std::set<std::size_t> liveTargets(const std::vector<Instruction>& code, const std::set<std::size_t>& symbol_offsets) {
    std::set<std::size_t> result = symbol_offsets;
    for (const auto& i : code) if (i.target != Missing) result.insert(i.target);
    return result;
}
bool selector(std::uint8_t op) {
    return (op >= 0x10 && op <= 0x1d) || (op >= 0x20 && op <= 0x2d) || (op >= 0xb0 && op <= 0xbd);
}
// One-byte opcodes that, executed with clear selectors, touch only R0-R13
// and NZCV: no memory, ROM buffer, PC, PLOT state or ALT prefix. They may run
// before a fault STOP, whose contract covers R6 and memory, not scratch R0.
bool registerOnly(const Instruction& i) {
    if (i.bytes.size() != 1 || i.operand_fragment) return false;
    const auto op = i.bytes[0];
    return selector(op) || (op >= 0xd0 && op <= 0xdd) || (op >= 0xe0 && op <= 0xed) ||
        (op >= 0x50 && op <= 0x5d) || (op >= 0x60 && op <= 0x6d) || (op >= 0x71 && op <= 0x7d) ||
        (op >= 0x80 && op <= 0x8d) || (op >= 0xc1 && op <= 0xcd) || op == 0x03 || op == 0x04 ||
        op == 0x4d || op == 0x4f || op == 0x70 || op == 0x95 || op == 0x96 || op == 0x97 ||
        op == 0x9e || op == 0x9f || op == 0xc0;
}
// One-byte opcodes whose effect does not depend on their own address. A BRA
// slot always executes them on the jump's only path, as the target did.
bool positionIndependent(const Instruction& i) {
    if (i.bytes.size() != 1 || i.operand_fragment) return false;
    const auto op = i.bytes[0];
    return registerOnly(i) || (op >= 0x3d && op <= 0x3f) || (op >= 0x30 && op <= 0x3b) ||
        (op >= 0x40 && op <= 0x4b) || op == 0x4c || op == 0x4e || op == 0x90 || op == 0xdf || op == 0xef;
}
std::size_t nextNonEmpty(const std::vector<Instruction>& code, std::size_t n) {
    while (n < code.size() && code[n].bytes.empty()) ++n;
    return n;
}
// Canonical fault island: IBT/IWT R6,#category / STOP / NOP.
bool faultStub(const std::vector<Instruction>& code, std::size_t at, std::uint16_t& value) {
    if (at + 2 >= code.size()) return false;
    const auto& literal = code[at];
    if (literal.old == Missing || literal.delay_slot || literal.operand_fragment ||
        !oneByte(code[at + 1], 0) || !oneByte(code[at + 2], 1) || code[at + 1].delay_slot || code[at + 2].delay_slot)
        return false;
    if (literal.bytes.size() == 2 && literal.bytes[0] == 0xa6)
        value = static_cast<std::uint16_t>(static_cast<std::int16_t>(static_cast<std::int8_t>(literal.bytes[1])));
    else if (literal.bytes.size() == 3 && literal.bytes[0] == 0xf6)
        value = static_cast<std::uint16_t>(literal.bytes[1] | (literal.bytes[2] << 8));
    else return false;
    return true;
}
// A slot an island may follow: the literal resets selectors, but an ALT
// prefix would turn IBT into LMS/SMS.
bool islandSlot(const Instruction& slot) {
    return slot.bytes.size() == 1 && !slot.operand_fragment && (slot.bytes[0] == 1 || selector(slot.bytes[0]));
}

struct LayoutFences {
    // Instruction indices before which the final layout may insert padding.
    std::vector<std::size_t> padding_before;
    bool crosses(std::size_t a, std::size_t b) const {
        if (a > b) std::swap(a, b);
        return padding_before[b + 1] != padding_before[a + 1];
    }
};
LayoutFences layoutFences(const std::vector<Instruction>& code, const std::set<std::size_t>& public_offsets) {
    LayoutFences fences;
    fences.padding_before.assign(code.size() + 1, 0);
    for (std::size_t n = 0; n < code.size(); ++n)
        fences.padding_before[n + 1] = fences.padding_before[n] +
            (oneByte(code[n], 2) && public_offsets.count(code[n].old) ? 1u : 0u);
    return fences;
}

// Each guard needs SOME island of its category within signed-byte range, not
// its own. Keep a greedy interval cover of islands and remove the rest. Only
// bytes are deleted, so every branch distance measured now can only shrink.
// Returns the old offsets of the islands that guards now reach by branch.
std::set<std::size_t> shareFaultStubs(std::vector<Instruction>& code, const std::set<std::size_t>& symbol_offsets,
                                      const std::set<std::size_t>& relocation_offsets, const LayoutFences& fences,
                                      GSUMachineScheduler::Statistics& stats) {
    struct Reference { std::size_t branch, stub; bool fallthrough; };
    struct Island { std::size_t index; bool removable; };
    const auto pos = positions(code);
    std::map<std::size_t, std::size_t> at_old;
    for (std::size_t n = 0; n < code.size(); ++n) if (code[n].old != Missing) at_old.emplace(code[n].old, n);
    std::map<std::uint16_t, std::vector<Island>> islands;
    std::map<std::uint16_t, std::vector<Reference>> references;
    std::set<std::size_t> pinned; // Islands with an entry no rewrite here can follow.
    std::set<std::size_t> interior_targets;
    for (const auto& i : code) if (i.target != Missing) interior_targets.insert(i.target);
    std::map<std::size_t, std::uint16_t> island_value;
    for (std::size_t s = 0; s < code.size(); ++s) {
        std::uint16_t value = 0;
        if (!faultStub(code, s, value)) continue;
        island_value.emplace(s, value);
        bool removable = s >= 2 && s + 3 < code.size() && code[s - 2].target != Missing &&
            code[s - 2].bytes.size() == 2 && code[s - 2].bytes[0] >= 6 && code[s - 2].bytes[0] <= 15 &&
            code[s - 2].plain && !code[s - 2].delay_slot && code[s - 1].delay_slot && islandSlot(code[s - 1]) &&
            code[s - 2].target == code[s + 3].old;
        for (std::size_t p = s; p < s + 3 && removable; ++p) {
            const auto old = code[p].old;
            removable = old != Missing && !symbol_offsets.count(old) && freeRange(code, p, p + 1, relocation_offsets) &&
                (p == s || !interior_targets.count(old));
        }
        islands[value].push_back({s, removable});
        if (removable) references[value].push_back({s - 2, s, true});
    }
    if (islands.empty()) return {};
    // Explicit branches into an island (codegen's backward reuse).
    for (std::size_t n = 0; n < code.size(); ++n) {
        const auto& branch = code[n];
        if (branch.target == Missing || branch.delay_slot) continue;
        const auto found = at_old.find(branch.target);
        if (found == at_old.end()) continue;
        const auto value = island_value.find(found->second);
        if (value == island_value.end()) continue;
        const bool redirectable = branch.bytes.size() == 2 && branch.bytes[0] >= 5 && branch.bytes[0] <= 15 &&
            branch.plain && n + 1 < code.size() && islandSlot(code[n + 1]);
        if (redirectable) references[value->second].push_back({n, found->second, false});
        else pinned.insert(found->second);
    }
    std::set<std::size_t> reached;
    for (auto& group : islands) {
        auto& refs = references[group.first];
        std::set<std::size_t> kept; // Island indices.
        for (const auto& island : group.second)
            if (!island.removable || pinned.count(island.index)) kept.insert(island.index);
        std::sort(refs.begin(), refs.end(), [](const Reference& a, const Reference& b) { return a.branch < b.branch; });
        std::map<std::size_t, std::size_t> assigned; // Reference branch index -> island index.
        const auto reaches = [&](std::size_t branch, std::size_t island) {
            const auto distance = static_cast<std::int64_t>(pos[island]) - static_cast<std::int64_t>(pos[branch] + 2);
            return distance >= -128 && distance <= 127 && !fences.crosses(branch, island);
        };
        for (const auto& ref : refs) {
            std::size_t best = Missing;
            for (const auto island : kept) if (reaches(ref.branch, island) &&
                (best == Missing || (island > ref.branch ? island - ref.branch : ref.branch - island) <
                                    (best > ref.branch ? best - ref.branch : ref.branch - best))) best = island;
            if (best == Missing) {
                // The rightmost reachable island covers the most later guards.
                for (const auto& island : group.second)
                    if (island.removable && reaches(ref.branch, island.index) &&
                        (best == Missing || island.index > best)) best = island.index;
                if (best == Missing) best = ref.stub; // Its current island is always reachable.
                kept.insert(best);
            }
            assigned[ref.branch] = best;
        }
        for (const auto& ref : refs) {
            const auto island = assigned.at(ref.branch);
            if (ref.fallthrough && kept.count(ref.stub)) continue; // Still falls into its own island.
            auto& branch = code[ref.branch];
            if (ref.fallthrough) branch.bytes[0] = static_cast<std::uint8_t>(branch.bytes[0] ^ 1u);
            branch.target = code[island].old;
            reached.insert(code[island].old);
        }
        for (const auto& island : group.second) {
            if (kept.count(island.index)) continue;
            for (std::size_t p = island.index; p < island.index + 3; ++p) {
                code[p].bytes.clear(); code[p].target = Missing; // Zero-byte anchors keep old boundaries.
            }
            ++stats.shared_fault_stubs;
        }
    }
    return reached;
}

// After sharing, a guard is Bcc island / NOP / continuation. The taken path
// faults: a register-only continuation opcode may run there before STOP.
void fillGuardSlots(std::vector<Instruction>& code, const std::set<std::size_t>& islands,
                    const SchedulingBoundaries& boundaries,
                    GSUMachineScheduler::Statistics& stats) {
    if (islands.empty()) return;
    const auto targets = liveTargets(code, boundaries.symbol_offsets);
    for (std::size_t n = 0; n + 2 < code.size(); ++n) {
        auto& branch = code[n];
        if (branch.bytes.size() != 2 || branch.bytes[0] < 6 || branch.bytes[0] > 15 || branch.delay_slot ||
            !branch.plain || !islands.count(branch.target) || !oneByte(code[n + 1], 1)) continue;
        const auto x = nextNonEmpty(code, n + 2);
        if (x >= code.size() || code[x].delay_slot || !registerOnly(code[x])) continue;
        // Nothing may enter at the slot, the vacated position or the anchors
        // between them: those paths would skip or double the moved opcode.
        bool entered = false;
        for (std::size_t p = n + 1; p <= x && !entered; ++p)
            entered = code[p].old == Missing || targets.count(code[p].old);
        // Current targets, not decode-time entries: this guard no longer
        // branches to its continuation.
        if (entered || !freeRange(code, n + 1, n + 2, boundaries.relocation_offsets) ||
            !freeRange(code, x, x + 1, boundaries.relocation_offsets))
            continue;
        // The branch keeps clear selectors, as the NOP did on both paths.
        code[n + 1] = code[x]; code[n + 1].delay_slot = true;
        code.erase(code.begin() + static_cast<std::ptrdiff_t>(x));
        ++stats.guard_slots;
    }
}

// BRA L / NOP becomes BRA L+1 / copy of L's first opcode. The target keeps
// its opcode for every other entry; only this edge executes the copy.
void fillJumpSlots(std::vector<Instruction>& code, const SchedulingBoundaries& boundaries,
                   const LayoutFences& fences,
                   GSUMachineScheduler::Statistics& stats) {
    const auto targets = liveTargets(code, boundaries.symbol_offsets);
    std::map<std::size_t, std::size_t> at_old;
    for (std::size_t n = 0; n < code.size(); ++n) if (code[n].old != Missing) at_old.emplace(code[n].old, n);
    const auto pos = positions(code);
    for (std::size_t n = 0; n + 1 < code.size(); ++n) {
        auto& branch = code[n];
        if (branch.bytes.size() != 2 || branch.bytes[0] != 5 || branch.target == Missing || branch.delay_slot ||
            !branch.plain || !oneByte(code[n + 1], 1)) continue;
        if (n && code[n - 1].bytes.size() == 1 && code[n - 1].bytes[0] >= 0x91 && code[n - 1].bytes[0] <= 0x94) continue;
        if (targets.count(code[n + 1].old) || code[n + 1].old == Missing ||
            !freeRange(code, n + 1, n + 2, boundaries.relocation_offsets)) continue;
        const auto found = at_old.find(branch.target);
        if (found == at_old.end()) continue;
        const auto y = nextNonEmpty(code, found->second);
        if (y + 1 >= code.size() || code[y].delay_slot || !positionIndependent(code[y]) ||
            code[y + 1].old == Missing || code[y + 1].delay_slot || code[y + 1].operand_fragment ||
            !freeRange(code, y, y + 1, boundaries.relocation_offsets)) continue; // A linked byte cannot be copied early.
        // Distances measured on the current list; later passes only delete.
        const auto distance = static_cast<std::int64_t>(pos[y + 1]) - static_cast<std::int64_t>(pos[n] + 2);
        if (distance < -128 || distance > 127 || fences.crosses(n, y + 1)) continue;
        code[n + 1] = code[y]; code[n + 1].delay_slot = true;
        // The copy has no old offset of its own: the original stays at L.
        code[n + 1].old = Missing; code[n + 1].target = Missing;
        branch.target = code[y + 1].old;
        ++stats.jump_slots;
    }
}
}

// Register reads/writes of one decoded instruction, for the few forms the
// liveness scan below understands. Anything else is "unknown".
struct RegisterEffect { bool known = false; unsigned reads = 0, writes = 0; };
RegisterEffect registerEffect(const std::vector<Instruction>& code, std::size_t n) {
    RegisterEffect effect;
    const auto& i = code[n];
    const unsigned alt = i.reachable_alt;
    if (i.bytes.empty() || i.operand_fragment || alt > 3 || i.reachable_source > 15 || i.reachable_destination > 15) return effect;
    const auto op = i.bytes[0];
    const unsigned reg = op & 15u, source = 1u << i.reachable_source, destination = 1u << i.reachable_destination;
    const bool after_with = n && code[n - 1].bytes.size() == 1 && code[n - 1].bytes[0] >= 0x20 && code[n - 1].bytes[0] <= 0x2f;
    if (i.bytes.size() == 2 && op >= 0xa0 && op <= 0xad && alt == 0) { effect.known = true; effect.writes = 1u << reg; return effect; }
    if (i.bytes.size() == 3 && op >= 0xf0 && op <= 0xfd && alt == 0) { effect.known = true; effect.writes = 1u << reg; return effect; }
    if (i.bytes.size() != 1) return effect;
    effect.known = true;
    if (op == 1 || (op >= 0x3d && op <= 0x3f) || (op >= 0x20 && op <= 0x2f)) return effect; // NOP, ALT, WITH
    if (op >= 0x10 && op <= 0x1f) { // TO, or MOVE after WITH
        if (after_with) { effect.reads = source; effect.writes = 1u << reg; }
        return effect;
    }
    if (op >= 0xb0 && op <= 0xbf) { // FROM, or MOVES after WITH
        if (after_with) { effect.reads = 1u << reg; effect.writes = destination; }
        return effect;
    }
    if (op >= 0x30 && op <= 0x3b) { effect.reads = source | (1u << reg); return effect; }      // STW/STB
    if (op >= 0x40 && op <= 0x4b) { effect.reads = 1u << reg; effect.writes = destination; return effect; } // LDW/LDB
    if ((op >= 0xd0 && op <= 0xde) || (op >= 0xe0 && op <= 0xee)) { effect.reads = effect.writes = 1u << reg; return effect; }
    if ((op >= 0x50 && op <= 0x6f) || (op >= 0x71 && op <= 0x8f) || (op >= 0xc1 && op <= 0xcf)) {
        const bool compare = op >= 0x60 && op <= 0x6f && alt == 3;
        const bool immediate = (alt & 2u) != 0 && !compare;
        effect.reads = source | (immediate ? 0u : 1u << reg);
        effect.writes = compare ? 0u : destination;
        return effect;
    }
    if (alt == 0 && (op == 3 || op == 4 || op == 0x4d || op == 0x4f || op == 0x95 || op == 0x96 || op == 0x97 ||
        op == 0x9e || op == 0xc0)) { effect.reads = source; effect.writes = destination; return effect; }
    effect.known = false;
    return effect;
}
// Whether `reg` is provably never read again from instruction `n` on: a
// bounded straight-line scan that ends at a full overwrite or at a function
// return. A return makes only R3 and R5-R8 dead: R0/R4 carry results, the
// private divmod kernels return R1/R2, and R9-R13 are preserved or owned by
// the caller. Those kernels neither write R5/R7/R8 nor expose R3/R6.
bool deadFrom(const std::vector<Instruction>& code, std::size_t n, Register reg) {
    const unsigned bit = 1u << static_cast<unsigned>(reg);
    for (unsigned steps = 0; steps < 32 && n < code.size(); ++steps, ++n) {
        const auto& i = code[n];
        if (i.reachable_entry || i.delay_slot) return false;
        if (i.bytes.size() == 1 && i.reachable_alt == 0 && i.bytes[0] == 0x9b) { // JMP R11
            if (n + 1 >= code.size() || code[n + 1].reachable_entry) return false;
            const auto slot = registerEffect(code, n + 1);
            return slot.known && !(slot.reads & bit) &&
                (reg == Register::R3 || (reg >= Register::R5 && reg <= Register::R8_MERGE_Y));
        }
        const auto effect = registerEffect(code, n);
        if (!effect.known || (effect.reads & bit)) return false;
        if (effect.writes & bit) return true;
        if (effect.writes & (1u << 15)) return false;
    }
    return false;
}

namespace {
// Register contents inside one straight-line window. `base` is a token:
// 0 is the plain numeric address space (`offset` is then the number), each
// ADDR16_RAM symbol has one token per name, and any other value gets a fresh
// token that only its own copies share. Equal tokens and offsets therefore
// prove equal 16-bit addresses; unequal ones prove nothing.
struct SymbolicValue {
    std::uint32_t base = 0;
    std::uint16_t offset = 0;
    bool static_ram = false; // `base` names relocated static RAM storage.
};
struct PendingStore { std::size_t index; SymbolicValue address; unsigned width; };

bool storeOpcode(const Instruction& i, unsigned alt) {
    return i.bytes.size() == 1 && i.bytes[0] >= 0x30 && i.bytes[0] <= 0x3b && alt <= 1;
}
// Whether executing `i` leaves clear selectors and ALT: every complete
// instruction except a TO/WITH/FROM/ALT prefix or a branch, which keeps them.
bool resetsSelectors(const Instruction& i) {
    if (i.bytes.empty() || i.operand_fragment) return false;
    const auto op = i.bytes[0];
    return !((op >= 0x10 && op <= 0x2f) || (op >= 0xb0 && op <= 0xbf) || (op >= 0x3d && op <= 0x3f) ||
             (op >= 5 && op <= 15));
}
// Control-flow entries whose incoming selector/ALT state is provably clear.
// decode() treats every entry as unknown, because an exported or relocated
// label can be reached from anywhere. A private label that nothing names in
// a relocation is reached only by fallthrough and by relative branches, so
// its state is clear when the instruction before it and every branch's
// delay slot reset the prefixes. Since stack-free leaves start their body
// at the stack guard's branch target, this keeps a leading literal provable.
std::set<std::size_t> clearEntries(const std::vector<Instruction>& code, const ObjectFile& object) {
    std::set<std::size_t> named;
    std::map<std::string, std::size_t> symbols;
    for (const auto& symbol : object.symbol_table) if (symbol.section == SymbolSection::CODE) {
        symbols.emplace(symbol.name, symbol.offset);
        if (symbol.name.empty() || symbol.name.front() != '\x01') named.insert(symbol.offset);
    }
    for (const auto& r : object.relocation_table) {
        const auto symbol = symbols.find(r.target_symbol_name);
        if (symbol != symbols.end()) named.insert(symbol->second);
    }
    std::map<std::size_t, bool> branch_edges_clear;
    for (std::size_t n = 0; n < code.size(); ++n) if (code[n].target != Missing) {
        const bool clear = n + 1 < code.size() && code[n + 1].delay_slot && resetsSelectors(code[n + 1]);
        auto edges = branch_edges_clear.emplace(code[n].target, true).first;
        edges->second = edges->second && clear;
    }
    std::set<std::size_t> result;
    for (std::size_t n = 1; n < code.size(); ++n) {
        const auto& i = code[n];
        if (!i.reachable_entry || i.delay_slot || named.count(i.old)) continue;
        const auto edges = branch_edges_clear.find(i.old);
        if ((edges != branch_edges_clear.end() && !edges->second) || !resetsSelectors(code[n - 1])) continue;
        result.insert(i.old);
    }
    return result;
}
bool onePrefix(const Instruction& i, std::uint8_t first, std::uint8_t last) {
    return i.bytes.size() == 1 && !i.delay_slot && !i.operand_fragment && i.bytes[0] >= first && i.bytes[0] <= last;
}
// Prefixes directly before `last` that only select operands/ALT for it:
// at most one selector (FROM, or WITH for a store) followed by at most one
// ALT, which is the order the code generator emits. Returns the first index
// of the group, or Missing if the group's incoming selector state is not
// plain. Accepting only one selector matters: WITH d / FROM s is a complete
// MOVES (a register and flag write), never a prefix of what follows, and
// requiring a plain state before the selector rejects that pending WITH.
std::size_t prefixGroupStart(const std::vector<Instruction>& code, std::size_t last, bool allow_with) {
    std::size_t start = last;
    if (start > 0 && onePrefix(code[start - 1], 0x3d, 0x3f)) --start;
    if (start > 0 && (onePrefix(code[start - 1], 0xb0, 0xbd) || (allow_with && onePrefix(code[start - 1], 0x20, 0x2d))))
        --start;
    return code[start].reachable_plain ? start : Missing;
}

// A nonvolatile store to static RAM is dead when a later nonvolatile store in
// the same straight-line window writes at least the same bytes and nothing in
// between can read memory or leave the window. Every load, SBK (which reuses
// the address latch the first store set), bank change, PLOT/RPIX, CACHE,
// branch or fault guard, call, return, STOP, LOOP, delay slot and control-flow
// entry ends the window. Only stores the code generator listed as nonvolatile
// may kill or be killed: machine bytes cannot distinguish a volatile access.
// Numeric (MMIO-like) and pointer-derived addresses are never candidates,
// and any other store in between ends the window too. Window state is O(1)
// per instruction and at most MaxPendingStores candidates are tracked.
struct PeepholeBoundaries {
    // Non-owning: run() keeps these offset sets alive through both passes.
    const std::set<std::size_t>& entries;            // Every CODE symbol and branch target.
    const std::set<std::size_t>& clear_entries;      // Entries proved to start with clear selectors.
    const std::set<std::size_t>& relocation_offsets; // Bytes a relocation patches or pins.
};
void eliminateDeadStores(std::vector<Instruction>& code, const ObjectFile& object,
                         const PeepholeBoundaries& boundaries,
                         const GSUMachineScheduler::Hints& hints, GSUMachineScheduler::Statistics& stats) {
    const auto& entries = boundaries.entries;
    const auto& clear_entries = boundaries.clear_entries;
    const auto& relocation_offsets = boundaries.relocation_offsets;
    if (hints.nonvolatile_stores.empty()) return;
    constexpr std::size_t MaxPendingStores = 16;
    // Opcode offset -> static RAM symbol of its only relocation.
    std::map<std::size_t, std::string> static_ram;
    std::map<std::size_t, unsigned> relocation_count;
    for (const auto& r : object.relocation_table) if (r.section_to_patch == SymbolSection::CODE) {
        ++relocation_count[r.patch_offset];
        if (r.type == RelocationType::ADDR16_RAM) static_ram[r.patch_offset] = r.target_symbol_name;
    }
    std::map<std::string, std::uint32_t> symbol_tokens;
    std::uint32_t next_token = 1;
    SymbolicValue registers[16];
    std::vector<PendingStore> pending;
    std::set<std::size_t> dead;
    const auto fresh = [&]() { SymbolicValue v; v.base = next_token++; return v; };
    const auto reset = [&]() { for (auto& r : registers) r = fresh(); pending.clear(); };
    const auto adjust = [](SymbolicValue v, int delta) {
        v.offset = static_cast<std::uint16_t>((v.offset + delta) & 0xffff); return v;
    };
    reset();
    for (std::size_t n = 0; n < code.size(); ++n) {
        const auto& i = code[n];
        if (entries.count(i.old)) reset();
        // A provably clear entry is decoded as such, not as unknown.
        const bool clear_entry = clear_entries.count(i.old) != 0;
        const unsigned alt = clear_entry ? 0u : i.reachable_alt;
        const unsigned source = clear_entry ? 0u : i.reachable_source;
        const unsigned destination = clear_entry ? 0u : i.reachable_destination;
        if (i.bytes.empty() || i.delay_slot || i.operand_fragment || alt > 3 || source > 15 || destination > 15) {
            reset(); continue;
        }
        const auto op = i.bytes[0];
        const bool after_with = !clear_entry && n && code[n - 1].bytes.size() == 1 &&
            code[n - 1].bytes[0] >= 0x20 && code[n - 1].bytes[0] <= 0x2f;
        if (i.bytes.size() == 1 && (op == 1 || (op >= 0x20 && op <= 0x2f) || (op >= 0x3d && op <= 0x3f))) continue;
        if (i.bytes.size() == 1 && ((op >= 0x10 && op <= 0x1f) || (op >= 0xb0 && op <= 0xbf))) {
            if (!after_with) continue; // TO/FROM prefix only.
            // WITH s / TO d is MOVE d,s; WITH d / FROM s is MOVES d,s.
            const bool move = op <= 0x1f;
            const unsigned to = move ? (op & 15u) : destination;
            if (to == 15) { reset(); continue; }
            registers[to] = registers[move ? source : (op & 15u)];
            continue;
        }
        const bool literal = alt == 0 && ((i.bytes.size() == 2 && op >= 0xa0 && op <= 0xaf) ||
                                          (i.bytes.size() == 3 && op >= 0xf0));
        if (literal) {
            const unsigned r = op & 15u;
            if (r == 15) { reset(); continue; } // A PC literal is a jump.
            bool relocated = false;
            for (std::size_t b = 0; b < i.bytes.size(); ++b) relocated = relocated || relocation_offsets.count(i.old + b);
            if (!relocated) {
                SymbolicValue v;
                v.offset = i.bytes.size() == 2
                    ? static_cast<std::uint16_t>(static_cast<std::int16_t>(static_cast<std::int8_t>(i.bytes[1])))
                    : static_cast<std::uint16_t>(i.bytes[1] | (i.bytes[2] << 8));
                registers[r] = v;
            } else {
                const auto symbol = static_ram.find(i.old);
                if (i.bytes.size() == 3 && symbol != static_ram.end() && relocation_count[i.old] == 1) {
                    // The linker overwrites the operand with the symbol's
                    // address, so no addend survives in the bytes.
                    auto token = symbol_tokens.find(symbol->second);
                    if (token == symbol_tokens.end()) token = symbol_tokens.emplace(symbol->second, next_token++).first;
                    SymbolicValue v; v.base = token->second; v.static_ram = true;
                    registers[r] = v;
                } else registers[r] = fresh();
            }
            continue;
        }
        if (storeOpcode(i, alt)) {
            const auto address = registers[op & 15u];
            const unsigned width = alt == 1 ? 1u : 2u;
            if (!address.static_ram || !hints.nonvolatile_stores.count(static_cast<std::uint32_t>(i.old))) {
                pending.clear(); // Volatile, numeric or pointer store: keep everything before it.
                continue;
            }
            for (auto p = pending.begin(); p != pending.end();) {
                if (p->address.base == address.base && p->address.offset == address.offset && p->width <= width) {
                    dead.insert(p->index); p = pending.erase(p);
                } else ++p;
            }
            pending.push_back({n, address, width});
            if (pending.size() > MaxPendingStores) pending.erase(pending.begin());
            continue;
        }
        if (registerOnly(i)) {
            if (destination == 15) { reset(); continue; }
            const bool add_immediate = alt == 2 && op >= 0x50 && op <= 0x5f;
            const bool sub_immediate = alt == 2 && op >= 0x60 && op <= 0x6f;
            if (add_immediate || sub_immediate) {
                const int k = static_cast<int>(op & 15u);
                registers[destination] = adjust(registers[source], add_immediate ? k : -k);
            } else if (increment(i)) {
                registers[op & 15u] = adjust(registers[op & 15u], op <= 0xdd ? 1 : -1);
            } else {
                // Unknown at an entry: every register is then forgotten.
                const auto effect = registerEffect(code, n);
                for (unsigned r = 0; r < 15; ++r)
                    if (!effect.known || (effect.writes & (1u << r))) registers[r] = fresh();
            }
            continue;
        }
        reset(); // Memory read, transfer, bank/ROM/PLOT state or unknown.
    }
    // Erase from the back so earlier indices stay valid. Groups never
    // overlap: each is one store and the prefixes directly before it.
    for (auto s = dead.rbegin(); s != dead.rend(); ++s) {
        const auto first = prefixGroupStart(code, *s, true);
        if (first == Missing || !freeRange(code, first, *s + 1, entries) ||
            !freeRange(code, first, *s + 1, relocation_offsets)) continue;
        code.erase(code.begin() + static_cast<std::ptrdiff_t>(first), code.begin() + static_cast<std::ptrdiff_t>(*s + 1));
        ++stats.dead_stores;
    }
}

// One-byte operations that, executed with destination R0, write only that
// destination and NZCV/carry state, and that a TO prefix retargets exactly.
bool retargetableResult(const Instruction& i) {
    if (i.bytes.size() != 1 || i.reachable_alt > 3 || i.reachable_source > 13) return false;
    const auto op = i.bytes[0];
    const unsigned alt = i.reachable_alt;
    const bool binary = (op >= 0x50 && op <= 0x6f) || (op >= 0x71 && op <= 0x8f) || (op >= 0xc1 && op <= 0xcf);
    if (binary) {
        const bool compare = op >= 0x60 && op <= 0x6f && alt == 3;
        const bool immediate = (alt & 2u) != 0 && !compare;
        // R14/R15 operands would read the ROM pointer or the moved PC.
        return !compare && (immediate || (op & 15u) <= 13);
    }
    if (alt == 0 && (op == 3 || op == 4 || op == 0x4d || op == 0x4f || op == 0x95 || op == 0x96 ||
        op == 0x97 || op == 0x9e || op == 0xc0)) return true;
    if (alt == 1 && op == 0x96) return true; // DIV2
    // LDW/LDB (Rm): Rm is the address; the source selector is irrelevant.
    return alt <= 1 && op >= 0x40 && op <= 0x4b && i.reachable_source == 0;
}

// A value materialized in R0 and then copied (WITH R0 / TO Rn) is produced in
// Rn directly when R0 is dead afterwards:
//   IBT/IWT R0,#k / MOVE Rn,R0         -> IBT/IWT Rn,#k
//   MOVE R0,Ra / MOVE Rn,R0            -> MOVE Rn,Ra
//   [FROM s] [ALT] op / MOVE Rn,R0     -> TO Rn / [FROM s] [ALT] op
// MOVE, IBT and IWT change no flag, and the retargeted operation computes the
// same value and NZCV, so flags are identical; only the dead R0 differs.
// The literal keeps its opcode offset, so an ADDR16 relocation (which patches
// only the operand bytes) stays valid. R13-R15 are excluded: R13 literals
// mark pixel loops, R14 starts ROM reads and R15 is the PC.
void retargetAccumulatorMoves(std::vector<Instruction>& code, const ObjectFile& object,
                              const PeepholeBoundaries& boundaries, GSUMachineScheduler::Statistics& stats) {
    const auto& entries = boundaries.entries;
    const auto& clear_entries = boundaries.clear_entries;
    const auto& relocation_offsets = boundaries.relocation_offsets;
    std::map<std::size_t, std::vector<RelocationType>> relocations;
    for (const auto& r : object.relocation_table)
        if (r.section_to_patch == SymbolSection::CODE) relocations[r.patch_offset].push_back(r.type);
    for (std::size_t w = 1; w + 1 < code.size(); ++w) {
        const auto& with = code[w];
        const auto& to = code[w + 1];
        if (!oneByte(with, 0x20) || to.bytes.size() != 1 || to.operand_fragment ||
            to.bytes[0] < 0x11 || to.bytes[0] > 0x1c) continue;
        const auto target = static_cast<std::uint8_t>(to.bytes[0] & 15u);
        if (!with.reachable_plain || with.delay_slot || to.delay_slot || !freeRange(code, w, w + 2, entries) ||
            !freeRange(code, w, w + 2, relocation_offsets) || !deadFrom(code, w + 2, Register::R0)) continue;
        auto& producer = code[w - 1];
        if (producer.bytes.empty() || producer.delay_slot || producer.operand_fragment) continue;
        const auto op = producer.bytes[0];
        const bool literal_alt_clear = producer.reachable_alt == 0 || clear_entries.count(producer.old) != 0;
        if (literal_alt_clear && ((producer.bytes.size() == 2 && op == 0xa0) ||
                                            (producer.bytes.size() == 3 && op == 0xf0))) {
            bool linkable = true;
            for (std::size_t b = 1; b < producer.bytes.size(); ++b)
                linkable = linkable && !relocations.count(producer.old + b);
            const auto relocation = relocations.find(producer.old);
            if (relocation != relocations.end()) for (const auto type : relocation->second)
                linkable = linkable && producer.bytes.size() == 3 &&
                    (type == RelocationType::ADDR16_IWT || type == RelocationType::ADDR16_RAM);
            if (!linkable) continue;
            producer.bytes[0] = static_cast<std::uint8_t>(op | target);
            code.erase(code.begin() + static_cast<std::ptrdiff_t>(w), code.begin() + static_cast<std::ptrdiff_t>(w + 2));
            ++stats.direct_literals;
            continue;
        }
        if (w >= 2 && oneByte(producer, 0x10)) {
            const auto& copy = code[w - 2];
            if (copy.bytes.size() != 1 || copy.bytes[0] < 0x21 || copy.bytes[0] > 0x2d || copy.operand_fragment ||
                (copy.bytes[0] & 15u) == target || !copy.reachable_plain || copy.delay_slot ||
                !freeRange(code, w - 1, w, entries) || !freeRange(code, w - 2, w, relocation_offsets)) continue;
            producer.bytes[0] = static_cast<std::uint8_t>(0x10u | target);
            code.erase(code.begin() + static_cast<std::ptrdiff_t>(w), code.begin() + static_cast<std::ptrdiff_t>(w + 2));
            ++stats.direct_results;
            continue;
        }
        if (!retargetableResult(producer) || producer.reachable_destination != 0) continue;
        const auto first = prefixGroupStart(code, w - 1, false);
        if (first == Missing || !freeRange(code, first, w, entries) || !freeRange(code, first, w, relocation_offsets))
            continue;
        // The TO byte moves in front of the group; it keeps its own offset,
        // which nothing names or patches (checked above).
        auto prefix = code[w + 1];
        const auto& before = code[first];
        prefix.plain = before.plain; prefix.reachable_plain = before.reachable_plain;
        prefix.alt = before.alt; prefix.reachable_alt = before.reachable_alt;
        prefix.source = before.source; prefix.reachable_source = before.reachable_source;
        prefix.destination = before.destination; prefix.reachable_destination = before.reachable_destination;
        prefix.reachable_entry = false;
        for (auto k = first; k < w; ++k) {
            code[k].plain = code[k].reachable_plain = false;
            code[k].destination = code[k].reachable_destination = target;
        }
        code.erase(code.begin() + static_cast<std::ptrdiff_t>(w), code.begin() + static_cast<std::ptrdiff_t>(w + 2));
        code.insert(code.begin() + static_cast<std::ptrdiff_t>(first), std::move(prefix));
        ++stats.direct_results;
    }
}
} // namespace

GSUMachineScheduler::Statistics GSUMachineScheduler::run(ObjectFile& object, const Hints& hints) {
    Statistics stats;
    // Layout/scheduling uses bounded per-byte maps. Bigger compiler objects are
    // left for the linker's existing one-program-bank diagnostic.
    if (object.config.target != TargetKind::GSU || object.code_section.size() > 65536) return stats;
    std::set<std::size_t> entries, protected_offsets, relocation_offsets;
    for (const auto& s : object.symbol_table) if (s.section == SymbolSection::CODE) entries.insert(s.offset);
    auto code = decode(object, entries);
    for (std::size_t n = 1; n < code.size(); ++n)
        code[n].delay_slot = transfer(code[n - 1].bytes[0]);
    {
        std::set<std::string> referenced;
        for (const auto& r : object.relocation_table) referenced.insert(r.target_symbol_name);
        std::set<std::size_t> reachable;
        for (const auto& s : object.symbol_table)
            if (s.section == SymbolSection::CODE && s.name != GSUCodeLayout::CacheAlignmentSymbol &&
                (s.name.empty() || s.name.front() != '\x01' || referenced.count(s.name))) reachable.insert(s.offset);
        const auto relaxed = decode(object, reachable);
        // Both decodes walk the same bytes, so the boundaries are identical.
        if (relaxed.size() != code.size()) throw std::runtime_error("GSU scheduler: inconsistent instruction boundaries.");
        for (std::size_t n = 0; n < code.size(); ++n) {
            code[n].reachable_plain = relaxed[n].plain;
            code[n].reachable_alt = relaxed[n].alt;
            code[n].reachable_source = relaxed[n].source;
            code[n].reachable_destination = relaxed[n].destination;
            code[n].reachable_entry = reachable.count(code[n].old) != 0;
        }
    }
    protected_offsets = entries;
    for (const auto& r : object.relocation_table) if (r.section_to_patch == SymbolSection::CODE) {
        // An LMS/SMS operand is a word index only right after its ALT
        // prefix, so that prefix is pinned with it (never a delay-slot copy).
        const bool short_ram = r.type == RelocationType::ADDR8_SHORT_RAM;
        for (unsigned b = short_ram && r.patch_offset ? 0u : 1u; b < (short_ram || r.type == RelocationType::ADDR24_BANK ? 3u : 4u); ++b) {
            protected_offsets.insert(r.patch_offset + b - 1);
            relocation_offsets.insert(r.patch_offset + b - 1);
        }
    }

    // Runs first, on the decoded stream whose offsets the hints name.
    // Computed once on the decoded stream; the rewrites below keep every
    // entry's offset and never touch the instruction before an entry.
    const auto clear_entries = clearEntries(code, object);
    const PeepholeBoundaries peephole_boundaries{entries, clear_entries, relocation_offsets};
    eliminateDeadStores(code, object, peephole_boundaries, hints, stats);

    // MOVE Ra -> Rb (WITH a / TO b) whose destination the very next
    // instruction overwrites without reading it: a home copy of a value its
    // only consumer already took from the source. MOVE changes no flag, and
    // the successor's selector state is plain both after the pair and (as
    // required here) before it, so deleting the pair changes nothing else.
    for (std::size_t n = 0; n + 2 < code.size(); ++n) {
        const auto& with = code[n];
        const auto& to = code[n + 1];
        if (!with.plain || with.delay_slot || with.bytes.size() != 1 || to.bytes.size() != 1 ||
            with.bytes[0] < 0x20 || with.bytes[0] > 0x2d || to.bytes[0] < 0x10 || to.bytes[0] > 0x1d) continue;
        const unsigned dead = to.bytes[0] & 15u;
        const auto& next = code[n + 2];
        std::size_t end = 0;
        if ((next.bytes.size() == 2 && next.bytes[0] == (0xa0u | dead)) ||
            (next.bytes.size() == 3 && next.bytes[0] == (0xf0u | dead))) {
            end = n + 3; // IBT/IWT Rb
        } else if (next.bytes.size() == 1 && next.bytes[0] == (0x10u | dead) && n + 3 < code.size()) {
            // TO Rb / [ALT1] LDW|LDB (Rm): reads only Rm.
            auto load = n + 3;
            if (oneByte(code[load], 0x3d) && load + 1 < code.size()) ++load;
            const auto op = code[load].bytes[0];
            if (code[load].bytes.size() == 1 && op >= 0x40 && op <= 0x4b && (op & 15u) != dead) end = load + 1;
        }
        if (!end || !freeRange(code, n, end, entries) || !freeRange(code, n, end, protected_offsets)) continue;
        code.erase(code.begin() + static_cast<std::ptrdiff_t>(n), code.begin() + static_cast<std::ptrdiff_t>(n + 2));
        ++stats.dead_copies;
    }

    // MOVE Ra -> R0 (WITH a / TO R0) feeding an accumulator operation that
    // overwrites R0: FROM a selects the same first operand without the copy.
    // The result, NZCV and every other register are identical, and R0 never
    // exposes the intermediate copy because the operation replaces it. CMP
    // (which leaves R0 holding the copy) and an R0 second operand (which
    // would read the old R0) are excluded.
    for (std::size_t n = 0; n + 2 < code.size(); ++n) {
        const auto& with = code[n];
        if (!with.reachable_plain || with.delay_slot || with.bytes.size() != 1 || with.bytes[0] < 0x21 || with.bytes[0] > 0x2d ||
            !oneByte(code[n + 1], 0x10)) continue;
        auto last = n + 2;
        unsigned alt = 0;
        if (code[last].bytes.size() == 1 && code[last].bytes[0] >= 0x3d && code[last].bytes[0] <= 0x3f) {
            alt = code[last].bytes[0] - 0x3cu;
            if (++last >= code.size()) continue;
        }
        if (code[last].bytes.size() != 1) continue;
        const auto op = code[last].bytes[0];
        const unsigned operand = op & 15u;
        const bool binary = (op >= 0x50 && op <= 0x6f) || (op >= 0x71 && op <= 0x8f) || (op >= 0xc1 && op <= 0xcf);
        bool accumulator = false;
        if (binary) {
            const bool compare = op >= 0x60 && op <= 0x6f && alt == 3;
            const bool immediate = (alt & 2u) != 0;
            accumulator = !compare && (immediate || (operand >= 1 && operand <= 13));
        } else if (alt == 0) {
            accumulator = op == 3 || op == 4 || op == 0x4d || op == 0x4f || op == 0x95 || op == 0x96 ||
                op == 0x97 || op == 0x9e || op == 0xc0;
        } else {
            accumulator = alt == 1 && op == 0x96; // DIV2
        }
        // The rewritten prefix keeps the WITH's offset, so an anchor there
        // survives; nothing may name or patch the bytes after it.
        if (!accumulator || !freeRange(code, n + 1, last + 1, entries) ||
            !freeRange(code, n, last + 1, relocation_offsets)) continue;
        const unsigned source = with.bytes[0] & 15u;
        code[n].bytes = {static_cast<std::uint8_t>(0xb0u | source)};
        code.erase(code.begin() + static_cast<std::ptrdiff_t>(n + 1));
        // The prefix now reaches the operation instead of being consumed.
        for (auto k = n + 1; k < last; ++k) {
            code[k].plain = code[k].reachable_plain = false;
            code[k].source = code[k].reachable_source = source;
        }
        ++stats.accumulator_sources;
    }

    // FROM Rx / TO Rx (either order) selects Rx as both operands, exactly as
    // WITH Rx does. WITH also sets B, which only a following TO/FROM reads,
    // so the pair must lead straight into its operation (or ALT prefix).
    // Incoming selector state is irrelevant: both forms overwrite the source
    // and destination and leave a pending ALT untouched. Canonical compiler
    // output never enters a label with B set, and a WITH is never left
    // dangling before the pair.
    for (std::size_t n = 0; n + 2 < code.size(); ++n) {
        const auto first = code[n].bytes[0], second = code[n + 1].bytes[0];
        if (code[n].bytes.size() != 1 || code[n + 1].bytes.size() != 1 || code[n].delay_slot) continue;
        const bool from_to = first >= 0xb0 && first <= 0xbd && second == (0x10u | (first & 15u));
        const bool to_from = first >= 0x10 && first <= 0x1d && second == (0xb0u | (first & 15u));
        if (!from_to && !to_from) continue;
        if (n && code[n - 1].bytes.size() == 1 && code[n - 1].bytes[0] >= 0x20 && code[n - 1].bytes[0] <= 0x2f) continue;
        // Look through one ALT prefix: it keeps B pending for its successor.
        auto consumer = n + 2;
        if (code[consumer].bytes.size() == 1 && code[consumer].bytes[0] >= 0x3d && code[consumer].bytes[0] <= 0x3f) {
            ++consumer;
            if (consumer >= code.size()) continue;
        }
        const auto next = code[consumer].bytes[0];
        if ((next >= 0x10 && next <= 0x2f) || (next >= 0xb0 && next <= 0xbf) || (next >= 0x3d && next <= 0x3f) ||
            transfer(next) || code[consumer].bytes.size() != 1) continue;
        if (!freeRange(code, n + 1, n + 2, entries) || !freeRange(code, n, n + 2, relocation_offsets)) continue;
        code[n].bytes = {static_cast<std::uint8_t>(0x20u | (first & 15u))};
        code.erase(code.begin() + static_cast<std::ptrdiff_t>(n + 1));
        ++stats.paired_selectors;
    }

    // WITH Rx / op / MOVE Rx,R0 with Rx dead afterwards: the operation can
    // deliver its result straight to R0 (FROM Rx / op), typically a return
    // value computed in its allocated home. Flags and R0 are identical; only
    // the dead Rx keeps its old value.
    for (std::size_t n = 0; n + 3 < code.size(); ++n) {
        const auto& with = code[n];
        if (!with.reachable_plain || with.delay_slot || with.bytes.size() != 1 || with.bytes[0] < 0x21 || with.bytes[0] > 0x2d) continue;
        const unsigned home = with.bytes[0] & 15u;
        auto operation = n + 1;
        unsigned alt = 0;
        if (code[operation].bytes.size() == 1 && code[operation].bytes[0] >= 0x3d && code[operation].bytes[0] <= 0x3f) {
            alt = code[operation].bytes[0] - 0x3cu; ++operation;
        }
        if (operation + 2 >= code.size() || code[operation].bytes.size() != 1) continue;
        const auto op = code[operation].bytes[0];
        const bool binary = (op >= 0x50 && op <= 0x6f) || (op >= 0x71 && op <= 0x8f) || (op >= 0xc1 && op <= 0xcf);
        const bool compare = op >= 0x60 && op <= 0x6f && alt == 3;
        const bool unary = op == 3 || op == 4 || op == 0x4d || op == 0x4f || op == 0x95 || op == 0x96 ||
            op == 0x97 || op == 0x9e || op == 0xc0;
        if (!((binary && !compare) || (unary && (alt == 0 || (alt == 1 && op == 0x96))))) continue;
        if (!oneByte(code[operation + 1], static_cast<std::uint8_t>(0x20u | home)) || !oneByte(code[operation + 2], 0x10)) continue;
        if (!freeRange(code, n + 1, operation + 3, entries) || !freeRange(code, n, operation + 3, relocation_offsets) ||
            !deadFrom(code, operation + 3, static_cast<Register>(home))) continue;
        code[n].bytes = {static_cast<std::uint8_t>(0xb0u | home)};
        for (auto k = n + 1; k <= operation; ++k) {
            code[k].plain = code[k].reachable_plain = false;
            code[k].source = code[k].reachable_source = home;
            code[k].destination = code[k].reachable_destination = 0;
        }
        code.erase(code.begin() + static_cast<std::ptrdiff_t>(operation + 1), code.begin() + static_cast<std::ptrdiff_t>(operation + 3));
        ++stats.accumulator_results;
    }

    // After the copy rewrites above, which already handle their own
    // R0/home forms, before any slot or ROM-latency scheduling.
    retargetAccumulatorMoves(code, object, peephole_boundaries, stats);

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
    std::set<std::size_t> symbol_offsets, public_offsets;
    for (const auto& s : object.symbol_table) if (s.section == SymbolSection::CODE) {
        if (s.name == GSUCodeLayout::CacheAlignmentSymbol) continue;
        symbol_offsets.insert(s.offset);
        if (!s.name.empty() && s.name.front() != '\x01') public_offsets.insert(s.offset);
    }
    const auto islands = shareFaultStubs(code, symbol_offsets, relocation_offsets, layoutFences(code, public_offsets), stats);
    // New island entries must fence any later pass that consults `entries`.
    entries.insert(islands.begin(), islands.end());
    protected_offsets.insert(islands.begin(), islands.end());
    const SchedulingBoundaries boundaries{entries, relocation_offsets, protected_offsets, symbol_offsets};
    fillGuardSlots(code, islands, boundaries, stats);
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
    planEntryIncrements(code, boundaries, object, jump_targets, stats);
    planImmediateSlots(code, object, jump_targets, relocation_offsets, protected_offsets, stats);
    fillJumpSlots(code, boundaries, layoutFences(code, public_offsets), stats);
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
