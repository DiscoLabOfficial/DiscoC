#include "GSUMachineScheduler.hpp"
#include "GSUCodeLayout.hpp"
#include "GSUInstructionModel.hpp"
#include "AssemblyGenerator.hpp"
#include "Assembler.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
ObjectFile object(std::vector<std::uint8_t> bytes) {
    ObjectFile o; o.code_section = std::move(bytes); o.config.code_start_address = 0x008000; return o;
}
std::vector<std::uint8_t> linked(const ObjectFile& o) {
    auto bytes = o.code_section;
    for (const auto& r : o.relocation_table) {
        bool found = false;
        for (const auto& s : o.symbol_table) if (s.name == r.target_symbol_name) {
            const auto address = o.config.code_start_address + s.offset;
            bytes.at(r.patch_offset + 1) = static_cast<std::uint8_t>(address);
            bytes.at(r.patch_offset + 2) = static_cast<std::uint8_t>(address >> 8); found = true;
        }
        require(found, "Missing unit-test relocation symbol");
    }
    return bytes;
}
void equivalent(const ObjectFile& before, const ObjectFile& after, bool compare_memory = true) {
    DiscoGSU::Machine a(linked(before), 0x008000), b(linked(after), 0x008000);
    a.seed(0x009000, 149, true); b.seed(0x009000, 149, true);
    a.run(); b.run();
    for (unsigned r = 0; r < 14; ++r) {
        // LINK naturally records the new, relocated PC, not the old byte offset.
        if (r != 11 && r != 13) require(a.reg(r) == b.reg(r), "Scheduler changed a live register");
    }
    require(a.graphicsState("--colr") == b.graphicsState("--colr") &&
            a.graphicsState("--por") == b.graphicsState("--por") &&
            a.graphicsState("--plots") == b.graphicsState("--plots") && a.graphicsState("--rpix") == b.graphicsState("--rpix"),
            "Scheduler changed graphics state/effects");
    require(a.metrics().taken_branches + a.metrics().jumps == b.metrics().taken_branches + b.metrics().jumps,
        "Scheduler changed an executed control transfer");
    if (compare_memory) for (std::uint32_t p = 0x700000; p <= 0x71ffff; ++p)
        require(a.byte(p) == b.byte(p), "Scheduler changed RAM/framebuffer bytes");
    else require(a.metrics().stores == 0 && b.metrics().stores == 0 &&
        a.metrics().ram_loads == 0 && b.metrics().ram_loads == 0, "Pure ALU fixture unexpectedly accessed RAM");
}
void conditionSlots() {
    // Independent flag masks, not the scheduler's classifier. Execute both
    // paths and arithmetic boundary cases with each of the four ALT states.
    struct Operation { std::uint8_t opcode; unsigned flags; };
    const unsigned nz = 3, carry = 4, overflow = 8;
    const Operation operations[] = {{3,nz|carry},{4,nz|carry},{0x4d,nz},{0x4f,nz},
        {0x51,nz|carry|overflow},{0x61,nz|carry|overflow},{0x71,nz},{0x81,nz},
        {0x95,nz},{0x96,nz|carry},{0x97,nz|carry},{0x9e,nz},{0xc0,nz},{0xc1,nz}};
    const unsigned flags[] = {0, 1|overflow, 1|overflow, 2, 2, 1, 1, carry, carry, overflow, overflow};
    for (const auto& operation : operations) for (unsigned alt = 0; alt < 4; ++alt)
        for (unsigned branch = 5; branch <= 15; ++branch) {
            for (const auto left : {0u,0x7fffu,0x8000u,0xffffu}) {
                auto o = object({0xf0,static_cast<std::uint8_t>(left),static_cast<std::uint8_t>(left >> 8),
                    0xa1,1,0xb0,0x3f,0x61,0x20});
                if (alt) o.code_section.push_back(static_cast<std::uint8_t>(0x3c + alt));
                o.code_section.insert(o.code_section.end(), {operation.opcode,static_cast<std::uint8_t>(branch),5,1,
                    0xa6,2,0,1,0xa7,3,0,1});
                const auto before = o;
                const bool eligible = (operation.flags & flags[branch - 5]) == 0;
                require(GSUMachineScheduler::run(o).delay_slots == (eligible ? 1u : 0u),
                    "Delay slot did not respect the instruction/branch NZCV dependency");
                equivalent(before, o, false);
            }
        }
    for (const auto opcode : {0x4c,0x4e}) for (unsigned branch = 5; branch <= 15; ++branch) {
        auto o = object({0xa0,3,static_cast<std::uint8_t>(opcode),static_cast<std::uint8_t>(branch),5,1,0xa6,2,0,1,0xa7,3,0,1});
        const auto before = o;
        require(GSUMachineScheduler::run(o).delay_slots == 1, "Flag-preserving PLOT/COLOR did not fill Bcc's slot");
        equivalent(before,o);
    }
    auto cmode = object({0xa0,3,0x3d,0x4e,0x09,5,1,0xa6,2,0,1,0xa7,3,0,1}); const auto before_cmode = cmode;
    require(GSUMachineScheduler::run(cmode).delay_slots == 1, "CMODE's register value/state was not preserved in Bcc's slot");
    equivalent(before_cmode,cmode);
    for (unsigned branch = 5; branch <= 15; ++branch) {
        auto o = object({0xa0,0x95,0x27,0xb0,static_cast<std::uint8_t>(branch),5,1,0xa6,2,0,1,0xa8,3,0,1});
        const auto before = o;
        require(GSUMachineScheduler::run(o).delay_slots == (branch == 5 || branch == 12 || branch == 13 ? 1u : 0u),
            "MOVES slot ignored its N/Z/V effects or carry-preserving copy");
        equivalent(before,o,false);
    }
    // These registers and memory/bank/flush effects are deliberately fenced.
    for (const auto& bytes : {std::vector<std::uint8_t>{0x2e,0xc1}, {0x2f,0xc1}, {0x20,0x7e},
            {0x20,0x3f,0x6f}, {0x3d,0x4c}, {0x3e,0xdf}, {0x31}, {0x41}, {0x90}}) {
        auto o = object({0xa0,3});
        o.code_section.insert(o.code_section.end(),bytes.begin(),bytes.end());
        o.code_section.insert(o.code_section.end(),{5,2,1,1,0,1}); const auto before = o;
        require(GSUMachineScheduler::run(o).delay_slots == 0 && o.code_section == before.code_section,
            "Unsafe state/PC/ROM-pointer operation filled a delay slot");
    }
}
void compactBranches() {
    for (const unsigned count : {1u, 2u, 127u, 128u, 65535u}) {
        auto o = object({0xf5, static_cast<std::uint8_t>(count), static_cast<std::uint8_t>(count >> 8),
            0xe5,9,4,1,5,0xfa,1,0xa0,31,0,1});
        o.symbol_table.push_back({"\x01" "unused.edge",SymbolSection::CODE,7});
        const auto before = o;
        require(GSUMachineScheduler::run(o).compact_branches == 1 && o.code_section.size() + 3 == before.code_section.size(),
            "Inverse conditional branch did not remove the redundant BRA/NOP");
        DiscoGSU::Machine a(linked(before),0x008000), b(linked(o),0x008000); a.run(); b.run();
        require(a.reg(0) == 31 && b.reg(0) == 31 && a.reg(5) == 0 && b.reg(5) == 0 &&
            a.metrics().taken_branches == count && b.metrics().taken_branches == count - 1,
            "Compact branch changed the loop's taken/final iteration");
        Assembler assembler;
        require(assembler.assemble(AssemblyGenerator(o).generate()).code_section == o.code_section,
            "Compact branch lost its byte-exact assembly labels");
    }
    for (const bool useful_slot : {false,true}) {
        auto o = object({0xf5,2,0,0xe5,9,4,1,5,0xfa,static_cast<std::uint8_t>(useful_slot ? 0xd0 : 1),0xa0,31,0,1});
        if (!useful_slot) o.symbol_table.push_back({"alternate_entry",SymbolSection::CODE,7});
        const auto before = o;
        require(GSUMachineScheduler::run(o).compact_branches == 0 && o.code_section == before.code_section,
            "Branch compaction removed a useful slot or independently callable entry");
    }
}

