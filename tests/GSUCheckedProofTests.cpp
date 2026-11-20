#include "GSUAddressProof.hpp"
#include "GSUStackCheckCredit.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "IRGlobalOptimizer.hpp"
#include "IRCodeGenerator.hpp"
#include "GSUInstructionModel.hpp"
#include "ABI.hpp"
#include "AssemblyGenerator.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Type word(bool unsigned_value = false) { return Type{BaseType::WORD, "", 2, unsigned_value}; }
Type byte() { return Type{BaseType::BYTE, "", 1, true}; }
Type pointer(Type value = word(), AddressSpace space = AddressSpace::RAM, bool far = false) { return pointerTo(value, space, far); }
struct Builder {
    IRFunction function;
    Analyzer::LocalSymbolTable locals;
    GSUAddressProof proof;
    std::size_t current = 0;
    Builder() {
        function.name = "main"; function.return_type = word(); function.entry = IRBlockId{0};
        function.blocks.push_back({IRBlockId{0}, "entry", {}});
    }
    IRValueId add(IROpcode opcode, Type type, std::vector<IRValueId> inputs = {}, std::string operation = {}, std::int64_t immediate = 0) {
        IRInstruction instruction; instruction.opcode = opcode; instruction.type = std::move(type);
        instruction.result = IRValueId{++function.value_count}; instruction.operands = std::move(inputs);
        instruction.operation = std::move(operation); instruction.immediate = immediate;
        function.blocks[current].instructions.push_back(std::move(instruction)); return IRValueId{function.value_count};
    }
    IRValueId literal(std::int64_t value, Type type = word()) { return add(IROpcode::Constant, type, {}, {}, value); }
    IRValueId local(int displacement, Type type = word()) {
        const auto id = add(IROpcode::Address, pointer(type));
        const SymbolId symbol{static_cast<std::uint32_t>(locals.size() + 1)};
        locals.emplace(symbol, Symbol{type, displacement, "", symbol});
        function.blocks[0].instructions.back().symbol_id = symbol; return id;
    }
    IRValueId offset(IRValueId base, IRValueId index, std::string direction = "+", int stride = 2) {
        return add(IROpcode::PointerOffset, pointer(), {base, index}, std::move(direction), stride);
    }
    bool access(IRValueId value, int width = 2, Type type = pointer()) const { return proof.provesAccess(value, type, width); }
    bool step(IRValueId value) const { return proof.provesOffset(function.blocks[0].instructions.at(value.value - 1)); }
    void run(std::size_t frame = 44, bool checked = true) { proof.run(function, locals, frame, checked); }
    void run(const GSUAddressProof::StaticObjects& statics) { proof.run(function, locals, statics, 44, true); }
    IRValueId global(std::string name, Type type = word()) {
        const auto id = add(IROpcode::Address, pointer(type));
        function.blocks[0].instructions.back().symbol = std::move(name); return id;
    }
};
void frameRanges() {
    Builder b;
    const auto table = b.local(-40), input = b.add(IROpcode::Call, word());
    const auto mask = b.literal(15), index = b.add(IROpcode::Binary, word(), {input, mask}, "&");
    const auto sixteen = b.literal(16), mirrored = b.add(IROpcode::Binary, word(), {sixteen, index}, "-");
    const auto phi = b.add(IROpcode::Phi, word(), {index, mirrored});
    const auto selected = b.offset(table, phi);
    const auto unknown = b.offset(table, input);
    const auto center = b.local(-8), backwards = b.offset(center, phi, "-");
    const auto bottom = b.local(-44), last_word = b.local(-2);
    const auto beyond = b.add(IROpcode::Address, pointer(), {last_word}, "member", 65535);
    const auto member = b.add(IROpcode::Address, pointer(), {table}, "member", 32);
    const auto odd = b.add(IROpcode::Address, pointer(), {table}, "member", 1);
    const auto cast = b.add(IROpcode::Cast, pointer(byte()), {table});
    Type aligned = word(); aligned.alignment = 4;
    const auto over_aligned = b.local(-20, aligned);
    const auto temp = b.add(IROpcode::Address, pointer(), {}, "temporary", -4);
    b.run();
    require(b.access(selected) && b.step(selected), "Bounded mask/diamond PHI did not prove a complete word access");
    require(b.access(backwards) && b.step(backwards), "Negative pointer displacement lost its safe frame proof");
    require(!b.access(unknown) && !b.step(unknown), "An unknown runtime index gained a frame proof");
    require(!b.access(bottom), "A frame at stack_floor=0 incorrectly proved the null bottom slot");
    require(b.access(last_word) && b.access(member) && b.access(temp), "Known local/member/temporary access was not proved");
    require(!b.access(beyond) && !b.access(odd), "Member overflow or word misalignment gained a proof");
    require(b.access(cast, 1, pointer(byte())) && !b.access(over_aligned), "Pointer qualifiers/layout alignment were incorrectly inferred");
    b.run(44, false);
    require(!b.access(table) && !b.step(selected), "An unchecked/unaligned entry frame gained a proof");
}
void staticObjectRanges() {
    Builder b;
    const GSUAddressProof::StaticObjects statics{{"table", {0, 16, 2}}, {"other", {1, 16, 2}}, {"bytes", {2, 5, 2}}};
    const auto table = b.global("table"), other = b.global("other"), missing = b.global("missing");
    const auto input = b.add(IROpcode::Call, word());
    const auto seven = b.literal(7), eight = b.literal(8), minus = b.literal(-1), four = b.literal(4), five = b.literal(5);
    const auto mask = b.add(IROpcode::Binary, word(), {input, seven}, "&");
    const auto last = b.offset(table, seven), past = b.offset(table, eight), before = b.offset(table, minus);
    const auto masked = b.offset(table, mask), unknown = b.offset(table, input);
    const auto same = b.add(IROpcode::Phi, pointer(), {last, table});
    const auto mixed = b.add(IROpcode::Phi, pointer(), {table, other});
    const auto odd = b.add(IROpcode::Address, pointer(), {table}, "member", 1);
    const auto bytes = b.global("bytes", byte());
    const auto last_byte = b.add(IROpcode::PointerOffset, pointer(byte()), {bytes, four}, "+", 1);
    const auto past_byte = b.add(IROpcode::PointerOffset, pointer(byte()), {bytes, five}, "+", 1);
    b.run(statics);
    require(!b.proof.usedStaticObjectProof(), "A static-object proof was recorded before any access relied on it");
    require(!b.access(past) && !b.access(before) && !b.access(unknown) && !b.step(unknown) && !b.access(mixed) &&
        !b.access(missing) && !b.access(odd), "An out-of-object, unknown, mixed-object or misaligned access was proved");
    require(!b.access(last_byte, 2, pointer(byte())) && !b.access(past_byte, 1, pointer(byte())),
        "A static access crossing the object's end was proved");
    require(!b.proof.usedStaticObjectProof(), "A rejected static-object query was recorded as relied upon");
    require(b.access(table) && b.access(last) && b.step(last) && b.access(masked) && b.step(masked) && b.access(same) &&
        b.access(last_byte, 1, pointer(byte())), "An in-object static RAM access was not proved");
    require(b.proof.usedStaticObjectProof(), "A static-object proof was not recorded for the linker marker");
    b.run();
    require(!b.access(table) && !b.access(last) && !b.proof.usedStaticObjectProof(),
        "Static RAM gained a proof without the object's static-object table");
}

// ROM objects defined by this object share the static-object table but never
// its RAM marker: their facts come from discld's near-ROM placement checks.
void staticRomObjectRanges() {
    Builder b;
    const GSUAddressProof::StaticObjects statics{{"ram", {0, 8, 2}}, {"palette", {1, 8, 2, AddressSpace::ROM}}};
    const auto rom_byte = pointer(byte(), AddressSpace::ROM), far_rom_byte = pointer(byte(), AddressSpace::ROM, true);
    const auto named = [&](const std::string& name, const Type& type) {
        const auto id = b.add(IROpcode::Address, type); b.function.blocks[0].instructions.back().symbol = name; return id;
    };
    const auto palette = named("palette", rom_byte), as_ram = named("palette", pointer(byte()));
    const auto ram_as_rom = named("ram", rom_byte), far = named("palette", far_rom_byte);
    const auto input = b.add(IROpcode::Call, word());
    const auto seven = b.literal(7), eight = b.literal(8);
    const auto mask = b.add(IROpcode::Binary, word(), {input, seven}, "&");
    const auto masked = b.add(IROpcode::PointerOffset, rom_byte, {palette, mask}, "+", 1);
    const auto past = b.add(IROpcode::PointerOffset, rom_byte, {palette, eight}, "+", 1);
    const auto unknown = b.add(IROpcode::PointerOffset, rom_byte, {palette, input}, "+", 1);
    b.run(statics);
    require(b.access(palette, 1, rom_byte) && b.access(masked, 1, rom_byte) && b.step(masked),
        "An in-object near ROM access was not proved");
    require(!b.access(past, 1, rom_byte) && !b.access(unknown, 1, rom_byte) && !b.step(unknown) &&
        !b.access(masked, 2, rom_byte), "An out-of-object or unknown ROM access was proved");
    require(!b.access(as_ram, 1, pointer(byte())) && !b.access(ram_as_rom, 1, rom_byte) &&
        !b.access(far, 1, far_rom_byte), "A ROM proof crossed address spaces or applied to a far pair");
    require(!b.proof.usedStaticObjectProof(), "A ROM object proof requested the static RAM marker");
}

ObjectFile compileAt(const std::string& source, OptimizationLevel level) {
    Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; const auto module = lowerer.lower(ast);
    CompilerConfig config; config.optimization = level;
    IRCodeGenerator generator(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
    return generator.generate(module);
}

void staticObjectCodegen() {
    const std::string body = " word main() { word seed=*(volatile word*)0x100; table[3]=5; table[seed&7]+=1; return table[3]+table[7]; }";
    for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto defined = compileAt("internal word table[8];" + body, level);
        const auto external = compileAt("extern word table[8];" + body, level);
        require(defined.hasStaticRamProof(), "Proved accesses to a defined static array did not request the linker marker");
        require(!external.hasStaticRamProof(), "An extern object's address was proved from this object's marker");
        require(defined.code_section.size() + 16 < external.code_section.size(),
            "Defined static-array accesses kept their null/alignment/wrap checks");
        // The in-object base stays proved; only the past-the-end offset keeps
        // its runtime wrap guard.
        const auto inside = compileAt("internal word table[8]; word main() { return table[7]; }", level);
        const auto past = compileAt("internal word table[8]; word main() { return table[8]; }", level);
        require(past.code_section.size() > inside.code_section.size(),
            "An access past a static array's end lost its runtime guard");
    }
    require(!compileAt("internal word table[8];" + body, OptimizationLevel::O1).hasStaticRamProof(),
        "O1 emitted a static-object proof marker without global proofs");
}

