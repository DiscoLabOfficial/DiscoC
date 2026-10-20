#include "GSUAddressProof.hpp"
#include "GSUStackCheckCredit.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "IRGlobalOptimizer.hpp"
#include "IRCodeGenerator.hpp"
#include "GSUInstructionModel.hpp"
#include "ABI.hpp"
#include "AssemblyGenerator.hpp"

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

std::vector<std::uint8_t> executable(const ObjectFile& object, std::uint16_t stack_floor = 0) {
    auto bytes = object.code_section;
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
        require(found, "Missing checked-offset fixture relocation");
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
                require(direct == (level == OptimizationLevel::Size && kind <= 1),
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
        frameRanges(); pointerBoundaries(); wrappedAndCyclicValues(); inductionIntervals(); boundedCastsAndConditions();
        stackCredit(); verifiedSource(); verifiedClearLoop(); stackCreditControlFlowExecution(); constantOffsetExecution(); pointerComponentExecution(); pointerSnapshotExecution(); multiplyProofsAndExecution();
        std::cout << "GSU checked-address and stack-credit proofs passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