void shortJumps() {
    for (const unsigned gap : {0u, 1u, 126u, 127u, 128u}) {
        auto o = object({1,0xff,0,0,1});
        o.code_section.resize(5 + gap, 1);
        o.symbol_table.push_back({"target", SymbolSection::CODE, static_cast<std::uint32_t>(o.code_section.size())});
        o.code_section.insert(o.code_section.end(), {0xa0,31,0,1});
        o.relocation_table.push_back({"target",SymbolSection::CODE,1,RelocationType::ADDR16_IWT});
        const auto before = o;
        const bool fits = gap <= 126;
        const auto stats = GSUMachineScheduler::run(o);
        require(stats.short_jumps == (fits ? 1u : 0u) && o.code_section.size() == before.code_section.size() - (fits ? 1u : 0u),
            "Symbol-aware PC jump shortening ignored the exact signed-byte boundary");
        require(o.relocation_table.size() == (fits ? 0u : 1u), "Shortened jump retained an absolute relocation");
        equivalent(before,o,false);
        require(Assembler().assemble(AssemblyGenerator(o).generate()).code_section == o.code_section,
            "Shortened jump lost byte-exact assembly export");
    }
    for (const unsigned at : {127u,128u,129u}) {
        auto o = object({0,1}); o.code_section.resize(at,1);
        o.code_section.insert(o.code_section.end(), {0xff,0,0,1});
        o.symbol_table.push_back({"target",SymbolSection::CODE,2});
        o.relocation_table.push_back({"target",SymbolSection::CODE,at,RelocationType::ADDR16_IWT});
        require(GSUMachineScheduler::run(o).short_jumps == (at <= 128 ? 1u : 0u),
            "Backward PC jump shortening ignored the -128 boundary");
    }
    // A call's LINK #4 distance and ALT/source selectors are hard fences.
    for (const auto& prefix : {std::vector<std::uint8_t>{0x94}, {0x3d}, {0x15}, {0xb5}}) {
        auto o = object({1}); o.code_section.insert(o.code_section.end(),prefix.begin(),prefix.end());
        const auto at = static_cast<std::uint32_t>(o.code_section.size());
        o.code_section.insert(o.code_section.end(), {0xff,0,0,1,0,1});
        o.symbol_table.push_back({"target",SymbolSection::CODE,at+4});
        o.relocation_table.push_back({"target",SymbolSection::CODE,at,RelocationType::ADDR16_IWT});
        require(GSUMachineScheduler::run(o).short_jumps == 0 && o.relocation_table.size() == 1,
            "PC shortening crossed LINK/ALT/source/destination state");
    }
}

void targetPrefixSlots() {
    for (const auto selector : {0x17,0xba}) for (const auto stack : {0x20feu,0x1ffeu}) {
        auto o = object({0xa0,8,0xfa,static_cast<std::uint8_t>(stack),static_cast<std::uint8_t>(stack >> 8),
            0xf3,0,0x20,0xba,0x3f,0x63,0x0d,5,1,0xa6,2,0,1,
            static_cast<std::uint8_t>(selector),static_cast<std::uint8_t>(selector == 0x17 ? 3 : 0x4d),0,1});
        o.symbol_table.push_back({"\x01select",SymbolSection::CODE,18}); const auto before = o;
        require(GSUMachineScheduler::run(o).delay_slots == 1 && o.code_section.size() == 21 &&
            o.code_section[13] == selector && o.symbol_table[0].offset == 18, "Taken-edge TO/FROM selector was not scheduled");
        equivalent(before,o);
    }
    for (const auto stack : {0x2000u,0x1ffeu}) {
        auto o = object({0xfa,static_cast<std::uint8_t>(stack),static_cast<std::uint8_t>(stack >> 8),
            0xf3,0,0x20,0xba,0x3f,0x63,0x0d,5,1,0xa6,2,0,1,0x2a,0x10,0,1});
        o.symbol_table.push_back({"\x01guard",SymbolSection::CODE,16}); const auto before = o;
        require(GSUMachineScheduler::run(o).delay_slots == 1 && o.code_section.size() == 19 &&
                o.code_section[11] == 0x2a && o.code_section[16] == 0x10 && o.symbol_table[0].offset == 16,
                "Unique taken-edge WITH was not hoisted with its continuation label remapped");
        equivalent(before,o);
        const auto assembly = AssemblyGenerator(o).generate();
        require(Assembler().assemble(assembly).code_section == o.code_section, "Taken-edge slot lost byte-exact assembly export");
        const auto second = o;
        require(GSUMachineScheduler::run(o).delay_slots == 0 && o.code_section == second.code_section,
            "Taken-edge prefix scheduling is not idempotent");
        for (unsigned fence = 0; fence < 5; ++fence) {
            auto unsafe = before;
            if (fence == 0) unsafe.symbol_table.push_back({"public_alias",SymbolSection::CODE,16});
            if (fence == 1) unsafe.code_section.insert(unsafe.code_section.end(),{5,0xfa,1}); // Another incoming edge to 16.
            if (fence == 2) unsafe.relocation_table.push_back({"\x01guard",SymbolSection::CODE,3,RelocationType::ADDR16_IWT});
            if (fence == 3) unsafe.code_section[8] = 0x3d; // Live ALT1 changes literal semantics.
            if (fence == 4) unsafe.code_section[12] = 0x31; // Fallthrough consumes WITH instead of resetting it.
            const auto saved = unsafe;
            const auto stats = GSUMachineScheduler::run(unsafe);
            // Fence 1's own BRA may receive a copy of its target (jump slot);
            // the guard and its taken-edge selector must stay untouched.
            require(stats.delay_slots == 0 && unsafe.code_section.size() == saved.code_section.size() &&
                std::equal(saved.code_section.begin(), saved.code_section.begin() + 20, unsafe.code_section.begin()) &&
                stats.jump_slots == (fence == 1 ? 1u : 0u),
                "Taken-edge prefix crossed another entry, relocation, or unsafe fallthrough state");
            if (fence == 1)
                require(unsafe.code_section[20] == 5 && unsafe.code_section[21] == 0xfb && unsafe.code_section[22] == 0x2a,
                    "BRA slot did not copy its target opcode and skip it");
        }
    }
}