void pointerBoundaries() {
    Builder b;
    const auto null = b.literal(0, pointer()), first = b.literal(2, pointer()), last = b.literal(65534, pointer());
    const auto odd = b.literal(65535, pointer());
    const auto rom = b.literal(0x8000, pointer(word(), AddressSpace::ROM));
    const auto far = b.literal(0x700100, pointer(word(), AddressSpace::RAM, true));
    const auto high = b.offset(last, b.literal(1));
    const auto low = b.offset(first, b.literal(-2));
    const auto max = b.offset(first, b.literal(65535, word(true)));
    const auto minus = b.offset(b.literal(20, pointer()), b.literal(-3));
    const auto large = b.add(IROpcode::Cast, pointer(Type{BaseType::STRUCT, "Four", 4}), {last});
    const auto load = b.add(IROpcode::LoadIndirect, pointer(), {first});
    const auto reloaded = b.offset(load, b.literal(0));
    b.run();
    require(!b.access(null) && b.access(first) && b.access(last), "Near RAM null/end-of-bank boundary is wrong");
    require(!b.access(odd) && b.access(odd, 1, pointer(byte())), "Byte/word alignment at $FFFF is wrong");
    require(!b.access(rom, 2, pointer(word(), AddressSpace::ROM)) && !b.access(far, 2, pointer(word(), AddressSpace::RAM, true)), "ROM/far bank checks were discarded");
    require(!b.step(high) && !b.step(low) && !b.step(max) && !b.access(large, 4), "Pointer wrap or a wider access gained a proof");
    require(b.step(minus), "Signed negative offset with an in-bank result was not proved");
    require(!b.step(reloaded), "A memory-loaded pointer gained constant/proven provenance");
    require(!b.proof.provesAccess(IRValueId{1000000}, pointer(), 2) && !b.access(first, 0), "Invalid proof query was accepted");
}
void wrappedAndCyclicValues() {
    Builder b;
    const auto base = b.local(-40), input = b.add(IROpcode::Call, word());
    const auto one = b.literal(1), zero = b.literal(0);
    const auto index = b.add(IROpcode::Phi, word(), {zero, IRValueId{6}});
    const auto next = b.add(IROpcode::Binary, word(), {index, one}, "+");
    require(next.value == 6, "Loop PHI test value IDs drifted");
    const auto cyclic = b.offset(base, index);
    const auto overflow = b.add(IROpcode::Binary, word(), {input, one}, "+");
    const auto wrapping = b.offset(base, overflow);
    const auto narrowed = b.add(IROpcode::Cast, Type{BaseType::BYTE, "", 1, false}, {input});
    const auto narrow_step = b.offset(base, narrowed);
    const auto a = b.local(-20), pointer_phi = b.add(IROpcode::Phi, pointer(), {a, IRValueId{14}});
    const auto advance = b.offset(pointer_phi, one);
    require(advance.value == 14, "Pointer loop test value IDs drifted");
    const auto bit = b.add(IROpcode::BitExtract, word(), {input}, {}, 9), bounded = b.offset(base, bit);
    b.run();
    require(!b.step(cyclic) && !b.step(wrapping) && !b.step(narrow_step) && !b.access(advance), "First-trip/wrapped values unsafely justified an address check");
    require(b.step(bounded), "Bit extraction's 0/1 range was not recognized");
    b.function.value_count = 100001; b.run();
    require(!b.access(base), "Resource-cap fallback retained stale function facts");
    b.function.value_count = 16; b.function.blocks[0].instructions[0].operands = {IRValueId{999999}}; b.run();
    require(!b.access(base), "Malformed operand ID retained stale proofs");
}
void inductionIntervals() {
    const auto check = [](std::int64_t begin, std::int64_t end, std::int64_t stride,
                          bool unsigned_value, const std::string& comparison, bool expected) {
        Builder b; const auto integer = word(unsigned_value);
        const auto initial = b.literal(begin, integer), bound = b.literal(end, integer), step = b.literal(stride, integer);
        IRInstruction entry; entry.opcode = IROpcode::Branch; entry.targets = {IRBlockId{1}};
        b.function.blocks[0].instructions.push_back(entry);
        b.function.blocks.push_back({IRBlockId{1}, "loop.header", {}});
        b.function.blocks.push_back({IRBlockId{2}, "loop.body", {}});
        b.function.blocks.push_back({IRBlockId{3}, "loop.exit", {}});
        b.current = 1;
        const auto induction = b.add(IROpcode::Phi, integer, {initial, initial});
        b.function.blocks[1].instructions.back().targets = {IRBlockId{0}, IRBlockId{2}};
        const auto condition = b.add(IROpcode::Binary, Type{BaseType::BOOL, "", 1, false}, {induction, bound}, comparison);
        IRInstruction guard; guard.opcode = IROpcode::CondBranch; guard.operands = {condition};
        guard.targets = {IRBlockId{2}, IRBlockId{3}}; b.function.blocks[1].instructions.push_back(guard);
        b.current = 2;
        const auto doubled = b.add(IROpcode::Binary, integer, {induction, b.literal(2, integer)}, "*");
        const auto address = b.add(IROpcode::Cast, pointer(), {doubled});
        const auto next = b.add(IROpcode::Binary, integer, {induction, step}, "+");
        entry.targets = {IRBlockId{1}}; b.function.blocks[2].instructions.push_back(entry);
        b.function.blocks[1].instructions[0].operands[1] = next;
        b.run();
        require(b.access(address) == expected, "Canonical induction interval ignored bounds, sign, zero-trip or wrap safety");
        return b;
    };
    check(2, 112, 1, false, "<", true);
    check(112, 2, -1, false, ">", true);
    check(2, 114, 2, false, "<", true);
    check(2, 2, 1, false, "<", true); // Body is dead, but the initial value is still valid.
    check(0, 112, 1, false, "<", false);
    check(32760, 32767, 1, false, "<=", false);
    check(65530, 65535, 1, true, "<=", false);
    check(2, 112, -1, false, "<", false);

    auto counted = check(2, 112, 1, false, "<", true);
    counted.current = 0; const auto trips = counted.literal(110, word(true));
    auto& pre = counted.function.blocks[0].instructions;
    std::swap(pre[pre.size() - 2], pre.back());
    auto& setup = pre.back(); setup.opcode = IROpcode::HardwareLoop; setup.operands = {trips};
    setup.targets = {IRBlockId{1}}; setup.loop_target = IRBlockId{1}; setup.loop_id = 1; setup.compiler_generated_loop = true;
    auto& header = counted.function.blocks[1].instructions;
    header.back().opcode = IROpcode::Branch; header.back().operands.clear(); header.back().targets = {IRBlockId{2}};
    auto& finish = counted.function.blocks[2].instructions.back();
    finish.opcode = IROpcode::HardwareLoopEnd; finish.targets = {IRBlockId{1}, IRBlockId{3}}; finish.loop_id = 1;
    counted.run();
    IRValueId address;
    for (const auto& i : counted.function.blocks[2].instructions) if (i.opcode == IROpcode::Cast) address = i.result;
    require(counted.access(address), "Counted hardware LOOP lost the retained scalar induction's range");
    pre[pre.size() - 2].immediate = 65535; counted.run();
    require(!counted.access(address), "A hardware LOOP with a wrapping induction gained an address proof");
    pre[pre.size() - 2].immediate = 110;
    counted.function.blocks[0].instructions.back().targets = {IRBlockId{4}};
    counted.function.blocks[0].instructions.back().loop_target = IRBlockId{4};
    counted.function.blocks[1].instructions[0].targets = {IRBlockId{4}, IRBlockId{5}};
    counted.function.blocks[2].instructions.back().targets[0] = IRBlockId{5};
    IRInstruction edge; edge.opcode = IROpcode::Branch; edge.targets = {IRBlockId{1}};
    counted.function.blocks.push_back({IRBlockId{4}, "phi.entry.edge", {edge}});
    counted.function.blocks.push_back({IRBlockId{5}, "phi.back.edge", {edge}});
    counted.run();
    require(counted.access(address), "Split-PHI entry/backedges lost a constant hardware LOOP range");
    IRInstruction nonempty; nonempty.opcode = IROpcode::Constant; nonempty.type = word();
    nonempty.result = IRValueId{++counted.function.value_count};
    counted.function.blocks[4].instructions.insert(counted.function.blocks[4].instructions.begin(), nonempty);
    counted.run();
    require(!counted.access(address), "Hardware LOOP range skipped a nontransparent value-defining edge");
}

