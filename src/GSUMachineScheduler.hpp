#pragma once

#include "ObjectFile.hpp"

// Bounded, post-allocation scheduling of canonical compiler output only.
// Assembly input is NOT rescheduled: arbitrary GSU byte streams may deliberately
// split an immediate instruction between a delay slot and its branch target.
class GSUMachineScheduler {
public:
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
    };
    static Statistics run(ObjectFile& object);
};