void privatePixelSlots() {
    const std::string body = "\x01pixel#body";
    auto original = object({0xfc,3,0,0xfd,0,0,1,0x4c,0x3c,1,0x3d,0x4c,0,1});
    original.symbol_table.push_back({body,SymbolSection::CODE,7});
    original.relocation_table.push_back({body,SymbolSection::CODE,3,RelocationType::ADDR16_IWT});
    for (const unsigned count : {0u,1u,2u,65535u}) {
        auto before = original;
        before.code_section[1] = static_cast<std::uint8_t>(count);
        before.code_section[2] = static_cast<std::uint8_t>(count >> 8);
        auto after = before;
        const auto statistics = GSUMachineScheduler::run(after);
        require(statistics.private_pixel_loops == 1 && statistics.delay_slots == 1 &&
            after.code_section.size() + 1 == before.code_section.size() &&
            after.code_section[7] == 0x3c && after.code_section[8] == 0x4c && after.symbol_table[0].offset == 7,
            "Private pixel entry did not retain its anchor on LOOP / useful PLOT slot");
        equivalent(before, after);
        DiscoGSU::Machine executed(linked(after), 0x008000); executed.run();
        require(executed.graphicsState("--plots") == (count ? count : 65536u) && executed.reg(12) == 0 && executed.reg(1) == count,
            "Private pixel slot changed first/final iteration or raw zero wrap");
        require(Assembler().assemble(AssemblyGenerator(after).generate()).code_section == after.code_section,
            "Private LOOP/PLOT assembly round trip changed bytes");
        const auto saved = after;
        require(GSUMachineScheduler::run(after).private_pixel_loops == 0 && after.code_section == saved.code_section,
            "Private pixel scheduling was not idempotent");
    }
    for (unsigned fence = 0; fence < 8; ++fence) {
        auto unsafe = original;
        if (fence == 0) unsafe.symbol_table.push_back({"public_pixel",SymbolSection::CODE,7});
        if (fence == 1) unsafe.symbol_table.push_back({"\x01loop_entry",SymbolSection::CODE,8});
        if (fence == 2) unsafe.symbol_table.push_back({"\x01slot_entry",SymbolSection::CODE,9});
        if (fence == 3) unsafe.relocation_table[0].type = RelocationType::ADDR16_JAL;
        if (fence == 4) unsafe.code_section[6] = 0x3d;
        if (fence == 5) unsafe.code_section.insert(unsafe.code_section.end(), {5,0xf7,1});
        if (fence == 6) {
            unsafe.code_section.insert(unsafe.code_section.begin() + 3, 0x3d);
            unsafe.symbol_table[0].offset = 8; unsafe.relocation_table[0].patch_offset = 4;
        }
        if (fence == 7) {
            unsafe.code_section.insert(unsafe.code_section.begin() + 6, 0x9b);
            unsafe.symbol_table[0].offset = 8;
        }
        const auto saved = unsafe;
        const auto stats = GSUMachineScheduler::run(unsafe);
        // Fence 5's appended BRA may copy its PLOT target into its own slot;
        // the private loop itself must stay untouched.
        const auto kept = saved.code_section.size() - (fence == 5 ? 3u : 0u);
        require(stats.private_pixel_loops == 0 && unsafe.code_section.size() == saved.code_section.size() &&
            std::equal(saved.code_section.begin(), saved.code_section.begin() + static_cast<std::ptrdiff_t>(kept),
                unsafe.code_section.begin()) && stats.jump_slots == (fence == 5 ? 1u : 0u),
            "Private pixel slot crossed an external/secondary entry, LINK/ALT state or non-R13 relocation");
    }
}
}
void pipelinePlan() {
    auto split = object({0x01, 0x05, 0x02, 0x01, 0x00, 0xa5, 0x09, 0x00, 0x01});
    const auto split_before = split;
    require(GSUMachineScheduler::run(split).immediate_slots == 1 && split.code_section.size() == 8 &&
            split.code_section[3] == 0xa5 && split.code_section[5] == 0x09,
            "Unconditional BRA did not leave only the IBT opcode in its delay slot");
    equivalent(split_before, split, false);
    DiscoGSU::Machine split_machine(linked(split), 0x008000); split_machine.run();
    require(split_machine.reg(5) == 9, "Split IBT did not write its immediate");

    auto adjacent = object({0x01, 0x05, 0x01, 0x01, 0xa5, 0x09, 0x00, 0x01});
    const auto adjacent_before = adjacent;
    require(GSUMachineScheduler::run(adjacent).immediate_slots == 1 && adjacent.code_section[3] == 0xa5 &&
            adjacent.code_section[4] == 0x09, "Adjacent IBT was not split across BRA's delay slot");
    equivalent(adjacent_before, adjacent, false);

    auto fallthrough = object({0x01, 0x05, 0x02, 0x01, 0x01, 0xa5, 0x09, 0x00, 0x01});
    const auto fallthrough_before = fallthrough;
    require(GSUMachineScheduler::run(fallthrough).immediate_slots == 0 &&
            fallthrough.code_section == fallthrough_before.code_section,
            "A fall-through immediate was split into a delay slot");
    auto conditional = object({0x01, 0x09, 0x02, 0x01, 0x00, 0xa5, 0x09, 0x00, 0x01});
    const auto conditional_before = conditional;
    require(GSUMachineScheduler::run(conditional).immediate_slots == 0 &&
            conditional.code_section == conditional_before.code_section,
            "A conditional branch split an immediate that both paths execute");

    for (const auto opcode : {std::uint8_t{0xd5}, std::uint8_t{0xe5}}) {
        auto entry = object({0x01, 0x05, 0x02, 0x01, 0x00, opcode, 0x05, 0x01, 0x01, 0x00, 0x01});
        entry.symbol_table.push_back({"\x01head", SymbolSection::CODE, 5});
        const auto entry_before = entry;
        const auto entry_stats = GSUMachineScheduler::run(entry);
        require(entry_stats.entry_slots == 1 && entry.code_section.size() == 10 && entry.code_section[5] == 5 &&
                entry.code_section[7] == opcode && entry.symbol_table[0].offset == 5,
                "An INC/DEC entry lost its BRA anchor or useful delay slot");
        equivalent(entry_before, entry, false);
        DiscoGSU::Machine entry_machine(linked(entry), 0x008000); entry_machine.run();
        require(entry_machine.reg(5) == (opcode == 0xd5 ? 1 : 65535), "Retargeted INC/DEC entry did not run once");
        for (unsigned fence = 0; fence < 5; ++fence) {
            auto unsafe = entry_before;
            if (fence == 0) unsafe.symbol_table.push_back({"public_head", SymbolSection::CODE, 5});
            if (fence == 1) unsafe.symbol_table.push_back({"\x01" "branch", SymbolSection::CODE, 6});
            if (fence == 2) unsafe.symbol_table.push_back({"\x01slot", SymbolSection::CODE, 8});
            if (fence == 3) unsafe.relocation_table.push_back({"\x01head", SymbolSection::CODE, 5, RelocationType::ADDR24_BANK});
            if (fence == 4) unsafe.relocation_table.push_back({"\x01head", SymbolSection::CODE, 7, RelocationType::ADDR24_BANK});
            const auto saved = unsafe;
            const auto stats = GSUMachineScheduler::run(unsafe);
            // Without the entry slot, the first BRA may copy INC/DEC into its
            // own slot and skip it, unless a relocation still patches that byte.
            auto expected = saved.code_section;
            if (fence != 3) { expected[2] = 3; expected[3] = opcode; }
            require(stats.entry_slots == 0 && stats.jump_slots == (fence == 3 ? 0u : 1u) && unsafe.code_section == expected &&
                unsafe.symbol_table[0].offset == saved.symbol_table[0].offset &&
                unsafe.relocation_table.size() == saved.relocation_table.size(),
                "Entry scheduling crossed a public entry, branch/slot label, or relocated byte");
            if (!unsafe.relocation_table.empty())
                require(unsafe.relocation_table[0].patch_offset == saved.relocation_table[0].patch_offset,
                    "Fenced entry scheduling changed a relocation offset");
        }
    }

    auto nested = object({0x02});
    nested.code_section.insert(nested.code_section.end(), 2, 0x01);
    nested.code_section.insert(nested.code_section.end(), {0x02, 0x00, 0x01});
    nested.symbol_table.push_back({GSUCodeLayout::CacheAlignmentSymbol, SymbolSection::CODE, 0});
    const auto nested_before = nested;
    require(GSUMachineScheduler::run(nested).nested_caches == 1, "Proven same-CBR/clear-selector CACHE was kept");
    require(std::count(nested.code_section.begin(), nested.code_section.end(), 0x02) == 1,
            "Nested CACHE opcode remained in the window");
    equivalent(nested_before, nested, false);

    auto spaced = object({0x02});
    spaced.code_section.insert(spaced.code_section.end(), 520, 0x01);
    spaced.code_section.insert(spaced.code_section.end(), {0x02, 0x00, 0x01});
    require(GSUMachineScheduler::run(spaced).nested_caches == 0 &&
            std::count(spaced.code_section.begin(), spaced.code_section.end(), 0x02) == 2,
            "A CACHE outside the previous 512-byte window was removed");

    auto function_entry = nested_before;
    function_entry.symbol_table.push_back({"inner", SymbolSection::CODE, 3});
    require(GSUMachineScheduler::run(function_entry).nested_caches == 0 &&
            std::count(function_entry.code_section.begin(), function_entry.code_section.end(), 0x02) == 2,
            "A public entry CACHE inside another window was removed");
}
void nestedCacheDelaySlots() {
    for (const auto branch : {std::uint8_t{5}, std::uint8_t{9}}) for (const auto input : {0u, 1u}) {
        auto o = object({2,0xf0,static_cast<std::uint8_t>(input),0,0xa3,0,0xb0,0x3f,0x63,
            branch,5,2,0xa5,0x44,0,1,0xa0,42,0,1});
        const auto before = o;
        const auto stats = GSUMachineScheduler::run(o);
        require(stats.nested_caches == 0 && o.code_section.size() == before.code_section.size(),
            "Unproven effective-PC CACHE delay slot was changed");
        require(o.code_section[11] == 2, "Branch CACHE delay slot lost its CBR effect or one-byte pipeline boundary");
        equivalent(before, o, false);
        DiscoGSU::Machine machine(linked(o),0x008000); machine.run();
        const bool taken = branch == 5 || input == 0;
        require(machine.reg(0) == (taken ? 42 : input) && machine.reg(5) == (taken ? 0 : 0x44),
            "CACHE removal split a following immediate on a taken/untaken branch");
    }
    for (const auto trips : {1u, 2u, 7u}) {
        auto o = object({2,0xac,static_cast<std::uint8_t>(trips),0xfd,0,0,0xd5,0x3c,2,0xa0,42,0,1});
        o.symbol_table.push_back({"\x01loop-cache-slot",SymbolSection::CODE,6});
        o.relocation_table.push_back({"\x01loop-cache-slot",SymbolSection::CODE,3,RelocationType::ADDR16_IWT});
        const auto before = o;
        require(GSUMachineScheduler::run(o).nested_caches == 0 && o.code_section[8] == 2,
            "LOOP's effective-PC CACHE lost its CBR effect or required delay-slot byte");
        equivalent(before, o, false);
        DiscoGSU::Machine machine(linked(o),0x008000); machine.run();
        require(machine.reg(5) == trips && machine.reg(0) == 42,
            "CACHE removal changed LOOP's taken/final iteration pipeline");
    }
    auto transfers = object({2,5,3,2,5,2,1,1,0,1});
    const auto before = transfers;
    GSUMachineScheduler::run(transfers); equivalent(before, transfers, false);
}