void boundedCastsAndConditions() {
    Builder b;
    const auto unknown = b.add(IROpcode::Call, word());
    const auto mask = b.literal(7), index = b.add(IROpcode::Binary, word(), {unknown, mask}, "&");
    const auto aligned = b.add(IROpcode::Binary, word(), {index, b.literal(2)}, "*");
    const auto nonnull = b.add(IROpcode::Binary, word(), {aligned, b.literal(0x0200)}, "+");
    const auto selected = b.add(IROpcode::Cast, pointer(), {nonnull});
    const auto odd = b.add(IROpcode::Cast, pointer(), {b.add(IROpcode::Binary, word(), {nonnull, b.literal(1)}, "+")});
    const auto nullable = b.add(IROpcode::Cast, pointer(), {aligned});
    const auto unknown_pointer = b.add(IROpcode::Cast, pointer(), {unknown});
    b.run();
    require(b.access(selected), "A nonnull bounded even integer range did not become a near RAM proof");
    require(!b.access(odd) && !b.access(nullable) && !b.access(unknown_pointer), "Odd, null or unknown integer-to-pointer casts gained a proof");

    const std::string text = "word checked(word seed) { if (seed >= 2) { if (seed <= 100) { return *((word*)(unsigned word)(seed * 2)); } } return 0; }";
    Lexer lexer(text); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; const auto module = lowerer.lower(ast);
    const auto& function = module.functions[0]; GSUAddressProof proof;
    proof.run(function, analyzer.getAllLocalSymbols().at(function.name), static_cast<std::size_t>(function.total_local_alloc_size), true);
    // Frontend locals must first be SSA-promoted for the guards and access to
    // refer to the same resolved value. The optimized form is checked below.
    auto optimized = module; IRGlobalOptimizer::run(optimized, analyzer.getAllLocalSymbols());
    const auto& f = optimized.functions[0]; proof.run(f, analyzer.getAllLocalSymbols().at(f.name), static_cast<std::size_t>(f.total_local_alloc_size), true);
    unsigned reads = 0;
    for (const auto& block : f.blocks) for (const auto& i : block.instructions) if (i.opcode == IROpcode::LoadIndirect) {
        ++reads; require(proof.provesAccess(i.operands[0], pointer(), 2), "Dominating lower/upper guards did not constrain the computed RAM address");
    }
    require(reads != 0, "Conditional interval test no longer performs its guarded RAM read");
}
void stackCredit() {
    GSUStackCheckCredit credit;
    require(!credit.covers(2), "Unknown stack state covered a push");
    credit.checked(12); credit.decrease(2); credit.decrease(2);
    require(credit.covers(8), "The prologue's frame guard did not cover its two register saves");
    credit.decrease(8); require(!credit.covers(2), "Frame allocation retained consumed credit");
    credit.checked(2); credit.decrease(2); credit.increase(2);
    require(credit.covers(2) && !credit.covers(4), "Paired stack traffic lost or overstated its lower bound");
    credit.reset(); require(!credit.covers(2), "A CFG join retained predecessor-specific stack credit");
    credit.checked(65535); credit.increase(std::numeric_limits<std::size_t>::max());
    require(credit.covers(65535) && !credit.covers(65536), "Stack credit overflowed its bank bound");
    credit.decrease(std::numeric_limits<std::size_t>::max()); require(!credit.covers(1), "Stack credit underflowed");
}
void verifiedSource() {
    const std::string text = "word choose(word seed) { word values[17]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17}; word i=seed&15; if(seed&16) i=16-i; return values[i]; }";
    Lexer lexer(text); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; auto module = lowerer.lower(ast);
    IRGlobalOptimizer::run(module, analyzer.getAllLocalSymbols()); IRVerifier::verify(module);
    GSUAddressProof proof; const auto& f = module.functions[0];
    proof.run(f, analyzer.getAllLocalSymbols().at(f.name), static_cast<std::size_t>(f.total_local_alloc_size), true);
    unsigned offsets = 0;
    for (const auto& block : f.blocks) for (const auto& i : block.instructions)
        if (i.opcode == IROpcode::PointerOffset) { ++offsets; require(proof.provesOffset(i), "Analyzed array/mask/PHI combination was not proved"); }
    require(offsets != 0, "Verified source no longer exercises the bounded array proof");
}
void verifiedClearLoop() {
    const std::string text = "word clear() { for(word column=9;column<=22;column++) { unsigned word address=(unsigned word)(column*384+80); word* tiles=(word*)address; @cache for(word i=0;i<112;i++) tiles[i]=0; } word values[3]={2,3,4}; word total=0; for(word edge=0;edge<3;edge++) total+=values[edge]; return total; }";
    Lexer lexer(text); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; auto module = lowerer.lower(ast);
    IRGlobalOptimizer::run(module, analyzer.getAllLocalSymbols()); IRVerifier::verify(module);
    const auto& f = module.functions[0]; GSUAddressProof proof;
    proof.run(f, analyzer.getAllLocalSymbols().at(f.name), static_cast<std::size_t>(f.total_local_alloc_size), true);
    unsigned offsets = 0;
    for (const auto& block : f.blocks) for (const auto& i : block.instructions) if (i.opcode == IROpcode::PointerOffset) {
        ++offsets; require(proof.provesOffset(i), "Nested bounded clear/array loop retained an unnecessary hardware-address guard");
    }
    require(offsets >= 2, "Clear-loop fixture no longer exercises both absolute RAM and frame-backed arrays");
}

// Static RAM (e.g. compiled frames) is placed at `ram_base`, below the floor.
// DATA follows CODE at an even address, as discld places it in one bank.
std::vector<std::uint8_t> executable(const ObjectFile& object, std::uint16_t stack_floor = 0, std::uint16_t ram_base = 0x0200) {
    auto bytes = object.code_section;
    if (!object.data_section.empty() && (bytes.size() & 1u)) bytes.push_back(1);
    const auto data_base = static_cast<std::uint32_t>(0x8000 + bytes.size());
    bytes.insert(bytes.end(), object.data_section.begin(), object.data_section.end());
    require(bytes.size() <= 0x8000, "Checked fixture payload exceeds its LoROM bank");
    if (!object.ram_section.empty())
        require(ram_base && ram_base + object.ram_section.size() <= stack_floor, "Checked fixture static RAM overlaps its stack");
    for (const auto& r : object.relocation_table) {
        std::uint32_t address = 0; bool found = false;
        const std::string prefix = GSUAbi::StackLimitPrefix;
        if (r.target_symbol_name.compare(0,prefix.size(),prefix) == 0) {
            address = static_cast<std::uint32_t>(std::stoul(r.target_symbol_name.substr(prefix.size())));
            require(address <= 65535u - stack_floor, "Checked fixture stack limit exceeds one bank");
            address += stack_floor; found = true;
        } else if (r.target_symbol_name == GSUAbi::NearRamBankSymbol) { address = 0x70; found = true; }
        else if (r.target_symbol_name == GSUAbi::NearRomBankSymbol) { address = 0; found = true; }
        for (const auto& s : object.symbol_table) if (s.name == r.target_symbol_name && s.section == SymbolSection::CODE) {
            address = 0x8000 + s.offset; found = true;
        }
        for (const auto& s : object.symbol_table) if (s.name == r.target_symbol_name && s.section == SymbolSection::DATA &&
            r.type == RelocationType::ADDR16_IWT) {
            address = data_base + s.offset; found = true;
        }
        for (const auto& s : object.symbol_table) if (s.name == r.target_symbol_name && s.section == SymbolSection::RAM &&
            r.type == RelocationType::ADDR16_RAM) {
            address = ram_base + s.offset; found = true;
        }
        // Short RAM sits at the linker's fixed window start, like discld.
        for (const auto& s : object.symbol_table) if (s.name == r.target_symbol_name && s.section == SymbolSection::SHORT_RAM &&
            (r.type == RelocationType::ADDR16_RAM || r.type == RelocationType::ADDR8_SHORT_RAM)) {
            address = GSUAbi::ShortRamBase + s.offset; found = true;
        }
        require(found, "Missing checked-offset fixture relocation");
        if (r.type == RelocationType::ADDR8_SHORT_RAM) {
            require(address % 2 == 0 && address < GSUAbi::ShortRamEnd, "Short RAM fixture slot is outside the window");
            bytes.at(r.patch_offset + 1) = static_cast<std::uint8_t>(address / 2);
            continue;
        }
        bytes.at(r.patch_offset + 1) = static_cast<std::uint8_t>(address);
        if (r.type != RelocationType::ADDR24_BANK) bytes.at(r.patch_offset + 2) = static_cast<std::uint8_t>(address >> 8);
    }
    return bytes;
}

unsigned entryStackBytes(const ObjectFile& object) {
    const std::string prefix = GSUAbi::StackLimitPrefix;
    for (const auto& r : object.relocation_table) {
        if (r.target_symbol_name.compare(0, prefix.size(), prefix) == 0) {
            const auto bytes = std::stoul(r.target_symbol_name.substr(prefix.size()));
            require(bytes >= 4 && bytes < 128, "Stack-boundary fixture lost its bounded entry frame");
            return static_cast<unsigned>(bytes);
        }
    }
    throw std::runtime_error("Stack-boundary fixture has no entry capacity guard");
}

void seedWord(DiscoGSU::Machine& machine, std::uint32_t address, std::uint16_t value) {
    machine.seed(address, static_cast<std::uint8_t>(value), false);
    machine.seed(address + 1, static_cast<std::uint8_t>(value >> 8), false);
}

ObjectFile compileStackProof(const std::string& source, OptimizationLevel level, bool proof, std::uint16_t short_ram = 0) {
    Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; const auto module = lowerer.lower(ast);
    CompilerConfig config; config.optimization = level; config.static_stack_proof = proof; config.short_ram_bytes = short_ram;
    IRCodeGenerator generator(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
    return generator.generate(module);
}

// Stack-limit relocations patched inside the named function's CODE range.
std::size_t stackGuardsIn(const ObjectFile& object, const std::string& function) {
    std::uint32_t begin = 0, end = static_cast<std::uint32_t>(object.code_section.size());
    bool found = false;
    const std::set<std::string> functions{"main", "shared", "\x01" "leaf", "\x01" "walk", "\x01" "deep"};
    for (const auto& symbol : object.symbol_table) if (symbol.section == SymbolSection::CODE && symbol.name == function) {
        begin = symbol.offset; found = true;
    }
    require(found, "Stack-proof fixture function has no CODE symbol");
    for (const auto& symbol : object.symbol_table)
        if (symbol.section == SymbolSection::CODE && functions.count(symbol.name) && symbol.offset > begin)
            end = std::min(end, symbol.offset);
    std::size_t guards = 0;
    for (const auto& r : object.relocation_table)
        if (r.target_symbol_name.compare(0, std::string(GSUAbi::StackLimitPrefix).size(), GSUAbi::StackLimitPrefix) == 0 &&
            r.patch_offset >= begin && r.patch_offset < end) ++guards;
    return guards;
}

// Near ROM region checks load the bank into R4 (IBT R4); the read itself
// selects it through R3. Count only the checks.
std::size_t romRegionChecks(const ObjectFile& object) {
    std::size_t checks = 0;
    for (const auto& r : object.relocation_table)
        checks += r.target_symbol_name == GSUAbi::NearRomBankSymbol && object.code_section.at(r.patch_offset) == 0xa4;
    return checks;
}

// Masked indices into a defined ROM table need neither null, bank, LoROM
// region nor wrap checks; reads and results must match O1 exactly. An
// unbounded index into the same table keeps every runtime check.
void staticRomObjectCodegen() {
    const std::string tables = "rom const byte palette[8] = {1, 2, 3, 4, 5, 6, 7, 100};"
        " rom const word steps[4] = {1000, 2000, 3000, 30000};";
    const std::string masked = tables + " word main() { word s = *(volatile word*)0x100; word t = 0;"
        " for (word i = 0; i < 16; i++) t += palette[(s + i) & 7] + steps[(s ^ i) & 3]; return t; }";
    const std::string unbounded = tables + " word main() { word s = *(volatile word*)0x100;"
        " return palette[s] + steps[s]; }";
    const auto baseline = compileAt(masked, OptimizationLevel::O1);
    require(romRegionChecks(baseline) != 0, "ROM table fixture has no baseline region checks");
    for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto proved = compileAt(masked, level);
        require(romRegionChecks(proved) == 0, "Defined ROM table reads kept their bank/region checks");
        // discld's near-ROM placement check is the proof's only link-time
        // guard, and it applies only to objects that select that bank.
        require(std::any_of(proved.relocation_table.begin(), proved.relocation_table.end(),
            [](const RelocationEntry& r) { return r.target_symbol_name == GSUAbi::NearRomBankSymbol; }),
            "Proved ROM table reads no longer select the linker-checked near ROM bank");
        require(!proved.hasStaticRamProof(), "ROM table proofs requested the static RAM marker");
        require(proved.code_section.size() + 32 < baseline.code_section.size(), "ROM table proofs did not remove check code");
        require(romRegionChecks(compileAt(unbounded, level)) >= 2, "An unbounded ROM index lost its runtime checks");
        for (const std::uint16_t seed : {0, 1, 5, 0x7fff, 0xffff}) {
            DiscoGSU::Machine a(executable(baseline), 0x8000), b(executable(proved), 0x8000);
            for (auto* machine : {&a, &b}) { seedWord(*machine, 0x700100, seed); machine->run(); }
            require(a.reg(6) == 0 && b.reg(6) == 0 && a.reg(0) == b.reg(0) && a.reg(10) == b.reg(10),
                "Proved ROM table reads changed the result or stack state");
        }
    }
}

