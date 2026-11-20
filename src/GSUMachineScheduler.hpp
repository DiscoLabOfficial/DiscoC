#pragma once

#include "ObjectFile.hpp"

#include <cstdint>
#include <set>

// Bounded, post-allocation scheduling of canonical compiler output only.
// Assembly input is NOT rescheduled: arbitrary GSU byte streams may deliberately
// split an immediate instruction between a delay slot and its branch target.
class GSUMachineScheduler {
public:
    // Facts only the code generator knows. Machine bytes cannot tell a
    // volatile store from a plain one, so dead-store elimination is opt-in:
    // a store is a candidate only if its STW/STB opcode offset is listed.
    struct Hints {
        // CODE offsets, as emitted and before any scheduling, of STW/STB
        // opcodes produced while lowering a nonvolatile store. The pass still
        // proves the address itself; this only rules out volatile accesses.
        std::set<std::uint32_t> nonvolatile_stores;
    };
    struct Statistics {
        unsigned delay_slots = 0;
        unsigned rom_operations = 0;
        unsigned cache_padding = 0;
        unsigned compact_branches = 0;
        unsigned short_jumps = 0;
        unsigned private_pixel_loops = 0;
        unsigned entry_slots = 0;
        unsigned immediate_slots = 0;
        unsigned nested_caches = 0;
        unsigned shared_fault_stubs = 0; // Removed duplicate IBT R6/STOP/NOP islands.
        unsigned guard_slots = 0;        // Continuation opcodes moved into a fault guard's slot.
        unsigned jump_slots = 0;         // Target opcodes copied into a BRA slot.
        unsigned dead_copies = 0;        // Removed MOVE pairs whose destination was overwritten unread.
        unsigned accumulator_sources = 0; // MOVE Ra,R0 + accumulator op rewritten as FROM Ra + op.
        unsigned paired_selectors = 0;    // FROM Rx / TO Rx before an operation rewritten as WITH Rx.
        unsigned accumulator_results = 0; // WITH Rx / op / MOVE Rx,R0 with Rx dead rewritten as FROM Rx / op.
        unsigned dead_stores = 0;     // Nonvolatile static stores overwritten before any possible read.
        unsigned direct_literals = 0; // IBT/IWT R0 / MOVE Rn,R0 with R0 dead rewritten as IBT/IWT Rn.
        unsigned direct_results = 0;  // R0 result / MOVE Rn,R0 with R0 dead rewritten as TO Rn / producer.
    };
    static Statistics run(ObjectFile& object, const Hints& hints = Hints{});
};