void cacheProvenance() {
    // IBT's opcode moves into BRA's slot, but its #02 operand remains at the
    // target. Numeric equality with CACHE must not turn that operand into code.
    auto immediate = object({2,5,2,1,0,0xa5,2,0,1});
    immediate.symbol_table.push_back({GSUCodeLayout::CacheAlignmentSymbol, SymbolSection::CODE, 0});
    const auto immediate_before = immediate;
    const auto split = GSUMachineScheduler::run(immediate);
    require(split.immediate_slots == 1 && split.nested_caches == 0 && immediate.code_section[5] == 2,
        "CACHE elimination interpreted a split immediate #02 operand as executable code");
    equivalent(immediate_before, immediate, false);
    DiscoGSU::Machine immediate_machine(linked(immediate), 0x008000); immediate_machine.run();
    require(immediate_machine.reg(5) == 2, "Split immediate #02 lost its literal value");

    for (const bool aligned : {false, true}) {
        auto window = object({2}); window.code_section.insert(window.code_section.end(), 20, 1);
        window.code_section.insert(window.code_section.end(), {2,0,1});
        if (aligned) window.symbol_table.push_back({GSUCodeLayout::CacheAlignmentSymbol, SymbolSection::CODE, 0});
        const auto before = window;
        require(GSUMachineScheduler::run(window).nested_caches == 0 && window.code_section == before.code_section,
            "Different-CBR CACHE was removed merely because its PC was inside the old 512-byte window");
        DiscoGSU::Machine a(linked(before), 0x008000), b(linked(window), 0x008000); a.run(); b.run();
        require(a.cacheBase() == b.cacheBase() && a.cacheRequests() == b.cacheRequests(),
            "CACHE scheduling changed the final effective CBR/window");
    }
    auto selectors = object({0xa5,9,2,0x25,2,0x17,0,1});
    selectors.symbol_table.push_back({GSUCodeLayout::CacheAlignmentSymbol, SymbolSection::CODE, 0});
    const auto selectors_before = selectors;
    require(GSUMachineScheduler::run(selectors).nested_caches == 0 && selectors.code_section == selectors_before.code_section,
        "CACHE selector reset was removed between WITH and TO");
    equivalent(selectors_before, selectors, false);
    for (const auto& bytes : {std::vector<std::uint8_t>{2,0xa0,1,0xa5,7,0xb5,2,0x50,0,1},
            std::vector<std::uint8_t>{2,0xa0,1,0x3e,2,0x50,0,1}}) {
        auto active = object(bytes);
        active.symbol_table.push_back({GSUCodeLayout::CacheAlignmentSymbol, SymbolSection::CODE, 0});
        const auto before = active;
        require(GSUMachineScheduler::run(active).nested_caches == 0 && active.code_section == before.code_section,
            "Same-CBR CACHE lost its active FROM/ALT selector reset");
        equivalent(before, active, false);
        DiscoGSU::Machine machine(linked(active), 0x008000); machine.run();
        require(machine.reg(0) == 2, "CACHE did not clear an active FROM/ALT before ADD R0");
    }

    for (const bool public_entry : {false, true}) {
        auto entry = object({2,1,1,1,2,0,1});
        entry.symbol_table.push_back({GSUCodeLayout::CacheAlignmentSymbol, SymbolSection::CODE, 0});
        entry.symbol_table.push_back({public_entry ? "main" : "\x01private-entry", SymbolSection::CODE, 3});
        const auto before = entry;
        require(GSUMachineScheduler::run(entry).nested_caches == 0 && entry.code_section == before.code_section,
            "CACHE proof crossed a public/private entry that can bypass its predecessor CACHE");
        DiscoGSU::Machine from_entry(std::vector<std::uint8_t>(entry.code_section.begin() + 3, entry.code_section.end()), 0x008003);
        from_entry.run(); require(from_entry.cacheRequests() == 1, "Independent entry lost its only executed CACHE");
    }
    auto call = object({2,0x94,0xff,0,0,1,2,0,1});
    call.code_section.resize(520, 1);
    call.symbol_table.push_back({GSUCodeLayout::CacheAlignmentSymbol, SymbolSection::CODE, 0});
    call.symbol_table.push_back({"\x01" "callee", SymbolSection::CODE, 520});
    call.relocation_table.push_back({"\x01" "callee", SymbolSection::CODE, 2, RelocationType::ADDR16_IWT});
    call.code_section.insert(call.code_section.end(), {2,0x9b,1});
    const auto call_before = call;
    require(GSUMachineScheduler::run(call).nested_caches == 0, "CACHE proof crossed a call that can change CBR");
    equivalent(call_before, call, false);
    DiscoGSU::Machine caller(linked(call), 0x008000); caller.run();
    require(caller.cacheRequests() == 3 && caller.cacheBase() == 0x8000, "Call continuation did not restore its CACHE window");
}

