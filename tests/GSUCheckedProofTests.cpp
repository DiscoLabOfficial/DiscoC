#include "GSUAddressProof.hpp"
#include "GSUStackCheckCredit.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "IRGlobalOptimizer.hpp"

#include <iostream>
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
}
int main() {
    try {
        frameRanges(); pointerBoundaries(); wrappedAndCyclicValues(); inductionIntervals(); boundedCastsAndConditions();
        stackCredit(); verifiedSource(); verifiedClearLoop();
        std::cout << "GSU checked-address and stack-credit proofs passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