// Single-site inlining must keep parameter reassignment, PHI joins and
// volatile effects exactly as the call did (O0 executes the real call).
void singleSiteInlineExecution() {
    const std::string source = "internal word span(word a, word b);"
        "word main() { word x = *(volatile word*)0x100; word r = span(x, 7) + 1; *(volatile word*)0x102 = r; return r; }"
        "internal word span(word a, word b) { if (a > b) { word t = a; a = b; b = t; }"
        " *(volatile word*)0x104 = a; if (b - a > 9) return 9; return b - a; }";
    const auto baseline = compileStackProof(source, OptimizationLevel::Baseline, false);
    for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto inlined = compileStackProof(source, level, false);
        require(std::none_of(inlined.symbol_table.begin(), inlined.symbol_table.end(),
            [](const SymbolEntry& s) { return s.name == std::string("\x01" "span"); }), "Single-site callee was still emitted");
        for (const std::uint16_t x : {0, 3, 7, 8, 16, 17, 30, 0xffff}) {
            DiscoGSU::Machine a(executable(baseline), 0x8000, 0x1000), b(executable(inlined), 0x8000, 0x1000);
            for (auto* machine : {&a, &b}) { seedWord(*machine, 0x700100, x); machine->run(); }
            require(a.reg(6) == 0 && b.reg(6) == 0 && a.reg(0) == b.reg(0) && a.word(0x700102) == b.word(0x700102) &&
                a.word(0x700104) == b.word(0x700104) && a.accesses(0x700104, true) == b.accesses(0x700104, true),
                "Inlined callee changed its result or volatile effects");
        }
    }
}

// Register arguments must match the stack convention exactly: swapped
// sources (a copy cycle), literals, frame-resident values under pressure,
// a call inside a hardware loop, a callee forwarding its own parameters, and
// a five-parameter callee that keeps the stack argument area.
void registerArgumentExecution() {
    const std::string source =
        "internal word mix(word a, word b, word c, word d); internal word twice(word v, word w);"
        "internal word wide(word a, word b, word c, word d, word e);"
        "word main() { word x = *(volatile word*)0x100; word y = *(volatile word*)0x102;"
        " word s = mix(y, x, 7, x + y) + mix(x, y, 300, 1);"
        " for (word k = 3; k > 0; k = k - 1) s = s + twice(s, x);"
        " word p = x + 1; word q = y + 2; word r = x ^ y; word u = s - x; word v = s + y; word w = p & q;"
        " s = s + mix(r, u, v, w) + mix(w, v, u, r) + p + q + r + u + v + w;"
        " s = s + wide(x, y, s, 2, 3) + wide(y, x, 1, s, 4); *(volatile word*)0x104 = s; return s; }"
        "internal word mix(word a, word b, word c, word d) { if (a > b) return a - b + c * 2 + d; return b - a + c + d * 3; }"
        "internal word twice(word v, word w) { word r = mix(w, v, 1, 2); return r + v; }"
        "internal word wide(word a, word b, word c, word d, word e) { if (e > 3) return a + b * 2 + c; return d * 3 + e + a; }";
    const auto baseline = compileStackProof(source, OptimizationLevel::Baseline, false);
    for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto optimized = compileStackProof(source, level, false);
        // LINK #4 / IWT R15: the opcodes before LINK are a PUSH (STW (R10), DEC
        // [, DEC]; the scheduler may move the second DEC into the call's slot)
        // only when the last argument went through the stack.
        std::size_t register_calls = 0, stack_calls = 0;
        for (const auto& r : optimized.relocation_table) {
            if (r.type != RelocationType::ADDR16_JAL || r.patch_offset < 4) continue;
            const auto& code = optimized.code_section;
            const bool pushed = code.at(r.patch_offset - 1) == 0x94 && code.at(r.patch_offset - 2) == 0xea &&
                (code.at(r.patch_offset - 3) == 0x3a || (code.at(r.patch_offset - 3) == 0xea && code.at(r.patch_offset - 4) == 0x3a));
            if (r.target_symbol_name == std::string("\x01" "mix")) { require(!pushed, "A register-argument call pushed its arguments"); ++register_calls; }
            if (r.target_symbol_name == std::string("\x01" "wide")) { require(pushed, "A five-parameter call lost its stack arguments"); ++stack_calls; }
        }
        if (register_calls < 3 || stack_calls != 2)
            throw std::runtime_error("Register-argument fixture lost its calls: " + std::to_string(register_calls) +
                " register, " + std::to_string(stack_calls) + " stack");
        const std::pair<std::uint16_t, std::uint16_t> inputs[] = {{0, 0}, {1, 2}, {9, 4}, {300, 7}, {0xfff0, 5}, {5, 0xfff0}};
        for (const auto& input : inputs) {
            DiscoGSU::Machine a(executable(baseline), 0x8000, 0x1000), b(executable(optimized), 0x8000, 0x1000);
            for (auto* machine : {&a, &b}) {
                seedWord(*machine, 0x700100, input.first); seedWord(*machine, 0x700102, input.second); machine->run();
            }
            require(a.reg(6) == 0 && b.reg(6) == 0 && a.reg(0) == b.reg(0) && a.word(0x700104) == b.word(0x700104),
                "Register arguments changed a call's result");
        }
    }
}

// Code bytes of `name` up to the next CODE symbol that starts a function
// (block labels and register entries inside it do not end the range).
std::vector<std::uint8_t> functionCode(const ObjectFile& object, const std::string& name, const std::set<std::string>& functions) {
    std::uint32_t begin = 0, end = static_cast<std::uint32_t>(object.code_section.size());
    bool found = false;
    for (const auto& symbol : object.symbol_table)
        if (symbol.section == SymbolSection::CODE && symbol.name == name) { begin = symbol.offset; found = true; }
    require(found, "Calling-convention fixture function has no CODE symbol");
    for (const auto& symbol : object.symbol_table)
        if (symbol.section == SymbolSection::CODE && functions.count(symbol.name) && symbol.offset > begin)
            end = std::min(end, symbol.offset);
    return {object.code_section.begin() + begin, object.code_section.begin() + end};
}

// Public functions with one-word parameters get a register entry that
// in-module calls use, while the public label keeps the stack convention for
// other objects. A leaf that never touches the stack saves no R11 and has no
// stack guard. `pick` passes the leaf prefilter but its pointer comparison
// pushes, so it must fall back to a guarded frame. Results and the preserved
// R9/R10 match O0. Stack callers from other objects are linked and run by
// run_checked_proofs_regression.cmake.
void publicRegisterEntryExecution() {
    const std::string source =
        "word leaf(word a, word b); internal word pick(word* p, word* q); word outer(word a); word deref(word* p, word n);"
        "word main() { word x = *(volatile word*)0x100; word y = *(volatile word*)0x102;"
        " word s = leaf(x, y) + leaf(y, 7) + outer(x); word t[2]; t[0] = x; t[1] = y;"
        " s = s + pick(&t[0], &t[1]) + pick(&t[1], &t[1]) + deref(&t[1], x); *(volatile word*)0x104 = s; return s; }"
        "word leaf(word a, word b) { if (a > b) return a - b; return b + a * 2; }"
        "internal word pick(word* p, word* q) { if (p == q) return *p; return *q + 1; }"
        "word outer(word a) { return leaf(a, 3) + leaf(3, a); }"
        "word deref(word* p, word n) { return *p + leaf(n, 1); }";
    const std::set<std::string> functions{"main", "leaf", "outer", "deref", "\x01" "pick"};
    const auto baseline = compileStackProof(source, OptimizationLevel::Baseline, false);
    require(baseline.symbol_table.end() == std::find_if(baseline.symbol_table.begin(), baseline.symbol_table.end(),
        [](const SymbolEntry& symbol) { return symbol.name.compare(0, 15, GSUAbi::RegisterCallPrefix) == 0; }),
        "O0 emitted a register entry");
    for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto optimized = compileStackProof(source, level, false);
        const auto entry = GSUAbi::registerCallSymbol("leaf", 2);
        std::uint32_t public_offset = 0, entry_offset = 0;
        for (const auto& symbol : optimized.symbol_table) {
            if (symbol.name == "leaf") public_offset = symbol.offset;
            if (symbol.name == entry) entry_offset = symbol.offset;
        }
        require(entry_offset > public_offset, "Public leaf lost its register entry after the stack-argument loads");
        for (const auto& r : optimized.relocation_table)
            require(r.target_symbol_name != "leaf", "An in-module call used the stack-argument label");
        // The leaf has no IWT operands, so any $3A byte is a STW (R10) push.
        const auto leaf = functionCode(optimized, "leaf", functions);
        require(std::find(leaf.begin(), leaf.end(), 0x3a) == leaf.end(), "A stack-free leaf saved R11");
        const auto guards = [&](const std::string& name) {
            std::uint32_t begin = 0;
            for (const auto& symbol : optimized.symbol_table) if (symbol.name == name) begin = symbol.offset;
            const auto end = begin + functionCode(optimized, name, functions).size();
            std::size_t count = 0;
            for (const auto& r : optimized.relocation_table)
                if (r.target_symbol_name.compare(0, std::string(GSUAbi::StackLimitPrefix).size(), GSUAbi::StackLimitPrefix) == 0 &&
                    r.patch_offset >= begin && r.patch_offset < end) ++count;
            return count;
        };
        require(guards("leaf") == 0, "A stack-free leaf kept a stack guard");
        // A checked public label performs only the argument checks (SP
        // alignment, then the argument ceiling) before its loads; the
        // register entry's own stack guard covers both entries.
        std::uint32_t deref_offset = 0;
        for (const auto& symbol : optimized.symbol_table) if (symbol.name == "deref") deref_offset = symbol.offset;
        require(!std::any_of(optimized.relocation_table.begin(), optimized.relocation_table.end(),
            [&](const RelocationEntry& r) {
                return r.patch_offset == deref_offset &&
                    r.target_symbol_name.compare(0, std::string(GSUAbi::StackLimitPrefix).size(), GSUAbi::StackLimitPrefix) == 0;
            }), "A checked public label duplicated its register entry's stack guard");
        require(guards("deref") >= 1, "A checked public register entry lost its own stack guard");
        require(guards("\x01" "pick") > 0, "A leaf that pushes lost its stack guard");
        const std::pair<std::uint16_t, std::uint16_t> inputs[] = {{0, 0}, {1, 2}, {9, 4}, {300, 7}, {0xfff0, 5}, {5, 0xfff0}};
        for (const auto& input : inputs) {
            DiscoGSU::Machine a(executable(baseline), 0x8000, 0x1000), b(executable(optimized), 0x8000, 0x1000);
            for (auto* machine : {&a, &b}) {
                seedWord(*machine, 0x700100, input.first); seedWord(*machine, 0x700102, input.second); machine->run();
            }
            require(a.reg(6) == 0 && b.reg(6) == 0 && a.reg(0) == b.reg(0) && a.word(0x700104) == b.word(0x700104),
                "A register entry or stack-free leaf changed a call's result");
            require(a.reg(9) == b.reg(9) && a.reg(10) == b.reg(10), "A register entry changed the preserved frame/stack state");
        }
    }
}