// Two alignment (1) and two null (2) guards. Each later guard can share the
// other's island, so one island per category remains; the earlier guards
// branch to it and take their continuation opcode into the delay slot.
std::vector<std::uint8_t> guardedFixture(std::size_t gap = 0, bool fenced = false) {
    std::vector<std::uint8_t> bytes{0xf0, 0, 0};
    const auto alignment = [&](std::uint8_t next) {
        bytes.insert(bytes.end(), {0xb0, 0x13, 0x03, 0x0c, 5, 1, 0xa6, 1, 0, 1, next});
    };
    const auto null = [&](std::uint8_t next) { bytes.insert(bytes.end(), {0x23, 0xb0, 0x08, 5, 1, 0xa6, 2, 0, 1, next}); };
    alignment(0xd5); null(0xd7);
    bytes.insert(bytes.end(), gap, 1);
    if (fenced) bytes.push_back(2); // A public function-entry CACHE: layout may pad before it.
    alignment(0xd5); null(0xd7);
    bytes.insert(bytes.end(), {0, 1});
    return bytes;
}
void sharedFaultStubs() {
    for (const unsigned value : {0u, 1u, 2u, 0x8000u, 0xffffu}) {
        auto o = object(guardedFixture());
        o.code_section[1] = static_cast<std::uint8_t>(value); o.code_section[2] = static_cast<std::uint8_t>(value >> 8);
        const auto before = o;
        const auto stats = GSUMachineScheduler::run(o);
        require(stats.shared_fault_stubs == 2 && stats.guard_slots == 2 && o.code_section.size() + 10 == before.code_section.size(),
            "Duplicate in-range fault islands were not shared, or guard slots were not filled");
        DiscoGSU::Machine a(linked(before), 0x008000), b(linked(o), 0x008000);
        a.run(); b.run();
        const bool fault = value == 0 || (value & 1);
        // The fault contract is the category in R6, the stop, and memory.
        // Register-only continuation opcodes may also run before that STOP.
        require(a.reg(6) == b.reg(6) && a.reg(6) == (value & 1 ? 1u : value == 0 ? 2u : 0u),
            "A shared fault island changed the reported category or its priority");
        require(a.metrics().stores == 0 && b.metrics().stores == 0, "Fault fixture unexpectedly stored");
        if (!fault) for (unsigned r = 0; r < 11; ++r) require(a.reg(r) == b.reg(r), "Shared islands changed a successful path");
        require(Assembler().assemble(AssemblyGenerator(o).generate()).code_section == o.code_section,
            "Shared fault islands lost byte-exact assembly export");
        const auto again = o;
        const auto second = GSUMachineScheduler::run(o);
        require(second.shared_fault_stubs == 0 && second.guard_slots == 0 && o.code_section == again.code_section,
            "Fault-island sharing is not idempotent");
    }
    // Out of signed-byte range, or across a point where layout may pad.
    for (const bool fenced : {false, true}) {
        auto o = object(guardedFixture(fenced ? 0 : 120, fenced));
        if (fenced) o.symbol_table.push_back({"function", SymbolSection::CODE, 24});
        const auto before = o;
        const auto stats = GSUMachineScheduler::run(o);
        require(stats.shared_fault_stubs == 0 && stats.guard_slots == 0 &&
            o.code_section.size() == before.code_section.size() + stats.cache_padding,
            "A fault island was shared out of branch range or across a padded function entry");
    }
    // An island with its own label stays; the other guard may still use it.
    auto labeled = object(guardedFixture());
    labeled.symbol_table.push_back({"\x01island", SymbolSection::CODE, 9});
    const auto stats = GSUMachineScheduler::run(labeled);
    require(stats.shared_fault_stubs == 2 && labeled.symbol_table[0].offset == 9 && labeled.code_section[9] == 0xa6,
        "A labeled fault island was removed or moved");
}
void jumpSlots() {
    auto o = object({0xf0,5,0, 5,3, 1, 0xd1,0, 0xd0, 0xd0, 0,1});
    const auto before = o;
    const auto stats = GSUMachineScheduler::run(o);
    require(stats.jump_slots == 1 && o.code_section.size() == before.code_section.size() &&
        o.code_section[4] == 4 && o.code_section[5] == 0xd0, "BRA slot did not copy its target opcode and skip it");
    equivalent(before, o);
    // CACHE depends on its own PC, STOP ends the program, and a transfer cannot
    // occupy another transfer's slot.
    for (const std::uint8_t opcode : {std::uint8_t{2}, std::uint8_t{0}, std::uint8_t{0x9b}, std::uint8_t{0x3c}}) {
        auto unsafe = object({0xf0,5,0, 5,3, 1, 0xd1,0, opcode, 0xd0, 0,1});
        const auto saved = unsafe;
        require(GSUMachineScheduler::run(unsafe).jump_slots == 0 && unsafe.code_section == saved.code_section,
            "A position-dependent or control opcode was copied into a BRA slot");
    }
    // A pending WITH would reach the copy in the slot, not the NOP-reset target.
    auto prefixed = object({0xf0,5,0, 0x21, 5,3, 1, 0xd1, 0xd0, 0x10, 0,1});
    require(GSUMachineScheduler::run(prefixed).jump_slots == 0, "A BRA with live selectors received a copied opcode");
}

// Observes C, Z and S after the fixture's operation in R7/R8/R9, so the
// register comparison in equivalent() also covers the condition codes.
void appendFlagProbe(std::vector<std::uint8_t>& bytes) {
    bytes.insert(bytes.end(), {0x0d,2,1,0xd7, 0x09,2,1,0xd8, 0x0b,2,1,0xd9, 0,1});
}
void copyPeepholes() {
    // MOVE whose destination the next instruction overwrites unread.
    for (const auto& overwrite : std::vector<std::vector<std::uint8_t>>{
             {0x15,0x40}, {0x15,0x3d,0x40}, {0xa5,7}, {0xf5,0x34,0x12}}) {
        std::vector<std::uint8_t> bytes{0xf1,0x34,0x12, 0xf0,0x00,0x90, 0x21,0x15};
        bytes.insert(bytes.end(), overwrite.begin(), overwrite.end());
        bytes.insert(bytes.end(), {0,1});
        auto o = object(bytes); const auto before = o;
        require(GSUMachineScheduler::run(o).dead_copies == 1 && o.code_section.size() + 2 == before.code_section.size(),
            "Overwritten MOVE destination kept its dead copy");
        equivalent(before, o);
    }
    // The load reads the copied register as its address: the copy is live.
    auto address = object({0xf1,0x00,0x90, 0xa0,0, 0x21,0x15, 0x10,0x45, 0,1}); auto saved = address;
    require(GSUMachineScheduler::run(address).dead_copies == 0, "A MOVE feeding the load address was removed");
    equivalent(saved, address);
    // A different destination, an intervening reader and a named overwrite keep it.
    auto other = object({0xf1,0x34,0x12, 0x21,0x15, 0xa7,7, 0,1});
    require(GSUMachineScheduler::run(other).dead_copies == 0, "A MOVE was removed before an unrelated literal");
    auto entered = object({0xf1,0x34,0x12, 0x21,0x15, 0xa5,7, 0,1});
    entered.symbol_table.push_back({"entry",SymbolSection::CODE,5});
    require(GSUMachineScheduler::run(entered).dead_copies == 0, "A MOVE was removed before a separately entered overwrite");

    // MOVE Ra,R0 + accumulator operation -> FROM Ra + operation.
    struct Case { std::vector<std::uint8_t> operation; bool rewritten; };
    const Case cases[] = {
        {{0x3e,0x58},true}, {{0x52},true}, {{0x3d,0x52},true}, {{0x62},true}, {{0x3e,0x63},true},
        {{0x72},true}, {{0x3e,0x7f},true}, {{0xc2},true}, {{0x3d,0xc2},true}, {{0x82},true},
        {{0x03},true}, {{0x04},true}, {{0x4d},true}, {{0x4f},true}, {{0x95},true}, {{0x96},true},
        {{0x3d,0x96},true}, {{0x97},true}, {{0x9e},true}, {{0xc0},true},
        {{0x50},false},      // ADD R0 reads the copied accumulator.
        {{0x3f,0x62},false}, // CMP leaves the copy in R0.
        {{0x12,0x52},false}, // Another destination: R0 keeps the copy.
        {{0xd0},false}};     // INC R0 is not a source-selected operation.
    for (const auto& item : cases) for (const auto value : {0x0000u,0x0001u,0x7fffu,0x8000u,0xffffu,0x1234u}) {
        std::vector<std::uint8_t> bytes{0xf1,static_cast<std::uint8_t>(value),static_cast<std::uint8_t>(value >> 8),
            0xf2,0xcd,0xab, 0xf0,0x55,0xaa, 0x21,0x10};
        bytes.insert(bytes.end(), item.operation.begin(), item.operation.end());
        appendFlagProbe(bytes);
        auto o = object(bytes); const auto before = o;
        const auto stats = GSUMachineScheduler::run(o);
        require((stats.accumulator_sources == 1) == item.rewritten, "Accumulator source rewrite crossed its operand rules");
        equivalent(before, o, false);
    }
    // A private anchor nothing refers to is not an entry; a public symbol is.
    auto anchored = object({0xf1,0x34,0x12, 0x21,0x10, 0x3e,0x58, 0,1});
    anchored.symbol_table.push_back({std::string("\x01") + "anchor",SymbolSection::CODE,3}); saved = anchored;
    require(GSUMachineScheduler::run(anchored).accumulator_sources == 1 && anchored.symbol_table[0].offset == 3 &&
        anchored.code_section[3] == 0xb1, "An unreferenced private anchor blocked or lost the rewrite");
    equivalent(saved, anchored, false);
    auto exported = object({0xf1,0x34,0x12, 0x21,0x10, 0x3e,0x58, 0,1});
    exported.symbol_table.push_back({"entry",SymbolSection::CODE,3});
    require(GSUMachineScheduler::run(exported).accumulator_sources == 0, "A public entry's selector state was assumed");
    auto split = object({0xf1,0x34,0x12, 0x21,0x10, 0x3e,0x58, 0,1});
    split.symbol_table.push_back({std::string("\x01") + "inner",SymbolSection::CODE,5});
    require(GSUMachineScheduler::run(split).accumulator_sources == 0, "A rewrite erased a named instruction boundary");

    // FROM Rx / TO Rx (either order) + operation -> WITH Rx + operation.
    for (const auto& pair : std::vector<std::vector<std::uint8_t>>{{0xb1,0x11},{0x11,0xb1}})
        for (const auto& operation : std::vector<std::vector<std::uint8_t>>{{0x51},{0x3d,0x51},{0x62},{0x3e,0x53},{0x03},{0x4d}})
            for (const auto value : {0x0001u,0x7fffu,0x8000u,0xffffu}) {
                std::vector<std::uint8_t> bytes{0xf1,static_cast<std::uint8_t>(value),static_cast<std::uint8_t>(value >> 8),
                    0xf2,0xcd,0xab};
                bytes.insert(bytes.end(), pair.begin(), pair.end());
                bytes.insert(bytes.end(), operation.begin(), operation.end());
                appendFlagProbe(bytes);
                auto o = object(bytes); const auto before = o;
                require(GSUMachineScheduler::run(o).paired_selectors == 1 && o.code_section[6] == 0x21,
                    "A FROM/TO pair on one register was not folded into WITH");
                equivalent(before, o, false);
            }
    // WITH's B flag would turn a following TO/FROM into MOVE/MOVES.
    for (const std::uint8_t follower : {std::uint8_t{0x12}, std::uint8_t{0xb2}}) {
        auto chained = object({0xf1,0x34,0x12, 0xf2,0xcd,0xab, 0xb1,0x11,follower,0x52, 0,1}); saved = chained;
        require(GSUMachineScheduler::run(chained).paired_selectors == 0, "WITH was introduced before another selector");
        equivalent(saved, chained, false);
    }
    auto distinct = object({0xf1,0x34,0x12, 0xb1,0x12,0x51, 0,1});
    require(GSUMachineScheduler::run(distinct).paired_selectors == 0, "Different source/destination registers were folded");
}