// Compiled (static) frames, in a unit that already owns static RAM:
// callers' frames stay intact under callees (a
// callee reads its caller's array through a pointer), a function shared by
// two static callers sits above both, and a recursive function keeps its
// stack frame. Results match O0's stack frames for every input.
void staticFrameExecution() {
    const std::string source =
        "internal word sum4(word* p); internal word fill(word seed); internal word deep(word n); internal word bias;"
        "word main() { word a[6]; word x = *(volatile word*)0x100; bias = x & 3;"
        " for (word i = 0; i < 6; i++) a[i] = x + i * 3;"
        " word s = sum4(&a[1]) + fill(x) + deep(*(volatile word*)0x104) + a[0] + a[5] + bias;"
        " *(volatile word*)0x102 = s; return s; }"
        "internal word sum4(word* p) { word t[4]; for (word i = 0; i < 4; i++) t[i] = p[i] * 2;"
        " if (t[0] > t[3]) return t[0] - t[3] + t[1]; return t[1] + t[2] + t[3]; }"
        "internal word fill(word seed) { word b[5]; for (word i = 0; i < 5; i++) b[i] = seed ^ i; return sum4(&b[0]) + b[4]; }"
        "internal word deep(word n) { word c[2]; c[0] = n; c[1] = n + 1; if (n == 0) return c[1]; return deep(n - 1) + c[0]; }";
    const auto baseline = compileStackProof(source, OptimizationLevel::Baseline, false);
    const auto frame = [](const ObjectFile& o, const std::string& owner) {
        const auto prefix = std::string("\x01" "__disco_frame.") + owner + ".";
        return std::any_of(o.symbol_table.begin(), o.symbol_table.end(), [&](const SymbolEntry& s) {
            return s.section == SymbolSection::RAM && s.name.compare(0, prefix.size(), prefix) == 0;
        });
    };
    for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto stacked = compileStackProof(source, level, false), framed = compileStackProof(source, level, true);
        require(!frame(stacked, "main") && stacked.ram_section.size() == 2, "Static frames without the runtime-entry contract");
        require(frame(framed, "main") && frame(framed, "sum4") && frame(framed, "fill") && !frame(framed, "deep"),
            "Static frames were not assigned to exactly the non-reentrant functions");
        require(framed.hasStaticRamProof() && framed.code_section.size() < stacked.code_section.size(),
            "Static frames did not shrink the code or omitted the nonzero-RAM check");
        for (const std::uint16_t x : {0, 1, 7, 300, 0xfff0}) for (const std::uint16_t depth : {0, 3}) {
            DiscoGSU::Machine a(executable(baseline, 0x0800), 0x8000, 0x1000), b(executable(framed, 0x0800), 0x8000, 0x1000);
            for (auto* machine : {&a, &b}) { seedWord(*machine, 0x700100, x); seedWord(*machine, 0x700104, depth); machine->run(); }
            require(a.reg(6) == 0 && b.reg(6) == 0 && a.reg(0) == b.reg(0) && a.word(0x700102) == b.word(0x700102),
                "Static frames changed a result");
        }
    }
    // A path out of the unit could re-enter through a callback: the caller
    // of an external function, and its own callers, keep stack frames, while
    // an unrelated leaf still gets a static one.
    const auto leaving = compileStackProof("word host(word v); internal word wrap(word v); internal word solo(word v); internal word kept;"
        "word main() { word m[4]; word x = *(volatile word*)0x100; for (word i = 0; i < 4; i++) m[i] = x + i;"
        " kept = solo(m[x & 3]) + solo(m[1]); return wrap(m[2]) + kept; }"
        "internal word wrap(word v) { word a[4]; for (word i = 0; i < 4; i++) a[i] = v ^ i; return host(a[v & 3]) + a[1]; }"
        "internal word solo(word v) { word c[4]; for (word i = 0; i < 4; i++) c[i] = v + i; return c[v & 3]; }", OptimizationLevel::O2, true);
    require(!frame(leaving, "main") && !frame(leaving, "wrap") && frame(leaving, "solo"),
        "A function that can reach outside the unit received a static frame");
}

// Opcode starts within [begin, end) of straight-line GSU code (no data):
// prefixes are their own one-byte opcodes, ALT1/ALT2 turn IBT into LMS/SMS
// and IWT into LM/SM (one more operand byte), branches take one operand.
std::vector<std::uint32_t> opcodeStarts(const std::vector<std::uint8_t>& code, std::uint32_t begin, std::uint32_t end) {
    std::vector<std::uint32_t> starts;
    bool alt = false;
    for (std::uint32_t pc = begin; pc < end;) {
        const auto op = code.at(pc);
        starts.push_back(pc);
        std::uint32_t length = 1;
        if (op >= 0x05 && op <= 0x0f) length = 2;
        else if (op >= 0xa0 && op <= 0xaf) length = 2;
        else if (op >= 0xf0) length = alt ? 4 : 3;
        alt = op == 0x3d || op == 0x3e || op == 0x3f;
        pc += length;
    }
    return starts;
}

std::pair<std::uint32_t, std::uint32_t> functionRange(const ObjectFile& object, const std::string& function,
                                                      const std::set<std::string>& functions) {
    std::uint32_t begin = 0, end = 0;
    bool found = false;
    for (const auto& symbol : object.symbol_table) if (symbol.section == SymbolSection::CODE && symbol.name == function) {
        begin = symbol.offset; found = true;
    }
    require(found, "Frameless fixture function has no CODE symbol");
    // Shared division helpers follow the last function; this fixture has none.
    end = static_cast<std::uint32_t>(object.code_section.size());
    for (const auto& symbol : object.symbol_table)
        if (symbol.section == SymbolSection::CODE && functions.count(symbol.name) && symbol.offset > begin)
            end = std::min(end, symbol.offset);
    return {begin, end};
}

// Static-frame functions without stack parameters neither save nor set FP,
// and under pressure hold a value in R9. Their callers preserve R9 only when
// it matters: `wide` (stack parameters) and `main` keep FP across the calls
// whose callee may write R9, `mid` passes the clobber on to its caller. Each
// result, and main's FP/SP at STOP, matches O0, where every function frames.
void framelessExecution() {
    const std::string source =
        "internal word hot(word a, word b); internal word mid(word a);"
        "internal word wide(word a, word b, word c, word d, word e); internal word bias;"
        "word main() { word x = *(volatile word*)0x100; word y = *(volatile word*)0x102; bias = x & 7;"
        " word s = hot(x, y) + wide(x, y, x + 1, y + 2, bias) + mid(y) + bias;"
        " *(volatile word*)0x104 = s; return s; }"
        "internal word hot(word a, word b) { word p = a + 1; word q = b + 2; word r = a ^ b; word u = a - b;"
        " word v = a + b + bias; word w = p & q; word z = r | u; word y = (p + q) ^ (r + u);"
        " return p + q + r + u + v + w + z + y + (p ^ v) + (q ^ w) + (r ^ z) + (u ^ y); }"
        "internal word mid(word a) { return hot(a, a + 1) + 1; }"
        "internal word wide(word a, word b, word c, word d, word e) { word h = hot(a, b); return h + a * 3 + b + c + d * 5 + e; }";
    const std::set<std::string> functions{"main", "\x01" "hot", "\x01" "mid", "\x01" "wide"};
    const auto baseline = compileStackProof(source, OptimizationLevel::Baseline, false);
    for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto object = compileStackProof(source, level, true);
        const auto count = [&](const std::string& function, std::uint8_t first, int second) {
            const auto range = functionRange(object, function, functions);
            const auto starts = opcodeStarts(object.code_section, range.first, range.second);
            std::size_t matches = 0;
            for (std::size_t k = 0; k < starts.size(); ++k)
                if (object.code_section.at(starts[k]) == first &&
                    (second < 0 || (k + 1 < starts.size() && object.code_section.at(starts[k + 1]) == second))) ++matches;
            return matches;
        };
        // PUSH R9 is WITH R9; STW (R10). TO/FROM/WITH R9 are 0x19/0xB9/0x29.
        require(count("\x01" "hot", 0x29, 0x3a) == 0 && count("\x01" "mid", 0x29, 0x3a) == 0,
            "A frameless function still saved FP");
        require(count("\x01" "hot", 0x19, -1) + count("\x01" "hot", 0xb9, -1) + count("\x01" "hot", 0x29, -1) > 0,
            "Register pressure in a frameless function did not reach R9");
        require(count("\x01" "wide", 0x29, 0x3a) >= 2 && count("main", 0x29, 0x3a) >= 2,
            "A framed caller did not preserve FP around an R9-writing callee");
        for (const std::uint16_t x : {0, 1, 7, 300, 0xfff0}) for (const std::uint16_t y : {0, 5, 0x8001}) {
            DiscoGSU::Machine a(executable(baseline, 0x0800), 0x8000, 0x1000), b(executable(object, 0x0800), 0x8000, 0x1000);
            for (auto* machine : {&a, &b}) { seedWord(*machine, 0x700100, x); seedWord(*machine, 0x700102, y); machine->run(); }
            require(a.reg(6) == 0 && b.reg(6) == 0 && a.reg(0) == b.reg(0) && a.word(0x700104) == b.word(0x700104) &&
                a.reg(9) == b.reg(9) && a.reg(10) == b.reg(10), "Frameless functions changed a result or main's frame");
        }
    }
}

// Short RAM: static-frame spills and divmod components move to the LMS/SMS
// window, read straight into argument registers and register loops, and
// store back after a short load (SBK uses the latch LMS/SMS set). A window
// too small for a function leaves its slots in ordinary static RAM. Every
// result matches O0's stack frames.
void shortRamExecution() {
    const std::string source =
        "internal word mix(word a, word b); internal word bias;"
        "word main() { word x = *(volatile word*)0x100; word y = *(volatile word*)0x102; bias = x & 3;"
        " word p = x + 1; word q = y + 2; word r = x ^ y; word u = x - y; word v = x + y; word w = p & q;"
        " word z = r | u; word k = (p + q) ^ (r + u); word s = mix(p, q) + mix(r, u);"
        " for (word i = 0; i < (y & 7); i++) { s = s + mix(s, i) + p; p = p + q; }"
        " s = s + p + q + r + u + v + w + z + k + (p ^ v) + (q ^ w) + (r ^ z) + (u ^ k);"
        " word d = y | 1; s = s + x / d + x % d; *(volatile word*)0x104 = s; return s; }"
        "internal word mix(word a, word b) { word t = a + b; word m = a - b; word n = a ^ b; word o = a | b;"
        " word e = b | 1; word g = a / e; word h = a % e;"
        " return t + m + n + o + g + h + (t ^ n) + (m ^ o) + (g ^ t) + (h ^ m) + bias; }";
    const auto baseline = compileStackProof(source, OptimizationLevel::Baseline, false);
    const auto lms = [](const ObjectFile& o) {
        return std::count_if(o.relocation_table.begin(), o.relocation_table.end(), [](const RelocationEntry& r) {
            return r.type == RelocationType::ADDR8_SHORT_RAM;
        });
    };
    for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto ordinary = compileStackProof(source, level, true);
        const auto windowed = compileStackProof(source, level, true, GSUAbi::ShortRamCapacity);
        const auto tight = compileStackProof(source, level, true, 2);
        require(!ordinary.usesShortRam() && windowed.usesShortRam() && lms(windowed) >= 4,
            "Short RAM did not hold the fixture's static-frame word slots");
        require(windowed.code_section.size() < ordinary.code_section.size(), "LMS/SMS did not shrink frame accesses");
        require(tight.short_ram_section.size() <= 2 && lms(tight) < lms(windowed),
            "Short RAM admitted more than its claimed bytes");
        for (const std::uint16_t x : {0, 1, 7, 300, 0xfff0}) for (const std::uint16_t y : {0, 5, 0x8001, 0x0007}) {
            std::uint16_t results[4] = {}, stored[4] = {};
            const ObjectFile* objects[4] = {&baseline, &ordinary, &windowed, &tight};
            for (int k = 0; k < 4; ++k) {
                DiscoGSU::Machine machine(executable(*objects[k], 0x0800), 0x8000, 0x1000);
                seedWord(machine, 0x700100, x); seedWord(machine, 0x700102, y);
                machine.run();
                require(machine.reg(6) == 0, "Short RAM fixture faulted");
                results[k] = machine.reg(0); stored[k] = machine.word(0x700104);
            }
            for (int k = 1; k < 4; ++k)
                require(results[k] == results[0] && stored[k] == stored[0], "Short RAM changed a result");
        }
    }
}

void staticStackProofCodegen() {
    // main comes first: the execution fixture starts at CODE offset zero.
    // walk is reached only from covered main; leaf also from exported shared,
    // and deep recurses, so both keep their runtime guards.
    const std::string source =
        "internal word leaf(word v); internal word walk(word n); internal word deep(word n); word shared(word v);"
        "word main() { word n = *(volatile word*)0x100; word t = walk(n) + shared(n) + deep(*(volatile word*)0x104);"
        " *(volatile word*)0x102 = t; return t; }"
        "internal word leaf(word v) { word a[4]; a[v & 3] = v; return a[v & 3] + 1; }"
        "internal word walk(word n) { word s = 0; for (word i = 0; i < n; i++) s += leaf(i); return s; }"
        "internal word deep(word n) { if (n == 0) return 0; return deep(n - 1) + 1; }"
        "word shared(word v) { return leaf(v) + 2; }";
    for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto checked = compileStackProof(source, level, false);
        const auto proved = compileStackProof(source, level, true);
        require(!checked.staticStackBound(), "A stack bound was recorded without the runtime-entry contract");
        require(proved.staticStackBound() >= 8, "Covered functions did not record their stack bound");
        require(stackGuardsIn(checked, "main") && stackGuardsIn(checked, "\x01" "walk"),
            "Unproved fixture has no guards to discharge");
        require(!stackGuardsIn(proved, "main") && !stackGuardsIn(proved, "\x01" "walk"),
            "A covered function kept a provably unfailing stack guard");
        require(stackGuardsIn(proved, "shared") && stackGuardsIn(proved, "\x01" "leaf") && stackGuardsIn(proved, "\x01" "deep"),
            "An exported, multiply-entered or recursive function lost its runtime guard");
        require(proved.code_section.size() < checked.code_section.size(), "Discharged guards did not shrink the payload");
        // Same results; an uncovered recursive callee still faults precisely
        // at its own push once the stack above the bound is exhausted.
        for (const std::uint16_t depth : {0, 3, 400}) {
            std::uint16_t results[2] = {0, 0}, faults[2] = {0, 0};
            const ObjectFile* objects[2] = {&checked, &proved};
            for (int k = 0; k < 2; ++k) {
                const auto floor = static_cast<std::uint16_t>(0x0400);
                const auto sp = static_cast<std::uint16_t>(floor + proved.staticStackBound() + 64);
                DiscoGSU::Machine machine(executable(*objects[k], floor), 0x8000, sp);
                seedWord(machine, 0x700100, 5); seedWord(machine, 0x700104, depth);
                machine.run();
                results[k] = machine.reg(0); faults[k] = machine.reg(6);
            }
            require(faults[0] == faults[1] && (depth == 400) == (faults[1] == 2), "Stack proof changed a recursive stack fault");
            if (!faults[1]) require(results[0] == results[1] && results[1] == 23 + depth,
                "Stack proof changed a covered call tree's result");
        }
    }
    // main entered by code is not the runtime entry: no proof at all.
    const auto reentered = compileStackProof("word main(); internal word again() { return main(); }"
        "word main() { word x[4]; x[1] = *(volatile word*)0x100; if (x[1] != 0) return 1; return again(); }", OptimizationLevel::O2, true);
    require(!reentered.staticStackBound(), "A main called by code received a runtime-entry stack bound");
    require(!compileStackProof(source, OptimizationLevel::O1, true).staticStackBound(),
        "O1 omitted stack guards without the global backend");
}


void stackCreditControlFlowExecution() {
    const std::string conditional =
        "word main(){word n=*(volatile word*)0x100;word c=*(volatile word*)0x102;word s=0;"
        "while(n>0){*(volatile word*)0x110=11;"
        "if(c){s+=n/(*(volatile word*)0x104);}else{s+=n/(*(volatile word*)0x106);}"
        "*(volatile word*)0x112=22;n--;}return s;}";
    const std::string far_join =
        "word main(){word n=*(volatile word*)0x100;"
        "far word* p=*(word* far volatile*)0x108;far word* q=*(word* far volatile*)0x10c;word s=0;"
        "while(n>0){plot{s+=(word)(p==q);*(volatile word*)0x110=11;"
        "s+=n/(*(volatile word*)0x104);*(volatile word*)0x112=22;}n--;}return s;}";
    unsigned tested = 0;
    for (const bool far : {false, true}) {
        Lexer lexer(far ? far_join : conditional); const auto tokens = lexer.scanTokens();
        Parser parser(tokens); auto ast = parser.parseProgram();
        DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast);
        IRLowerer lowerer; const auto module = lowerer.lower(ast);
        // O1 is the unaffected local backend; all three policies must satisfy
        // the same ABI contract relative to their own final emitted frame.
        for (const auto level : {OptimizationLevel::O1, OptimizationLevel::O2, OptimizationLevel::Size}) {
            CompilerConfig config; config.optimization = level;
            IRCodeGenerator generator(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
            const auto object = generator.generate(module);
            const auto entry_bytes = entryStackBytes(object);
            for (const std::uint16_t floor : {std::uint16_t{0}, std::uint16_t{64}})
                for (const unsigned trips : {0u, 1u, 6u})
                    for (unsigned arm = 0; arm < (far ? 3u : 2u); ++arm)
                        for (const bool negative_divisor : {false, true})
                            for (const unsigned boundary : {0u, 2u, 4u}) {
                                // Far comparison needs four bytes itself. In
                                // plot, the following signed division needs
                                // eight (coordinates plus two sign words).
                                const auto available = boundary + (far ? 4u : 0u);
                                const auto required = far ? 8u : 4u;
                                const auto initial = floor + entry_bytes + available;
                                require(initial + 2 < 0x100, "Stack-boundary fixture overlaps its input ports");
                                DiscoGSU::Machine machine(executable(object, floor), 0x8000,
                                    static_cast<std::uint16_t>(initial));
                                const int divisor = negative_divisor ? -3 : 3;
                                const int other_divisor = negative_divisor ? -5 : 5;
                                seedWord(machine, 0x700100, static_cast<std::uint16_t>(trips));
                                seedWord(machine, 0x700102, static_cast<std::uint16_t>(arm));
                                seedWord(machine, 0x700104, static_cast<std::uint16_t>(divisor));
                                seedWord(machine, 0x700106, static_cast<std::uint16_t>(other_divisor));
                                // Far pointer slots store bank first, then offset.
                                seedWord(machine, 0x700108, 0x70); seedWord(machine, 0x70010a, 0x200);
                                seedWord(machine, 0x70010c, arm == 2 ? 0x71 : 0x70);
                                seedWord(machine, 0x70010e, arm == 1 ? 0x202 : 0x200);
                                seedWord(machine, 0x700110, 0x33cc); seedWord(machine, 0x700112, 0x9669);
                                const auto below = floor ? floor - 2u : 0xfffeu;
                                seedWord(machine, 0x700000u + floor, 0x5aa5);
                                seedWord(machine, 0x700000u + below, 0xc33c);
                                seedWord(machine, 0x700000u + initial + 2u, 0x6996);
                                seedWord(machine, 0x710000u + floor, 0x3cc3);
                                machine.run();
                                const bool fault = trips != 0 && available < required;
                                const auto context = std::string(far ? "far compare join" : "conditional arm") +
                                    " level=" + std::to_string(static_cast<unsigned>(level)) +
                                    " floor=" + std::to_string(floor) + " trips=" + std::to_string(trips) +
                                    " arm=" + std::to_string(arm) + " negative=" + std::to_string(negative_divisor) +
                                    " available=" + std::to_string(available);
                                const auto check = [&](bool ok, const char* message) {
                                    if (!ok) throw std::runtime_error(context + ": " + message);
                                };
                                check(machine.reg(6) == (fault ? 2 : 0), "stack fault category changed");
                                check(machine.reg(10) == (fault ? floor : initial - 4), "stack failure/return SP changed");
                                check(machine.ramBank() == 0 && machine.romBank() == 0, "stack guard changed a bank");
                                check(machine.word(0x700110) == (trips ? 11 : 0x33cc), "guard moved before its preceding witness");
                                check(machine.word(0x700112) == (trips && !fault ? 22 : 0x9669), "guard moved after its following witness");
                                check(machine.accesses(0x700110, true) == (fault ? 1u : trips) &&
                                    machine.accesses(0x700112, true) == (fault ? 0u : trips), "volatile witness count changed");
                                const auto divisions = fault ? 1u : trips;
                                check(machine.accesses(0x700104, false) == (far || arm ? divisions : 0u) &&
                                    machine.accesses(0x700106, false) == (!far && !arm ? divisions : 0u),
                                    "guard crossed or duplicated a selected volatile divisor read");
                                check(machine.word(0x700000u + floor) == 0x5aa5 &&
                                    machine.word(0x700000u + below) == 0xc33c &&
                                    machine.word(0x700000u + initial + 2u) == 0x6996 &&
                                    machine.word(0x710000u + floor) == 0x3cc3, "stack boundary/foreign-bank canary changed");
                                for (const auto address : {0x700000u + floor, 0x700000u + below,
                                        0x700000u + initial + 2u, 0x710000u + floor})
                                    check(machine.accesses(address, true) == 0, "stack boundary/foreign-bank canary was written");
                                if (!fault) {
                                    int expected = 0;
                                    for (unsigned n = trips; n > 0; --n)
                                        expected += static_cast<int>(n) / (far || arm ? divisor : other_divisor) +
                                            (far && arm == 0 ? 1 : 0);
                                    check(machine.reg(0) == static_cast<std::uint16_t>(expected), "original loop result changed");
                                }
                                ++tested;
                            }
        }
    }
    std::cout << "Stack-credit CFG/local joins: " << tested << " execution boundary cases\n";
}