// WITH Rx / op / MOVE Rx,R0 -> FROM Rx / op needs Rx provably dead.
void accumulatorResults() {
    for (const auto& operation : std::vector<std::vector<std::uint8_t>>{
             {0x51},{0x3d,0x51},{0x61},{0x3e,0x53},{0x3e,0x63},{0x71},{0x3d,0xc1},{0x50},{0x4f},{0x03},{0x4d},{0x96}})
        for (const auto value : {0x0000u,0x0001u,0x7fffu,0x8000u,0xffffu,0x1234u}) {
            std::vector<std::uint8_t> bytes{0xf5,static_cast<std::uint8_t>(value),static_cast<std::uint8_t>(value >> 8),
                0xf1,0xcd,0xab, 0xf0,0x55,0xaa, 0x25};
            bytes.insert(bytes.end(), operation.begin(), operation.end());
            bytes.insert(bytes.end(), {0x25,0x10, 0xa5,3}); // MOVE R5,R0, then R5 is overwritten.
            appendFlagProbe(bytes);
            auto o = object(bytes); const auto before = o;
            require(GSUMachineScheduler::run(o).accumulator_results == 1 && o.code_section.size() + 2 == before.code_section.size(),
                "A result computed in a dead home was not delivered straight to R0");
            equivalent(before, o, false);
        }
    // Still read, compared rather than written, never overwritten before STOP,
    // or overwritten only past a real entry: the home stays authoritative.
    const std::vector<std::vector<std::uint8_t>> kept = {
        {0xf5,0x34,0x12, 0xf1,0xcd,0xab, 0x25,0x51, 0x25,0x10, 0x25,0x16, 0xa5,3, 0,1},
        {0xf5,0x34,0x12, 0xf1,0xcd,0xab, 0x25,0x3f,0x61, 0x25,0x10, 0xa5,3, 0,1},
        {0xf5,0x34,0x12, 0xf1,0xcd,0xab, 0x25,0x51, 0x25,0x10, 0,1},
        {0xf5,0x34,0x12, 0xf1,0xcd,0xab, 0x25,0x51, 0x25,0x10, 0x16,0x45, 0xa5,3, 0,1}}; // LDW (R5)
    for (const auto& bytes : kept) {
        auto o = object(bytes); const auto before = o;
        require(GSUMachineScheduler::run(o).accumulator_results == 0, "A live or unproved result home was bypassed");
        equivalent(before, o);
    }
    auto entered = object({0xf5,0x34,0x12, 0xf1,0xcd,0xab, 0x25,0x51, 0x25,0x10, 0xa5,3, 0,1});
    entered.symbol_table.push_back({"entry",SymbolSection::CODE,10});
    require(GSUMachineScheduler::run(entered).accumulator_results == 0, "Liveness was assumed across a public entry");
    // A return ends R5-R8 and R3; R1/R2 are the private divmod results.
    for (const std::uint8_t home : {std::uint8_t{5}, std::uint8_t{7}, std::uint8_t{8}, std::uint8_t{2}, std::uint8_t{4}}) {
        auto o = object({0xfb,0,0, static_cast<std::uint8_t>(0xf0 | home),0x34,0x12, 0xf1,0xcd,0xab,
            static_cast<std::uint8_t>(0x20 | home),0x51, static_cast<std::uint8_t>(0x20 | home),0x10, 0x9b,1, 0,1});
        o.symbol_table.push_back({"exit",SymbolSection::CODE,15});
        o.relocation_table.push_back({"exit",SymbolSection::CODE,0,RelocationType::ADDR16_IWT});
        const auto before = o;
        const bool caller_dead = home == 5 || home == 7 || home == 8;
        require((GSUMachineScheduler::run(o).accumulator_results == 1) == caller_dead,
            "Return liveness crossed the result/preserved register set");
        DiscoGSU::Machine a(linked(before), 0x008000), b(linked(o), 0x008000);
        a.run(); b.run();
        require(a.reg(0) == b.reg(0) && a.reg(0) == static_cast<std::uint16_t>(0x1234 + 0xabcd) && a.reg(1) == b.reg(1),
            "Returning a result through R0 changed the returned value");
        if (!caller_dead) require(a.reg(home) == b.reg(home), "A register live at return was changed");
    }
}