void constantOffsetExecution() {
    for (const auto& offset : {std::string("0"), std::string("1"), std::string("-1"), std::string("32767"),
            std::string("-32768"), std::string("(unsigned word)65535")}) {
        for (const bool subtract : {false,true}) {
            const std::string source = "word main(){word* p=(word*)*(volatile unsigned word*)0x100;"
                "*(volatile word*)0x102=11;word* q=p" + std::string(subtract ? "-" : "+") + "(" + offset +
                ");*(volatile word*)0x104=22;return (word)(unsigned word)q;}";
            Lexer lexer(source); const auto tokens=lexer.scanTokens(); Parser parser(tokens); auto ast=parser.parseProgram();
            DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; const auto module=lowerer.lower(ast);
            CompilerConfig config; config.optimization=OptimizationLevel::Baseline;
            IRCodeGenerator baseline(analyzer.getAllLocalSymbols(),analyzer.getFunctionSymbols(),data,config);
            const auto original=executable(baseline.generate(module));
            for (const auto level : {OptimizationLevel::O1,OptimizationLevel::O2,OptimizationLevel::Size}) {
                config.optimization=level; IRCodeGenerator generator(analyzer.getAllLocalSymbols(),analyzer.getFunctionSymbols(),data,config);
                const auto optimized=executable(generator.generate(module));
                for (const unsigned address : {0u,1u,2u,4u,0x7ffeu,0x8000u,0xfffcu,0xfffeu,0xffffu}) {
                    DiscoGSU::Machine a(original,0x8000),b(optimized,0x8000);
                    for (auto* m : {&a,&b}) { m->seed(0x700100,static_cast<std::uint8_t>(address),false);
                        m->seed(0x700101,static_cast<std::uint8_t>(address >> 8),false); m->run(); }
                    require(a.reg(6)==b.reg(6),"Literal pointer offset changed null/alignment/wrap fault priority");
                    require(a.reg(6)!=0 || a.reg(0)==b.reg(0),"Literal pointer offset changed its successful result");
                    require(a.word(0x700102)==b.word(0x700102) && a.word(0x700104)==b.word(0x700104),
                        "Literal pointer offset moved a trap across an observable store");
                }
            }
        }
    }
}

void pointerComponentExecution() {
    for (const bool unsigned_index : {false,true}) for (const bool subtract : {false,true}) {
        const std::string index_type = unsigned_index ? "unsigned word" : "word";
        const std::string source = "word main(){word* p=(word*)*(volatile unsigned word*)0x100;" + index_type +
            " i=*(volatile " + index_type + "*)0x102;*(volatile word*)0x104=11;word* q=p" +
            (subtract ? "-" : "+") + "i;*(volatile word*)0x106=22;return (word)(unsigned word)q;}";
        Lexer lexer(source); const auto tokens=lexer.scanTokens(); Parser parser(tokens); auto ast=parser.parseProgram();
        DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; const auto module=lowerer.lower(ast);
        CompilerConfig config; config.optimization=OptimizationLevel::Baseline;
        IRCodeGenerator baseline(analyzer.getAllLocalSymbols(),analyzer.getFunctionSymbols(),data,config);
        const auto original=executable(baseline.generate(module));
        for (const auto level : {OptimizationLevel::O2,OptimizationLevel::Size}) {
            config.optimization=level; IRCodeGenerator generator(analyzer.getAllLocalSymbols(),analyzer.getFunctionSymbols(),data,config);
            const auto optimized=executable(generator.generate(module));
            for (const unsigned address : {0u,1u,2u,4u,0x7ffeu,0x8000u,0xfffcu,0xfffeu,0xffffu})
                for (const unsigned index : {0u,1u,2u,32767u,32768u,65535u}) {
                    DiscoGSU::Machine a(original,0x8000), b(optimized,0x8000);
                    for (auto* m : {&a,&b}) {
                        m->seed(0x700100,static_cast<std::uint8_t>(address),false);
                        m->seed(0x700101,static_cast<std::uint8_t>(address >> 8),false);
                        m->seed(0x700102,static_cast<std::uint8_t>(index),false);
                        m->seed(0x700103,static_cast<std::uint8_t>(index >> 8),false); m->run();
                    }
                    require(a.reg(6)==b.reg(6) && (a.reg(6) || a.reg(0)==b.reg(0)),
                        "Component check changed signed/unsigned pointer alignment, null or overflow priority");
                    require(a.word(0x700104)==b.word(0x700104) && a.word(0x700106)==b.word(0x700106),
                        "Component check moved a pointer fault across an observable witness");
                }
        }
    }
    for (const auto& address : {std::string("0x700000"),std::string("0x710000")}) {
        const auto source = "word main(){*(volatile word*)0x104=11;far word* p=(far word*)" + address +
            ";word* q=(word*)p;*(volatile word*)0x106=22;return (word)(unsigned word)q;}";
        Lexer lexer(source); const auto tokens=lexer.scanTokens(); Parser parser(tokens); auto ast=parser.parseProgram();
        DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; const auto module=lowerer.lower(ast);
        for (const auto level : {OptimizationLevel::Baseline,OptimizationLevel::O2,OptimizationLevel::Size}) {
            CompilerConfig config; config.optimization=level;
            IRCodeGenerator generator(analyzer.getAllLocalSymbols(),analyzer.getFunctionSymbols(),data,config);
            DiscoGSU::Machine machine(executable(generator.generate(module)),0x8000); machine.run();
            require(machine.reg(6)==(address=="0x700000" ? 2 : 4) && machine.word(0x700104)==11 && machine.word(0x700106)==0,
                "Non-null far pair incorrectly proved its zero offset non-null during narrowing");
        }
    }
}

void pointerSnapshotExecution() {
    for (const std::string source : {
        "word main(){word a=7;word b=11;word* p=&a;word** alias=&p;word* old=*alias;*alias=&b;return *old+*p;}",
        "word step();word main(){word* p=(word*)*(volatile unsigned word*)0x100;word before=*p;word effect=step();return before+effect+*p;}"
            "word step(){*(volatile word*)0x200=19;return 3;}",
        "word main(){volatile word* p=(volatile word*)*(volatile unsigned word*)0x100;word before=*p;*p=19;return before+*p;}"}) {
        Lexer lexer(source); const auto tokens=lexer.scanTokens(); Parser parser(tokens); auto ast=parser.parseProgram();
        DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; const auto module=lowerer.lower(ast);
        CompilerConfig config; config.optimization=OptimizationLevel::Baseline;
        IRCodeGenerator baseline(analyzer.getAllLocalSymbols(),analyzer.getFunctionSymbols(),data,config);
        const auto original=executable(baseline.generate(module));
        for (const auto level : {OptimizationLevel::O2,OptimizationLevel::Size}) {
            config.optimization=level; IRCodeGenerator generator(analyzer.getAllLocalSymbols(),analyzer.getFunctionSymbols(),data,config);
            const auto optimized=executable(generator.generate(module));
            for (const unsigned address : {0u,1u,0x200u,0xfffeu}) {
                DiscoGSU::Machine a(original,0x8000),b(optimized,0x8000);
                for (auto* m : {&a,&b}) {
                    m->seed(0x700100,static_cast<std::uint8_t>(address),false);
                    m->seed(0x700101,static_cast<std::uint8_t>(address >> 8),false);
                    m->seed(0x700200,7,false); m->run();
                }
                require(a.reg(6)==b.reg(6) && (a.reg(6) || a.reg(0)==b.reg(0)) && a.word(0x700200)==b.word(0x700200),
                    "Near pointer snapshot escaped an alias, call or volatile fence");
                if (std::string(source).find("volatile word* p=") != std::string::npos && address==0x200)
                    require(b.accesses(0x700200,false)==2 && b.accesses(0x700200,true)==1 && b.reg(0)==26,
                        "Near pointer forwarding eliminated a volatile access");
            }
        }
    }
}

// A near RAM pointer to a byte-aligned pointee has no value check left at
// O2/Os, so its parameter load is as hoistable as an integer's. A word
// pointee keeps its alignment guard, and with it the original load point.
void pointerParameterLoads() {
    const auto loopLoads = [](const std::string& source) {
        Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
        DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; auto module = lowerer.lower(ast);
        IRGlobalOptimizer::run(module, analyzer.getAllLocalSymbols(), OptimizationLevel::O2);
        std::size_t loads = 0;
        for (const auto& f : module.functions) {
            std::set<std::uint32_t> parameter_addresses;
            for (const auto& b : f.blocks) for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::Address && i.operation.empty() && i.result.isValid())
                    for (const auto& parameter : f.parameters)
                        if (parameter.name.lexeme == i.symbol && parameter.type.pointer_level) parameter_addresses.insert(i.result.value);
            for (const auto& b : f.blocks) if (b.id.value != f.entry.value) for (const auto& i : b.instructions)
                loads += i.opcode == IROpcode::LoadIndirect && parameter_addresses.count(i.operands.at(0).value);
        }
        return loads;
    };
    require(loopLoads("void fill(u8* d,u16 n){for(u16 i=0;i<n;i++) d[i]=7;}") == 0,
        "A byte pointer parameter was reloaded inside its loop");
    require(loopLoads("void fill(word* d,u16 n){for(u16 i=0;i<n;i++) d[i]=7;}") != 0,
        "A checked word pointer parameter load was hoisted into an untaken path");

    // `main` is defined first: these fixtures start executing at offset 0.
    const std::string source =
        "void copy(u8* d,const u8* s,u16 n);"
        "word main(){u16 n=*(volatile u16*)0x100;u16 d=*(volatile u16*)0x102;u16 s=*(volatile u16*)0x104;"
        "copy((u8*)d,(const u8*)s,n);copy((u8*)(d+n),(const u8*)s,n);return 5;}"
        "void copy(u8* d,const u8* s,u16 n){for(u16 i=0;i<n;i++) d[i]=s[i];}";
    const auto original = executable(compileAt(source, OptimizationLevel::Baseline));
    struct Input { std::uint16_t count, destination, source; };
    // Destinations stay clear of the stack at the top of the bank; a source
    // may run into the bank end and take the carry fault.
    const Input inputs[] = {{0,0,0}, {0,0x300,0}, {4,0x300,0x280}, {1,0x300,0x280}, {4,0,0x280}, {4,0x300,0},
                            {4,0x300,0xfffe}};
    for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto optimized = executable(compileAt(source, level));
        for (const auto& input : inputs) {
            DiscoGSU::Machine a(original, 0x8000), b(optimized, 0x8000);
            for (auto* m : {&a, &b}) {
                const std::uint16_t words[] = {input.count, input.destination, input.source};
                for (unsigned n = 0; n < 3; ++n) {
                    m->seed(0x700100 + 2 * n, static_cast<std::uint8_t>(words[n]), false);
                    m->seed(0x700101 + 2 * n, static_cast<std::uint8_t>(words[n] >> 8), false);
                }
                for (unsigned n = 0; n < 8; ++n) m->seed(0x700280 + n, static_cast<std::uint8_t>(0x51 + n), false);
                m->run();
            }
            require(a.reg(6) == b.reg(6) && (a.reg(6) || a.reg(0) == b.reg(0)),
                "Byte pointer parameter promotion changed a result or fault category");
            for (std::uint32_t p = 0x700300; p < 0x700310; ++p)
                require(a.byte(p) == b.byte(p), "Byte pointer parameter promotion changed copied memory");
            if (!input.count) require(b.reg(6) == 0 && b.reg(0) == 5, "A zero-trip byte copy faulted on its unused pointers");
        }
    }
}