int main() {
    try {
        sharedFaultStubs(); jumpSlots(); copyPeepholes(); accumulatorResults();
        conditionSlots(); targetPrefixSlots(); compactBranches(); shortJumps(); privatePixelSlots(); pipelinePlan(); nestedCacheDelaySlots(); cacheProvenance();
        auto bra = object({0xf0,0,0,0xd0,5,2,1,1,0,1}); auto before = bra;
        require(GSUMachineScheduler::run(bra).delay_slots == 1 && bra.code_section.size() == 9,
                "Safe one-byte BRA delay slot was not filled"); equivalent(before, bra);
        auto copy = object({0xf0,149,0,0x20,0x17,5,2,1,1,0,1}); before = copy;
        require(GSUMachineScheduler::run(copy).delay_slots == 1 && copy.code_section[3] == 0x20 &&
                copy.code_section[6] == 0x17, "BRA did not retain WITH's prefix until the useful TO slot"); equivalent(before, copy);
        auto alu = object({0xf7,0,0,0x27,0x3e,0x51,5,2,1,1,0,1}); before = alu;
        require(GSUMachineScheduler::run(alu).delay_slots == 1 && alu.code_section[7] == 0x51,
                "BRA lost the selected-register immediate arithmetic in its slot"); equivalent(before, alu);
        auto guard = object({0xfa,0,0x20,0xea,0x0c,5,1,0xff,0,0,1,0,1});
        guard.symbol_table.push_back({"exit",SymbolSection::CODE,11});
        guard.relocation_table.push_back({"exit",SymbolSection::CODE,7,RelocationType::ADDR16_IWT}); before = guard;
        const auto guard_stats = GSUMachineScheduler::run(guard);
        require(guard_stats.delay_slots == 1 && guard_stats.short_jumps == 1 && guard.code_section[6] == 5,
                "A filled conditional slot was moved again into the following jump's slot"); equivalent(before, guard);
        for (auto opcode : {8,9,10,11,6,7}) {
            auto unsafe = object({0xf0,0,0,0xd0,static_cast<std::uint8_t>(opcode),2,1,1,0,1}); before = unsafe;
            require(GSUMachineScheduler::run(unsafe).delay_slots == 0 && unsafe.code_section == before.code_section,
                    "Branch was moved before its N/Z producer");
        }
        auto entry = object({0xf0,0,0,0xd0,5,2,1,1,0,1});
        entry.symbol_table.push_back({"alternate_entry",SymbolSection::CODE,4});
        require(GSUMachineScheduler::run(entry).delay_slots == 0, "Delay scheduling crossed a control-flow entry");
        auto immediate = object({0xf0,0xd0,1,5,2,1,1,0,1}); before = immediate;
        require(GSUMachineScheduler::run(immediate).delay_slots == 0 && immediate.code_section == before.code_section,
                "Operand bytes were mistaken for opcodes");
        auto call = object({0xfa,0,0x20,0xea,0xea,0x94,0xff,0,0,1,0,1,0xf0,149,0,0x9b,1}); before = call;
        call.symbol_table.push_back({"callee",SymbolSection::CODE,12});
        call.relocation_table.push_back({"callee",SymbolSection::CODE,6,RelocationType::ADDR16_JAL}); before = call;
        require(GSUMachineScheduler::run(call).delay_slots == 1 && call.relocation_table[0].patch_offset == 5 &&
                call.symbol_table[0].offset == 11, "Call delay fill did not relocate CODE consistently"); equivalent(before, call);
        DiscoGSU::Machine calls(linked(call),0x008000); calls.run();
        require(calls.reg(0) == 149 && calls.reg(10) == 0x1ffe && calls.reg(11) == 0x8009,
                "LINK #4 / useful slot changed the return address or stack");
        auto pixels = object({0xfc,3,0,0xfd,0,0,0xa0,3,0x4e,1,0x4c,0x3c,1,0x3d,0x4c,0,1});
        pixels.symbol_table.push_back({"body",SymbolSection::CODE,9});
        pixels.relocation_table.push_back({"body",SymbolSection::CODE,3,RelocationType::ADDR16_IWT}); before = pixels;
        require(GSUMachineScheduler::run(pixels).delay_slots == 1 && pixels.code_section[10] == 0x3c &&
                pixels.code_section[11] == 0x4c, "PLOT did not execute in LOOP's taken/final slot"); equivalent(before,pixels);
        DiscoGSU::Machine plot_loop(linked(pixels),0x008000); plot_loop.run();
        require(plot_loop.reg(1) == 3 && plot_loop.reg(12) == 0 && plot_loop.graphicsState("--plots") == 3,
                "LOOP's useful PLOT slot lost its final iteration or incremented R1 twice");
        for (const unsigned counter : {0u, 1u, 65535u}) {
            auto boundary = before;
            boundary.code_section[1] = static_cast<std::uint8_t>(counter);
            boundary.code_section[2] = static_cast<std::uint8_t>(counter >> 8);
            const auto original = boundary;
            require(GSUMachineScheduler::run(boundary).delay_slots == 1, "Boundary LOOP lost its useful PLOT slot");
            equivalent(original, boundary);
            DiscoGSU::Machine executed(linked(boundary), 0x008000); executed.run();
            require(executed.graphicsState("--plots") == (counter == 0 ? 65536u : counter) &&
                    executed.reg(1) == counter && executed.reg(12) == 0,
                "Scheduled LOOP/PLOT lost the taken/final iteration or raw zero-count wrap");
        }
        auto alt = object({0x3e,0xd0,0xff,0,0x80,1,0,1}); before = alt;
        require(GSUMachineScheduler::run(alt).delay_slots == 0 && alt.code_section == before.code_section,
                "Scheduling exposed IWT to a live ALT prefix");
        auto selects = object({0x21,0x3e,0x10,0xdf,0xd0,0,1}); before = selects;
        require(GSUMachineScheduler::run(selects).rom_operations == 0 && selects.code_section == before.code_section,
                "ALT failed to clear WITH's copy prefix or preserve selectors");

        auto color = object({0xfe,0,0x90,0xdf,0xf0,0x34,0x12,0,1}); before = color;
        require(GSUMachineScheduler::run(color).rom_operations == 1 && color.code_section[6] == 0xdf,
                "Independent materialization did not hide GETC latency"); equivalent(before, color);
        auto span = object({0xfe,0,0x90,0xdf,0x4c,0xd7,0x3d,0x4c,0,1}); before = span;
        require(GSUMachineScheduler::run(span).rom_operations == 1 && span.code_section[3] == 0xd7,
                "Independent span counter was not scheduled into ROM latency"); equivalent(before, span);
        auto loop_counter = object({0xfe,0,0x90,0xdf,0x4c,0x27,0x3e,0x51,0x3d,0x4c,0,1}); before = loop_counter;
        require(GSUMachineScheduler::run(loop_counter).rom_operations == 1 && loop_counter.code_section[3] == 0x27,
                "A span's selected-register counter update did not hide GETC latency"); equivalent(before, loop_counter);
        auto cursor = object({0xfe,0,0x90,0xdf,0x4c,0x21,0x17,0,1}); before = cursor;
        require(GSUMachineScheduler::run(cursor).rom_operations == 0 && cursor.code_section == before.code_section,
                "A read of post-PLOT cursor.x moved before its auto-increment");
        auto getb = object({0xfe,0,0x90,0x12,0xef,0xf0,0x34,0x12,0,1}); before = getb;
        require(GSUMachineScheduler::run(getb).rom_operations == 1 && getb.code_section[6] == 0x12,
                "GETB's complete destination prefix was not preserved"); equivalent(before, getb);
        for (const auto& bytes : {std::vector<std::uint8_t>{0xfe,0,0x90,0x12,0xef,0xf2,0x34,0x12,0,1},
                                std::vector<std::uint8_t>{0xfe,0,0x90,0xdf,0x3d,0x41,0,1},
                                std::vector<std::uint8_t>{0xfe,0,0x90,0xdf,0xde,0,1}}) {
            auto unsafe = object(bytes); before = unsafe;
            require(GSUMachineScheduler::run(unsafe).rom_operations == 0 && unsafe.code_section == before.code_section,
                    "ROM scheduling crossed a result dependency, memory access, or R14 write");
        }
        auto labeled = object({0xfe,0,0x90,0xdf,0xd0,0,1}); labeled.symbol_table.push_back({"entry",SymbolSection::CODE,4});
        require(GSUMachineScheduler::run(labeled).rom_operations == 0, "ROM scheduling crossed an entry label");

        auto cache = object({2,0xf0,149,0,0,1}); cache.symbol_table.push_back({"cached",SymbolSection::CODE,0});
        require(GSUMachineScheduler::run(cache).cache_padding == 15 && cache.symbol_table[0].offset == 15 &&
                GSUCodeLayout::alignment(cache) == 16 && cache.code_section[15] == 2,
                "CACHE's next byte was not 16-byte aligned");
        const auto assembly = AssemblyGenerator(cache).generate();
        const auto roundtrip = Assembler().assemble(assembly);
        require(roundtrip.code_section == cache.code_section && GSUCodeLayout::alignment(roundtrip) == 16,
                "Assembly round trip lost CACHE alignment metadata");
        auto local_cache = object({1,2,1,0,1}); before = local_cache;
        require(GSUMachineScheduler::run(local_cache).cache_padding == 0 && local_cache.code_section == before.code_section,
                "A loop/local CACHE was padded without measured justification");
        auto invalid_hint = cache; invalid_hint.symbol_table.back().offset = 1;
        bool rejected = false;
        try { GSUCodeLayout::alignment(invalid_hint); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected,"Malformed CACHE alignment hint was accepted");
        auto limit = object({5,126,1}); limit.code_section.resize(130,1); limit.code_section[128] = 2;
        limit.symbol_table.push_back({"cached",SymbolSection::CODE,128}); before = limit;
        require(GSUMachineScheduler::run(limit).cache_padding == 0 && limit.code_section == before.code_section &&
                GSUCodeLayout::alignment(limit) == 1, "CACHE padding overflowed an already relaxed short branch");
        std::cout << "GSU machine scheduling tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