void multiplyProofsAndExecution() {
    Builder b;
    const auto input=b.add(IROpcode::Call,word()), full=b.literal(32767), zero=b.literal(0);
    const auto low=b.add(IROpcode::Binary,word(),{input,b.literal(127)},"&");
    const auto signed_byte=b.add(IROpcode::Binary,word(),{low,b.literal(64)},"-");
    const auto unsigned_byte=b.add(IROpcode::Binary,word(),{input,b.literal(255)},"&");
    const auto signed_product=b.add(IROpcode::Binary,word(),{signed_byte,low},"*");
    const auto unsigned_product=b.add(IROpcode::Binary,word(),{unsigned_byte,unsigned_byte},"*");
    const auto mixed=b.add(IROpcode::Binary,word(),{signed_byte,unsigned_byte},"*");
    const auto small_product=b.add(IROpcode::Binary,word(),{full,zero},"*"); b.run();
    const auto kind=[&](IRValueId v){return b.proof.multiplyKind(b.function.blocks[0].instructions.at(v.value-1));};
    require(kind(signed_product)==GSUAddressProof::MultiplyKind::SignedByte &&
        kind(unsigned_product)==GSUAddressProof::MultiplyKind::UnsignedByte,"Byte multiplication lost its signed/unsigned operand proof");
    require(kind(mixed)==GSUAddressProof::MultiplyKind::FullWord && kind(small_product)==GSUAddressProof::MultiplyKind::FullWord,
        "A mixed-range input or small product justified truncating a wide operand");
    struct Fixture { const char* expression; const char* instruction; };
    for (const auto& fixture : {Fixture{"(a&127)*((b&127)-64)","mult "},
            Fixture{"(a&255)*(b&255)","umult "},Fixture{"a*(b&1)",".byte $9F ; explicit encoding"},
            Fixture{"((a&127)-64)*(b&255)",".byte $9F ; explicit encoding"},Fixture{"a*b",".byte $9F ; explicit encoding"}}) {
        const std::string source=std::string("word main(){word a=*(volatile word*)0x100;word b=*(volatile word*)0x106;return ")+fixture.expression+";}";
        Lexer lexer(source); const auto tokens=lexer.scanTokens(); Parser parser(tokens); auto ast=parser.parseProgram();
        DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; const auto module=lowerer.lower(ast);
        CompilerConfig config; config.optimization=OptimizationLevel::Baseline;
        IRCodeGenerator baseline(analyzer.getAllLocalSymbols(),analyzer.getFunctionSymbols(),data,config);
        const auto original=executable(baseline.generate(module));
        for (const auto level : {OptimizationLevel::O1,OptimizationLevel::O2,OptimizationLevel::Size}) {
            config.optimization=level; IRCodeGenerator generator(analyzer.getAllLocalSymbols(),analyzer.getFunctionSymbols(),data,config);
            const auto object=generator.generate(module);
            const auto assembly=AssemblyGenerator(object).generate();
            if (isGlobalOptimization(level) && assembly.find(fixture.instruction)==std::string::npos)
                throw std::runtime_error(std::string("Selected multiply instruction disagrees with the actual operand ranges: ")+
                    fixture.expression+"\n"+assembly);
            for (const unsigned a : {0u,1u,127u,128u,255u,256u,32767u,32768u,65535u})
                for (const unsigned bvalue : {0u,1u,127u,128u,255u,32768u,65535u}) {
                    DiscoGSU::Machine x(original,0x8000),y(executable(object),0x8000);
                    for (auto* m : {&x,&y}) {
                        m->seed(0x700100,static_cast<std::uint8_t>(a),false);m->seed(0x700101,static_cast<std::uint8_t>(a>>8),false);
                        m->seed(0x700106,static_cast<std::uint8_t>(bvalue),false);m->seed(0x700107,static_cast<std::uint8_t>(bvalue>>8),false);m->run();
                    }
                    require(x.reg(0)==y.reg(0) && y.reg(6)==0,"Byte/full multiply selection changed modular word semantics");
                }
        }
    }
}
}

struct IRCodeGeneratorTestAccess {
    static void stackCreditFences() {
        DataSegmentManager data; std::map<std::string, Analyzer::LocalSymbolTable> locals;
        std::map<std::string, FunctionSymbol> globals; CompilerConfig config;
        config.optimization = OptimizationLevel::O2;
        IRCodeGenerator backend(locals, globals, data, config);
        const Token source(TokenType::UNKNOWN, "", 0, 0);
        backend.emitStackGuard(8, source);
        backend.emitPush(0); backend.emitPop(0); backend.emitStackGuard(8, source);
        require(backend.m_object_file.relocation_table.size() == 1,
            "Straight-line balanced stack traffic lost its existing capacity proof");
        backend.bindLabel("local.join");
        require(!backend.m_stack_check_credit.covers(1), "Local-label entry retained path-specific stack credit");
        backend.emitPush(0); backend.emitPop(0);
        require(backend.m_object_file.relocation_table.size() == 2, "Local-label entry suppressed its first stack guard");
        const IRBasicBlock block{IRBlockId{0}, "cfg.join", {}};
        backend.emitBlock(block);
        require(!backend.m_stack_check_credit.covers(1), "IR-block entry retained predecessor-specific stack credit");
        backend.emitPush(0);
        require(backend.m_object_file.relocation_table.size() == 3, "IR-block entry suppressed its first stack guard");
    }

    static void pointerIndexCopies() {
        unsigned tested = 0;
        for (const auto level : {OptimizationLevel::O1,OptimizationLevel::O2,OptimizationLevel::Size})
            for (unsigned kind = 0; kind < 13; ++kind) {
                Builder b;
                const auto input_address = b.literal(0x100,pointer());
                const auto input = b.add(IROpcode::LoadIndirect,word(),{input_address});
                const auto mask = b.literal(31);
                auto index_type = word(kind == 1);
                if (kind == 2 || kind == 3) { index_type = byte(); index_type.is_unsigned = kind == 3; }
                if (kind == 4) index_type = Type{BaseType::BOOL,"",1,false};
                if (kind == 12) index_type.array_size = 2;
                auto index = kind == 5 ? mask : b.add(IROpcode::Binary,index_type,{input,mask},"&");
                const bool far = kind == 7;
                const auto base_type = pointer(word(),kind == 8 ? AddressSpace::ROM : AddressSpace::RAM,far);
                const auto base = b.literal(far ? 0x710400 : kind == 8 ? 0x8000 : kind == 6 ? 0xfffe : 0x400,base_type);
                const auto result = b.add(IROpcode::PointerOffset,base_type,{base,index},"+",2);
                const auto read = b.add(IROpcode::LoadIndirect,word(),{result});
                IRInstruction ret; ret.opcode = IROpcode::Return; ret.type = word(); ret.operands = {read};
                b.function.blocks[0].instructions.push_back(ret);
                const auto offset = b.function.blocks[0].instructions.at(result.value - 1);
                DataSegmentManager data; std::map<std::string,Analyzer::LocalSymbolTable> locals;
                std::map<std::string,FunctionSymbol> globals; CompilerConfig config; config.optimization = level;
                IRCodeGenerator backend(locals,globals,data,config); backend.m_current_function = &b.function;
                backend.m_checked_pointer_mode = true; backend.m_emitting_value = result;
                backend.m_register_allocator.runGlobal(b.function,{5},OptimizationLevel::Size);
                for (const auto& i : b.function.blocks[0].instructions) {
                    if (i.result.isValid()) backend.m_values.emplace(i.result.value,&i);
                    for (const auto value : i.operands) ++backend.m_use_counts[value.value];
                }
                backend.m_address_proof.run(b.function,b.locals,44,true);
                const auto* home = backend.m_register_allocator.find(index);
                if (kind <= 4 || kind >= 9) require(home && home->has_register,
                    "Pointer-index copy fixture lost the allocated index home");
                if (kind == 9) backend.m_spill_offsets[index.value] = -8;
                if (kind == 10) {
                    IRCodeGenerator::RegionalLoopValues regional; regional.blocks.insert(0); regional.values.push_back({index,4});
                    backend.m_regional_loop_values.emplace(1,std::move(regional));
                }
                if (kind == 11) backend.m_ephemeral_values.insert(index.value);
                backend.m_direct_register_load = true;
                backend.emitPointerOffset(offset);
                const auto& bytes = backend.m_object_file.code_section;
                const bool direct = home && bytes.size() >= 2 && bytes[0] == static_cast<std::uint8_t>(0x20 | home->physical_register) && bytes[1] == 0x16;
                // Os keeps the proof requirement; O2 also stages an unproved
                // word index (kind 6), whose checks read the same R6 value.
                const bool expected = (level == OptimizationLevel::Size && kind <= 1) ||
                    (level == OptimizationLevel::O2 && (kind <= 1 || kind == 6));
                require(direct == expected,
                    "Direct pointer index copy crossed type/proof/register-home/policy boundaries");
                require(!backend.m_direct_register_load, "Base materialization did not reset direct-load emitter state");
                ++tested;
            }
        std::cout << "Pointer index copy: " << tested << " policy/type/proof/home structural cases\n";
    }
};

int main() {
    try {
        IRCodeGeneratorTestAccess::stackCreditFences();
        IRCodeGeneratorTestAccess::pointerIndexCopies();
        frameRanges(); staticObjectRanges(); staticObjectCodegen(); staticRomObjectRanges(); staticRomObjectCodegen(); singleSiteInlineExecution(); registerArgumentExecution(); publicRegisterEntryExecution(); staticFrameExecution(); framelessExecution(); shortRamExecution(); staticStackProofCodegen(); pointerBoundaries(); wrappedAndCyclicValues(); inductionIntervals(); boundedCastsAndConditions();
        stackCredit(); verifiedSource(); verifiedClearLoop(); stackCreditControlFlowExecution(); constantOffsetExecution(); pointerComponentExecution(); pointerSnapshotExecution(); pointerParameterLoads(); multiplyProofsAndExecution();
        std::cout << "GSU checked-address and stack-credit proofs passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
