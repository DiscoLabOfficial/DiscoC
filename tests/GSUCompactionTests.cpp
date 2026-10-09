#include "IRCodeGenerator.hpp"
#include "IRGlobalOptimizer.hpp"
#include "IRCompactOptimizer.hpp"
#include "AssemblyGenerator.hpp"
#include "Assembler.hpp"
#include "GSUInstructionModel.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Optimizer.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool b, const char* text) { if (!b) throw std::runtime_error(text); }
struct Compilation { ObjectFile object; IRModule ir; };
Compilation compile(const std::string& source, OptimizationLevel level, std::uint32_t origin, int minimum_frame_bytes = 0) {
    Lexer lexer(source); auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer;
    auto ir = lowerer.lower(ast);
    if (minimum_frame_bytes) {
        require(ir.functions.size() == 1, "Reserved-frame fixture must have one function");
        ir.functions[0].total_local_alloc_size = std::max(ir.functions[0].total_local_alloc_size, minimum_frame_bytes);
    }
    auto optimized = ir;
    if (isGlobalOptimization(level)) {
        IRGlobalOptimizer::run(optimized, analyzer.getAllLocalSymbols(), level);
        const auto first = dumpIR(optimized); IRGlobalOptimizer::run(optimized, analyzer.getAllLocalSymbols(), level);
        if (first != dumpIR(optimized)) throw std::runtime_error("Compaction/global pipeline is not idempotent\n" + source +
            "\nFirst:\n" + first + "Second:\n" + dumpIR(optimized));
    }
    CompilerConfig config; config.optimization = level; config.code_start_address = origin;
    IRCodeGenerator backend(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
    auto object = backend.generate(ir);
    const auto assembly = AssemblyGenerator(object).generate(); Assembler assembler;
    const auto round_trip = assembler.assemble(assembly);
    require(round_trip.code_section == object.code_section, "Compact assembly round trip changed bytes");
    IRCodeGenerator again(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
    require(assembly == AssemblyGenerator(again.generate(ir)).generate(), "Compaction is nondeterministic");
    return {std::move(object), std::move(optimized)};
}
std::vector<std::uint8_t> linked(const ObjectFile& object) {
    auto bytes = object.code_section;
    for (const auto& relocation : object.relocation_table) {
        bool found = false;
        const std::string prefix = "__disco_stack_limit_";
        if (relocation.target_symbol_name.compare(0, prefix.size(), prefix) == 0) {
            const auto required = static_cast<unsigned>(std::stoul(relocation.target_symbol_name.substr(prefix.size())));
            require(relocation.type == RelocationType::ADDR16_RAM && required <= 65535, "Invalid stack-limit fixture");
            bytes.at(relocation.patch_offset + 1) = static_cast<std::uint8_t>(required);
            bytes.at(relocation.patch_offset + 2) = static_cast<std::uint8_t>(required >> 8); found = true;
        }
        for (const auto& symbol : object.symbol_table) if (symbol.name == relocation.target_symbol_name) {
            require(symbol.section == SymbolSection::CODE &&
                (relocation.type == RelocationType::ADDR16_IWT || relocation.type == RelocationType::ADDR16_JAL),
                "Unexpected compaction fixture relocation");
            const auto value = object.config.code_start_address + symbol.offset;
            bytes.at(relocation.patch_offset + 1) = static_cast<std::uint8_t>(value);
            bytes.at(relocation.patch_offset + 2) = static_cast<std::uint8_t>(value >> 8); found = true;
        }
        if (!found) throw std::runtime_error("Missing compact loop relocation symbol: " + relocation.target_symbol_name);
    }
    return bytes;
}
std::size_t count(const IRModule& m, IROpcode op) {
    std::size_t n = 0; for (const auto& f : m.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions) n += i.opcode == op;
    return n;
}
void execution(const std::string& source, std::uint16_t result, bool cache = false, bool compact = false) {
    for (const auto origin : {0x008000u, 0x706007u}) for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto optimized = compile(source, level, origin), baseline = compile(source, OptimizationLevel::O1, origin);
        for (unsigned seed : {0u, 1u, 3u}) {
            DiscoGSU::Machine a(linked(optimized.object), origin), b(linked(baseline.object), origin);
            a.seed(0x700100, static_cast<std::uint8_t>(seed), false); b.seed(0x700100, static_cast<std::uint8_t>(seed), false);
            a.run(); b.run();
            require(a.reg(0) == result && a.reg(0) == b.reg(0), "Compact program changed its return value");
            require(a.reg(6) == 0 && a.reg(10) == b.reg(10), "Compaction damaged stack/fault state");
            require(a.reg(12) == b.reg(12) && a.reg(13) == b.reg(13), "Generated LOOP did not restore R12/R13");
            for (unsigned p = 0; p < 1024; ++p)
                require(a.byte(0x700000 + p) == b.byte(0x700000 + p), "Compaction changed externally visible RAM");
            if (cache && (level == OptimizationLevel::O2 || compact))
                if (a.cacheRequests() == 0) throw std::runtime_error("Profitable small generated loop has no automatic CACHE: " + source +
                    "\n" + dumpIR(optimized.ir) + "\n" + AssemblyGenerator(optimized.object).generate());
        }
        if (compact) require(optimized.object.code_section.size() < baseline.object.code_section.size(), "Initializer compaction did not save bytes");
    }
}
void reject(IRModule m, const char* expected) {
    try { IRVerifier::verify(m); } catch (const CompilerError& e) {
        require(e.getMessage().find(expected) != std::string::npos, "Wrong compact IR diagnostic"); return;
    }
    throw std::runtime_error("Malformed compact IR was accepted");
}
void dynamicLoops() {
    const std::string span = "word main(){word n=*((volatile word*)0x100);word r=10;plot{at(4,3);color 2;"
        "@cache for(word i=0;i<n;i++){r++;pixel;}flush;return r+cursor.x;}}";
    for (const auto origin : {0x008000u, 0x706007u}) for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto optimized = compile(span, level, origin), baseline = compile(span, OptimizationLevel::O1, origin);
        require(count(optimized.ir, IROpcode::HardwareLoop) == 1, "Dynamic cached span was not converted to LOOP");
        const bool emitted_loop = AssemblyGenerator(optimized.object).generate().find("\n    loop ;") != std::string::npos;
        require(level != OptimizationLevel::O2 || emitted_loop, "O2 did not emit the converted dynamic LOOP");
        for (const auto n : std::array<std::uint16_t, 7>{{0, 1, 3, 112, 127, 0xffff, 0x8000}}) {
            DiscoGSU::Machine a(linked(optimized.object), origin), b(linked(baseline.object), origin);
            for (auto* machine : {&a, &b}) {
                machine->seed(0x700100, static_cast<std::uint8_t>(n), false);
                machine->seed(0x700101, static_cast<std::uint8_t>(n >> 8), false);
                machine->run();
            }
            const unsigned iterations = n < 0x8000 ? n : 0;
            require(a.reg(0) == 14 + 2 * iterations && a.reg(0) == b.reg(0), "Dynamic LOOP mishandled zero/signed count or live-out value");
            require(a.reg(6) == 0 && a.reg(10) == b.reg(10), "Dynamic LOOP damaged stack/fault state");
            require(a.reg(12) == b.reg(12) && a.reg(13) == b.reg(13), "Dynamic LOOP failed to restore scoped loop registers");
            for (unsigned p = 0; p < 1024; ++p)
                require(a.byte(0x700000 + p) == b.byte(0x700000 + p), "Dynamic LOOP changed pixel/cache output");
            // The initial edge executes the original CACHE at its exact byte;
            // backedges retain PHI copies but skip this redundant request.
            require(a.cacheRequests() == (emitted_loop && !iterations ? 0u : 1u),
                "Dynamic LOOP lost the explicit CACHE hint or entered a zero count");
        }
    }
    // The final induction value is also a live-out. A negative start and a
    // positive end exercise unsigned count construction for signed words.
    execution("word main(){word n=(word)*((volatile u8*)0x100);word i=-3;word r=0;"
        "for(;i<n;i++){r++;}return r-i;}", 3);
    const std::string signed_distance = "word main(){word i=*((volatile word*)0x100);word end=*((volatile word*)0x102);"
        "word count=0;word sum=5;for(;i<end;i++){count++;sum+=i;}"
        "*((volatile word*)0x104)=count;*((volatile word*)0x106)=i;return sum;}";
    const auto signed_loop = compile(signed_distance, OptimizationLevel::O2, 0x008000);
    const auto signed_baseline = compile(signed_distance, OptimizationLevel::O1, 0x008000);
    require(count(signed_loop.ir, IROpcode::HardwareLoop) == 1, "Runtime signed start/end were not converted");
    for (const auto& range : {std::make_pair(0x8000u, 0x7fffu), std::make_pair(0xfffdu, 3u),
                            std::make_pair(3u, 0xfffdu), std::make_pair(0x8000u, 0x8000u)}) {
        DiscoGSU::Machine a(linked(signed_loop.object), 0x008000), b(linked(signed_baseline.object), 0x008000);
        for (auto* machine : {&a, &b}) {
            machine->seed(0x700100, static_cast<std::uint8_t>(range.first), false);
            machine->seed(0x700101, static_cast<std::uint8_t>(range.first >> 8), false);
            machine->seed(0x700102, static_cast<std::uint8_t>(range.second), false);
            machine->seed(0x700103, static_cast<std::uint8_t>(range.second >> 8), false);
            machine->run();
        }
        require(a.reg(0) == b.reg(0) && a.reg(10) == b.reg(10) && a.reg(12) == b.reg(12) && a.reg(13) == b.reg(13),
            "Signed crossing LOOP changed multiple carried live-outs or register restoration");
        for (unsigned p = 0; p < 8; ++p)
            require(a.byte(0x700100 + p) == b.byte(0x700100 + p), "Signed crossing LOOP changed induction/count observability");
    }
    execution("word main(){word n=(word)*((volatile u8*)0x100);word r=0;"
        "for(word i=0;i<=7;i++){r+=n+1;}return r/(n+1);}", 8);
    for (const auto source : {
        "word main(){word n=*((volatile word*)0x100);word r=0;for(word i=0;i<=n;i++){r++;}return r;}",
        "word main(){word r=0;for(word i=32760;i<=32767;i++){r++;}return r;}",
        "word main(){unsigned word r=0;for(unsigned word i=65528;i<=65535;i++){r++;}return (word)r;}"})
        require(count(compile(source, OptimizationLevel::O2, 0x008000).ir, IROpcode::HardwareLoop) == 0,
            "Potentially wrapping inclusive loop was converted to a finite count");
    const auto unsigned_span = compile("word main(){unsigned word n=*((volatile unsigned word*)0x100);unsigned word r=0;"
        "for(unsigned word i=0;i<n;i++){r++;}return (word)r;}", OptimizationLevel::O2, 0x008006);
    require(count(unsigned_span.ir, IROpcode::HardwareLoop) == 1, "Unsigned strict-bound loop was not converted");
    auto seeded_code = linked(unsigned_span.object);
    // Start before the function with live caller LOOP registers. Relocations
    // were linked at $8006, so this literal prelude changes no payload address.
    seeded_code.insert(seeded_code.begin(), {0xfc, 0xef, 0xbe, 0xfd, 0xfe, 0xca});
    for (const auto n : std::array<std::uint16_t, 4>{{0, 1, 0x8000, 0xffff}}) {
        DiscoGSU::Machine a(seeded_code, 0x008000);
        a.seed(0x700100, static_cast<std::uint8_t>(n), false); a.seed(0x700101, static_cast<std::uint8_t>(n >> 8), false); a.run();
        require(a.reg(0) == n && a.reg(6) == 0 && a.reg(10) == 0x1ffc && a.reg(12) == 0xbeef && a.reg(13) == 0xcafe,
            "Unsigned LOOP truncated its 16-bit count or failed to exit cleanly");
    }
}

void standaloneCountdowns() {
    // Public pass inputs are verified IR, not necessarily CFG chains already
    // contracted by SCCP. The PHI's update can legally precede its latch.
    for (const bool latch_update : {false, true}) {
        IRModule module;
        IRFunction function; function.name = "main"; function.entry = IRBlockId{0}; function.value_count = 6;
        const Type word{BaseType::WORD, "", 2, false}, boolean{BaseType::BOOL, "", 1, false};
        function.return_type = word;
        for (std::uint32_t block = 0; block < 5; ++block)
            function.blocks.push_back({IRBlockId{block}, "b" + std::to_string(block), {}});
        const auto branch = [](std::uint32_t target) {
            IRInstruction instruction; instruction.opcode = IROpcode::Branch; instruction.targets = {IRBlockId{target}};
            return instruction;
        };
        IRInstruction zero; zero.opcode = IROpcode::Constant; zero.type = word; zero.result = IRValueId{1};
        IRInstruction limit = zero; limit.result = IRValueId{2}; limit.immediate = 128;
        IRInstruction one = zero; one.result = IRValueId{3}; one.immediate = 1;
        function.blocks[0].instructions = {zero, limit, one, branch(1)};
        IRInstruction phi; phi.opcode = IROpcode::Phi; phi.type = word; phi.result = IRValueId{4};
        phi.operands = {zero.result, IRValueId{6}}; phi.targets = {IRBlockId{0}, IRBlockId{3}};
        IRInstruction condition; condition.opcode = IROpcode::Binary; condition.type = boolean;
        condition.result = IRValueId{5}; condition.operation = "<"; condition.operands = {phi.result, limit.result};
        IRInstruction test; test.opcode = IROpcode::CondBranch; test.operands = {condition.result};
        test.targets = {IRBlockId{2}, IRBlockId{4}};
        function.blocks[1].instructions = {phi, condition, test};
        IRInstruction update; update.opcode = IROpcode::Binary; update.type = word; update.result = IRValueId{6};
        update.operation = "+"; update.operands = {phi.result, one.result};
        function.blocks[2].instructions = {branch(3)};
        function.blocks[3].instructions = {branch(1)};
        auto& updates = function.blocks[latch_update ? 3 : 2].instructions;
        updates.insert(updates.begin(), update);
        IRInstruction ret; ret.opcode = IROpcode::Return; ret.operands = {zero.result};
        function.blocks[4].instructions = {ret}; module.functions.push_back(std::move(function));
        IRVerifier::verify(module);
        const auto before = dumpIR(module);
        IRCompactOptimizer::run(module, {}, OptimizationLevel::Size);
        IRVerifier::verify(module);
        require(count(module, IROpcode::HardwareLoop) == 0, "Standalone countdown borrowed hardware loop registers");
        if (latch_update) require(dumpIR(module).find("binary !=") != std::string::npos,
            "Standalone latch-local induction did not become a countdown");
        else require(dumpIR(module) == before, "Standalone countdown changed an uncontracted update/body chain");
        const auto first = dumpIR(module); IRCompactOptimizer::run(module, {}, OptimizationLevel::Size);
        require(dumpIR(module) == first, "Standalone countdown is not idempotent");
    }
}

void sizeCountdownsAndFlags() {
    for (const bool unsigned_count : {false, true}) {
        const std::string type = unsigned_count ? "u16" : "word";
        const std::string source = "word main(){" + type + " begin=*((volatile " + type + "*)0x100);" +
            type + " end=*((volatile " + type + "*)0x102);plot{at(4,3);color 2;for(" +
            type + " i=begin;i<end;i++){pixel;}flush;return cursor.x;}}";
        for (const auto origin : {0x008000u, 0x706007u}) {
            const auto compact = compile(source, OptimizationLevel::Size, origin);
            const auto baseline = compile(source, OptimizationLevel::O1, origin);
            require(count(compact.ir, IROpcode::HardwareLoop) == 0 && dumpIR(compact.ir).find("binary !=") != std::string::npos,
                "Unused induction did not become a software countdown");
            for (const auto& range : {std::make_pair(0u, 0u), std::make_pair(0u, 1u), std::make_pair(0u, 128u),
                    std::make_pair(7u, 3u), std::make_pair(0xfffdu, 3u), std::make_pair(0x8000u, 0x7fffu),
                    std::make_pair(0u, 0xffffu)}) {
                DiscoGSU::Machine a(linked(compact.object), origin), b(linked(baseline.object), origin);
                for (auto* machine : {&a, &b}) {
                    machine->seed(0x700100, static_cast<std::uint8_t>(range.first), false);
                    machine->seed(0x700101, static_cast<std::uint8_t>(range.first >> 8), false);
                    machine->seed(0x700102, static_cast<std::uint8_t>(range.second), false);
                    machine->seed(0x700103, static_cast<std::uint8_t>(range.second >> 8), false);
                    machine->run();
                }
                const auto value = [&](unsigned bits) { return !unsigned_count && bits >= 0x8000u ? static_cast<int>(bits) - 65536 : static_cast<int>(bits); };
                const auto trips = std::max(0, value(range.second) - value(range.first));
                require(a.reg(0) == static_cast<std::uint16_t>(4 + trips) && a.reg(0) == b.reg(0) &&
                    a.graphicsState("--plots") == static_cast<unsigned>(trips) && a.graphicsState("--rpix") == 1,
                    "Software countdown changed signed/unsigned trips, PLOT increment or flush");
                require(a.reg(6) == 0 && a.reg(10) == b.reg(10) && a.reg(12) == b.reg(12) && a.reg(13) == b.reg(13),
                    "Software countdown changed fault/stack/scoped LOOP state");
                for (unsigned p = 0; p < 0x1f00; ++p)
                    require(a.byte(0x700000 + p) == b.byte(0x700000 + p), "Software countdown changed framebuffer/RAM");
            }
        }
    }
    for (const auto& source : {
            "word main(){u16 x=*((volatile u16*)0x100);u16 y=x+1;if(y==0)return 31;return 47;}",
            "word main(){u16 x=*((volatile u16*)0x100);u16 y=x-1;if(y!=0)return 47;return 31;}",
            "word main(){u16 x=*((volatile u16*)0x100);u16 y=x-1;*((volatile word*)0x104)=11;if(y==0)return 31;return 47;}"}) {
        for (const auto origin : {0x008000u, 0x706007u}) for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
            const auto compact = compile(source, level, origin), baseline = compile(source, OptimizationLevel::O1, origin);
            for (const unsigned seed : {0u, 1u, 2u, 127u, 128u, 32767u, 32768u, 65535u}) {
                DiscoGSU::Machine a(linked(compact.object), origin), b(linked(baseline.object), origin);
                for (auto* machine : {&a, &b}) {
                    machine->seed(0x700100, static_cast<std::uint8_t>(seed), false);
                    machine->seed(0x700101, static_cast<std::uint8_t>(seed >> 8), false); machine->run();
                }
                require(a.reg(0) == b.reg(0) && a.reg(6) == 0 && a.reg(10) == b.reg(10) &&
                    a.byte(0x700104) == b.byte(0x700104), "INC/DEC or zero-flag reuse crossed wrap or an intervening store");
            }
        }
    }
    // Borrowing the counter would change source values; these stay ordinary.
    const auto used = compile("word main(){plot{for(word i=0;i<128;i++){color i;pixel;}flush;}return 0;}", OptimizationLevel::Size, 0x008000);
    require(count(used.ir, IROpcode::HardwareLoop) == 0, "Observable induction was discarded by the size countdown");
}

void sizeDivisionCache() {
    for (const bool explicit_cache : {false, true}) for (const auto origin : {0x00800fu, 0x706007u}) {
        const std::string source = std::string(explicit_cache ? "@cache " : "") +
            "word main(){u16 a=*((volatile u16*)0x100);u16 b=*((volatile u16*)0x102);"
            "*((volatile word*)0x104)=11;return (word)(a/b);}";
        const auto compact = compile(source, OptimizationLevel::Size, origin), baseline = compile(source, OptimizationLevel::O1, origin);
        for (const auto& values : {std::make_pair(65535u, 3u), std::make_pair(32768u, 1u),
                                 std::make_pair(0u, 13u), std::make_pair(149u, 0u)}) {
            DiscoGSU::Machine a(linked(compact.object), origin), b(linked(baseline.object), origin);
            for (auto* machine : {&a, &b}) {
                machine->seed(0x700100, static_cast<std::uint8_t>(values.first), false);
                machine->seed(0x700101, static_cast<std::uint8_t>(values.first >> 8), false);
                machine->seed(0x700102, static_cast<std::uint8_t>(values.second), false);
                machine->seed(0x700103, static_cast<std::uint8_t>(values.second >> 8), false); machine->run();
            }
            require(a.reg(6) == b.reg(6) && a.reg(6) == (values.second ? 0u : 6u) && a.byte(0x700104) == 11,
                "Size CACHE removed or reordered divide-by-zero fault/witness");
            if (values.second) require(a.reg(0) == values.first / values.second && a.reg(0) == b.reg(0) && a.reg(10) == b.reg(10),
                "Size CACHE changed division or stack restoration");
            require(a.cacheRequests() == 1, "Size division did not reuse a NOP, or rebased explicit CACHE");
        }
    }
}

void inlineHardwareScopes() {
    // These are verified IR contracts, not source patterns: the AST optimizer
    // intentionally declines nested/calling countdown loops. Both implicit
    // counters and the caller's pre-existing R12/R13 must survive lowering.
    for (const bool calls : {false, true}) {
        const std::string source = calls
            ? "word step(); word main(){volatile word total=0;total+=step();return total;}"
            : "word main(){volatile word total=0;total++;return total;}";
        Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
        DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); Optimizer optimizer; optimizer.optimize(ast);
        IRLowerer lowerer; auto ir = lowerer.lower(ast);
        auto& function = ir.functions.front();
        require(function.name == "main" && function.blocks.size() == 1, "Inline scope fixture lost its single-block entry");
        std::vector<IRInstruction> body;
        unsigned stores = 0;
        for (const auto& instruction : function.blocks[0].instructions) {
            body.push_back(instruction);
            if (instruction.opcode != IROpcode::StoreIndirect) continue;
            if (++stores == 1) {
                for (unsigned loop = 1; loop <= 2; ++loop) {
                    IRInstruction count_value; count_value.opcode = IROpcode::Constant;
                    count_value.type = Type{BaseType::WORD, "", 2, false};
                    count_value.result = IRValueId{++function.value_count}; count_value.immediate = loop + 1;
                    body.push_back(count_value);
                    IRInstruction setup; setup.opcode = IROpcode::HardwareLoop;
                    setup.loop_id = loop; setup.operands = {count_value.result}; body.push_back(std::move(setup));
                }
            } else if (stores == 2) {
                for (unsigned loop : {2u, 1u}) {
                    IRInstruction end; end.opcode = IROpcode::HardwareLoopEnd;
                    end.loop_id = loop; body.push_back(std::move(end));
                }
            }
        }
        require(stores == 2, "Inline scope fixture must have initialization and one body store");
        function.blocks[0].instructions = std::move(body);
        IRVerifier::verify(ir);
        for (const auto origin : {0x008006u, 0x706006u})
            for (const auto level : {OptimizationLevel::Baseline, OptimizationLevel::O1, OptimizationLevel::O2, OptimizationLevel::Size}) {
                CompilerConfig config; config.optimization = level; config.code_start_address = origin;
                IRCodeGenerator backend(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
                auto object = backend.generate(ir);
                if (calls) {
                    // An external callee deliberately clobbers both loop
                    // registers; a compiler-generated callee would preserve
                    // them itself and could mask missing caller protection.
                    object.symbol_table.push_back({"step", SymbolSection::CODE,
                        static_cast<std::uint32_t>(object.code_section.size())});
                    object.code_section.insert(object.code_section.end(),
                        {0xfc, 0x11, 0x11, 0xfd, 0x22, 0x22, 0xa0, 2, 0x9b, 1});
                }
                require(Assembler().assemble(AssemblyGenerator(object).generate()).code_section == object.code_section,
                    "Nested inline scope assembly changed bytes");
                auto bytes = linked(object);
                bytes.insert(bytes.begin(), {0xfc, 0xef, 0xbe, 0xfd, 0xfe, 0xca});
                DiscoGSU::Machine machine(std::move(bytes), origin - 6); machine.run();
                require(machine.reg(0) == (calls ? 12 : 6) && machine.reg(6) == 0 && machine.reg(10) == 0x1ffc &&
                        machine.reg(12) == 0xbeef && machine.reg(13) == 0xcafe,
                    "Nested inline/calling LOOP failed to restore implicit counters, addresses or stack");
            }
    }
}

void cursorAndTransientValues() {
    for (const auto& source : {
        "word main(){plot{at(-1,0);cursor.x++;cursor.y--;return cursor.x+cursor.y;}}",
        "word main(){plot{at(7,3);word old=cursor.x++;word now=++cursor.y;return old+now+cursor.x;}}",
        "word main(){plot{at(7,3);pixel;cursor.x--;pixel;cursor.y++;flush;return cursor.x+cursor.y;}}",
        "word main(){word a[4]={2,3,4,5};word i=*((volatile word*)0x100)&3;plot{cursor.x=a[i]+1;cursor.y=a[i+0];return cursor.x+cursor.y;}}",
        "word main(){word a[4]={2,3,4,5};word i=*((volatile word*)0x100)&3;a[i]=11;return a[i];}"}) {
        for (const auto origin : {0x008000u, 0x70600fu}) {
            const auto baseline = compile(source, OptimizationLevel::O1, origin);
            for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
                const auto optimized = compile(source, level, origin);
                for (const auto input : {0u, 1u, 3u, 65535u}) {
                    DiscoGSU::Machine a(linked(optimized.object), origin), b(linked(baseline.object), origin);
                    for (auto* machine : {&a, &b}) {
                        machine->seed(0x700100, static_cast<std::uint8_t>(input), false);
                        machine->seed(0x700101, static_cast<std::uint8_t>(input >> 8), false); machine->run();
                    }
                    const bool plot = std::string(source).find("plot{") != std::string::npos;
                    if (!(a.reg(0) == b.reg(0) && (!plot || (a.reg(1) == b.reg(1) && a.reg(2) == b.reg(2))) &&
                        a.reg(6) == 0 && a.reg(10) == b.reg(10))) throw std::runtime_error(
                            "Cursor/transient mismatch: " + std::string(source) + " R0=" + std::to_string(a.reg(0)) + "/" + std::to_string(b.reg(0)) +
                            " R1=" + std::to_string(a.reg(1)) + "/" + std::to_string(b.reg(1)) + " R2=" + std::to_string(a.reg(2)) + "/" + std::to_string(b.reg(2)) +
                            " R6=" + std::to_string(a.reg(6)) + " SP=" + std::to_string(a.reg(10)) + "/" + std::to_string(b.reg(10)));
                    for (unsigned p = 0; p < 1024; ++p) require(a.byte(0x700000+p) == b.byte(0x700000+p),
                        "Cursor fusion/transient address changed RAM or pixels");
                }
            }
        }
    }
    const auto fused = compile("word main(){plot{at(7,3);cursor.x++;cursor.y--;return 0;}}", OptimizationLevel::O2, 0x706000);
    const auto assembly = AssemblyGenerator(fused.object).generate();
    require(assembly.find("inc r1") != std::string::npos && assembly.find("dec r2") != std::string::npos,
        "Discarded cursor updates were not selected as physical INC/DEC");
}

void cacheWindowsAndCheckedAddresses() {
    const std::string software = "word main(){volatile word sum=0;for(word i=0;i<97;i++){if(i&1)sum+=2;else sum++;}return sum;}";
    const std::string explicit_loop = "word main(){word sum=0;plot{@cache for(word i=0;i<8;i++){pixel;sum+=cursor.x;}flush;}return sum;}";
    for (unsigned alignment=0; alignment<16; ++alignment) {
        const auto origin = 0x706000u + alignment;
        for (const auto& source : {software, explicit_loop}) {
            const auto baseline = compile(source, OptimizationLevel::O1, origin);
            for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
                const auto optimized = compile(source, level, origin);
                DiscoGSU::Machine a(linked(optimized.object), origin), b(linked(baseline.object), origin); a.run(); b.run();
                require(a.reg(0) == b.reg(0) && a.reg(6) == 0 && a.reg(10) == b.reg(10) &&
                    a.reg(12) == b.reg(12) && a.reg(13) == b.reg(13), "CACHE entry/PHI backedge changed loop state");
                if (level == OptimizationLevel::O2 || source == explicit_loop)
                    require(a.cacheRequests() != 0, "Profitable ordinary/carried-PHI loop lost CACHE discovery");
                if (source == explicit_loop) require(a.cacheRequests() == 1, "Explicit loop CACHE repeated on the backedge");
                if (source == explicit_loop) {
                    const auto assembly = AssemblyGenerator(optimized.object).generate();
                    const auto cache = assembly.find("cache ; CODE+$");
                    require(cache != std::string::npos && assembly.find("cache ; CODE+$", cache+1) == std::string::npos,
                        "Explicit loop gained a competing cache window");
                    const auto offset = std::stoul(assembly.substr(cache+14), nullptr, 16);
                    require(a.cacheBase() == ((origin + offset + 1) & 0xfff0), "CACHE bypass changed its original CBR/alignment");
                }
                for (unsigned p=0; p<1024; ++p) require(a.byte(0x700000+p) == b.byte(0x700000+p), "CACHE changed pixel/RAM output");
            }
        }
    }
    const auto nested = compile("word main(){plot{@cache for(word y=0;y<3;y++){for(word i=0;i<32;i++){pixel;}}flush;}return 7;}",
        OptimizationLevel::O2, 0x706000);
    DiscoGSU::Machine nested_machine(linked(nested.object), 0x706000); nested_machine.run();
    require(nested_machine.reg(0) == 7 && nested_machine.cacheRequests() == 4,
        "Automatic CACHE rebased an enclosing explicit loop window");
    const std::string far_store = "word main(){word a=*((volatile word*)0x102);word b=*((volatile word*)0x104);"
        "word c=*((volatile word*)0x106);word* far* slot=(word* far*)(u16)(((word)*((volatile u8*)0x100)+1)*4);"
        "*slot=(far word*)0x710200;return a+b+c;}";
    for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto compiled = compile(far_store, level, 0x008000);
        DiscoGSU::Machine machine(linked(compiled.object), 0x008000);
        machine.seed(0x700100, 2, false); machine.seed(0x700102, 10, false);
        machine.seed(0x700104, 20, false); machine.seed(0x700106, 30, false); machine.run();
        require(machine.reg(0) == 60 && machine.reg(6) == 0 && machine.accesses(0x700100, false) == 1 &&
            machine.word(0x70000c) == 0x71 && machine.word(0x70000e) == 0x200,
            "Ephemeral near destination rematerialized a volatile input while storing a far pair");
    }
    // The offset may fail; eliminating its duplicate access check must not
    // erase carry/null/alignment checks or move them before this witness.
    const std::string checked = "word main(){word* p=(word*)*((volatile u16*)0x100);word i=*((volatile word*)0x102);"
        "*((volatile byte*)0x104)=(byte)11;word* q=p+i;word v=*q;*q=v+1;return *q;}";
    for (const auto input : {std::make_pair(0x200u, 1u), std::make_pair(0x200u, 0xffffu),
                            std::make_pair(0xfffeu, 1u), std::make_pair(0u, 1u), std::make_pair(0x201u, 1u)}) {
        const auto baseline = compile(checked, OptimizationLevel::O1, 0x008000);
        for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
            const auto optimized = compile(checked, level, 0x008000);
            DiscoGSU::Machine a(linked(optimized.object), 0x008000), b(linked(baseline.object), 0x008000);
            for (auto* machine : {&a, &b}) {
                machine->seed(0x700100, static_cast<std::uint8_t>(input.first), false);
                machine->seed(0x700101, static_cast<std::uint8_t>(input.first >> 8), false);
                machine->seed(0x700102, static_cast<std::uint8_t>(input.second), false);
                machine->seed(0x700103, static_cast<std::uint8_t>(input.second >> 8), false); machine->run();
            }
            require(a.reg(6) == b.reg(6) && a.byte(0x700104) == b.byte(0x700104) &&
                (a.reg(6) || (a.reg(0) == b.reg(0) && a.reg(10) == b.reg(10))),
                "Local checked-address credit changed first-fault ordering or result");
        }
    }
    const std::string wider = "word main(){byte* p=(byte*)*((volatile u16*)0x100);byte v=*p;"
        "*((volatile byte*)0x104)=v;return *((word*)p);}";
    for (const auto address : {0x200u, 0x201u, 0xffffu}) {
        const auto baseline = compile(wider, OptimizationLevel::O1, 0x008000);
        for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
            const auto optimized = compile(wider, level, 0x008000);
            DiscoGSU::Machine a(linked(optimized.object), 0x008000), b(linked(baseline.object), 0x008000);
            for (auto* machine : {&a, &b}) {
                machine->seed(0x700100, static_cast<std::uint8_t>(address), false);
                machine->seed(0x700101, static_cast<std::uint8_t>(address >> 8), false);
                machine->seed(0x700200, 42, false); machine->seed(0x700201, 17, false);
                machine->seed(0x70ffff, 23, false); machine->run();
            }
            require(a.reg(6) == b.reg(6) && a.byte(0x700104) == b.byte(0x700104) &&
                a.accesses(0x700100, false) == 1 && (a.reg(6) || a.reg(0) == b.reg(0)),
                "A byte-width address check incorrectly justified a wider/misaligned access");
        }
    }
}

void frameProofBoundary() {
    unsigned executed = 0, rejected = 0;
    for (const auto size : {65500u, 65504u, 65508u, 65512u, 65516u, 65518u, 65520u, 65522u, 65524u}) {
        const std::string source = "word main(){volatile byte padding[" + std::to_string(size) +
            "];padding[0]=1;word a=*((volatile word*)0x102);word b=*((volatile word*)0x104);"
            "word c=*((volatile word*)0x106);word* p=(word*)(u16)(0x400+(((word)*((volatile u8*)0x100)&3)<<1));"
            "*p=123;return a+b+c;}";
        try {
            const auto compiled = compile(source, OptimizationLevel::O2, 0x008000);
            DiscoGSU::Machine machine(linked(compiled.object), 0x008000, 0xfffe);
            machine.seed(0x700100, 2, false); machine.seed(0x700102, 10, false);
            machine.seed(0x700104, 20, false); machine.seed(0x700106, 30, false); machine.run();
            require(machine.reg(6) == 0 && machine.reg(0) == 60 && machine.word(0x700404) == 123 &&
                machine.accesses(0x700100, false) == 1,
                "Final frame sizing invalidated an unspilled address proof");
            ++executed;
        } catch (const CompilerError& e) {
            const auto message = e.getMessage();
            if (message.find("frame exceeds one RAM bank") == std::string::npos &&
                message != "Local stack frame exceeds the 16-bit GSU address space.")
                throw std::runtime_error("Unexpected frame-boundary diagnostic for " + std::to_string(size) + ": " + e.what());
            ++rejected;
        }
    }
    require(executed != 0 && rejected != 0, "Frame-cap regression did not cover both execution and bounded rejection");
    // Reserve otherwise unused IR frame space to force the final proof cap
    // without exceeding the frontend's valid local-declaration size limit.
    const auto reserved = compile("u16 main(){volatile byte padding[2];padding[0]=1;"
        "u16 a=*((volatile u16*)0x100);u16 b=*((volatile u16*)0x102);u16 c=*((volatile u16*)0x104);"
        "u16 d=*((volatile u16*)0x106);u16* p=(u16*)0x400+(word)(d&3);*p=123;return a+b+c+d;}",
        OptimizationLevel::O2, 0x008000, 65520);
    const auto& function = reserved.ir.functions.at(0);
    require(function.total_local_alloc_size == 65520, "Reserved frame disappeared during SSA promotion");
    const bool exceeds_proof_cap = std::any_of(reserved.object.relocation_table.begin(), reserved.object.relocation_table.end(),
        [](const RelocationEntry& r) { return r.target_symbol_name == "__disco_stack_limit_65532"; });
    require(exceeds_proof_cap, "Frame-cap fixture did not emit a 65528-byte final frame");
    const IRInstruction* pointer = nullptr;
    for (const auto& b : function.blocks) for (const auto& i : b.instructions)
        if (i.opcode == IROpcode::PointerOffset && i.operation == "+") pointer = &i;
    require(pointer != nullptr, "Frame-cap fixture lost its checked pointer offset");
    LinearScanAllocator allocation; allocation.runGlobal(function, {5, 7, 8}, OptimizationLevel::O2);
    const auto* location = allocation.find(pointer->result);
    require(location && !location->has_register && !location->rematerializable,
        "Frame-cap fixture does not pressure the immediately consumed address");
    GSUAddressProof proof; proof.run(function, {}, 65520, true);
    require(proof.provesAccess(pointer->result, pointer->type, 2), "Preliminary pointer proof was not established");
    proof.run(function, {}, 65528, true);
    require(!proof.provesAccess(pointer->result, pointer->type, 2), "Final frame did not exhaust the proof cap");
    DiscoGSU::Machine machine(linked(reserved.object), 0x008000, 0xfffe);
    for (const auto input : {std::make_pair(0x100u, 10u), std::make_pair(0x102u, 20u),
                             std::make_pair(0x104u, 30u), std::make_pair(0x106u, 2u)})
        machine.seed(0x700000 + input.first, static_cast<std::uint8_t>(input.second), false);
    machine.run();
    require(machine.reg(6) == 0 && machine.reg(0) == 62 && machine.word(0x700404) == 123,
        "Lost final-frame proof rematerialized an address across the stored value");
    for (const auto address : {0x700100u, 0x700102u, 0x700104u, 0x700106u})
        require(machine.accesses(address, false) == 1, "Frame-proof fallback repeated a volatile load");
}
}
int main() {
    try {
        standaloneCountdowns();
        inlineHardwareScopes();
        cursorAndTransientValues(); cacheWindowsAndCheckedAddresses(); frameProofBoundary();
        const std::string table = "word main(){word a[17]={0,6,12,19,24,30,36,41,45,49,53,56,59,61,63,64,64};"
            "word i=(word)*((volatile u8*)0x100)&3; word total=0; for(word n=0;n<17;n++) total+=a[n]; return total+(i-i);}";
        // All entries, including signed materialization, are observed dynamically.
        execution(table, 682, false, true);
        auto good = compile(table, OptimizationLevel::O2, 0x008000).ir;
        require(count(good, IROpcode::MemoryInitialize) == 1, "Sequential local initializer was not compacted");
        for (unsigned mutation = 0; mutation < 4; ++mutation) {
            auto invalid = good;
            for (auto& b : invalid.functions[0].blocks) for (auto& i : b.instructions) if (i.opcode == IROpcode::MemoryInitialize) {
                if (mutation == 0) i.initialization_values.resize(257);
                if (mutation == 1) i.immediate = 1;
                if (mutation == 2) i.operands.clear();
                if (mutation == 3) i.memory_volatile = true;
            }
            reject(invalid, mutation == 3 ? "volatile metadata" : "memory initializer");
        }
        std::string uniform = "word main(){word a[32]={";
        for (unsigned n = 0; n < 32; ++n) { if (n) uniform += ','; uniform += "-3"; }
        uniform += "}; word total=0;for(word i=0;i<32;i++)total+=a[i];return total;}";
        execution(uniform, 65440, true, true);
        const std::string span = "word main(){plot{at(4,3); color 2; for(word i=0;i<128;i++){pixel;} flush; return cursor.x;}}";
        execution(span, 132, true);
        const auto explicit_cache = compile("@cache " + span, OptimizationLevel::O2, 0x706007);
        DiscoGSU::Machine manual(linked(explicit_cache.object), 0x706007); manual.run();
        require(manual.reg(0) == 132 && manual.cacheRequests() == 1, "Automatic CACHE interfered with an explicit cache window");
        std::string large = "word main(){for(word i=0;i<8;i++){";
        for (unsigned n = 0; n < 160; ++n) large += "*((volatile byte*)0x200)=(byte)3;";
        large += "}return 7;}";
        const auto large_loop = compile(large, OptimizationLevel::O2, 0x706007);
        require(count(large_loop.ir, IROpcode::HardwareLoop) == 1, "Oversized-cache fallback fixture lost its LOOP");
        DiscoGSU::Machine cold(linked(large_loop.object), 0x706007); cold.run();
        require(cold.reg(0) == 7 && cold.cacheRequests() == 0 && cold.accesses(0x700200, true) == 1280,
            "Automatic CACHE crossed its window or batched observable stores");
        require(count(compile(span, OptimizationLevel::O2, 0x008000).ir, IROpcode::HardwareLoop) == 1,
            "Ordinary constant-trip loop was not converted to LOOP");
        require(compile(span, OptimizationLevel::Size, 0x008000).object.code_section.size() <
            compile(span, OptimizationLevel::O2, 0x008000).object.code_section.size(),
            "Os charged a larger LOOP setup instead of selecting conservative emitted bytes");
        const auto early = compile("word main(){word r=0;for(word i=0;i<16;i++){if(i==3)break;r++;}return r;}", OptimizationLevel::O2, 0x008000);
        require(count(early.ir, IROpcode::HardwareLoop) == 0, "Early-exit loop was unsafely converted");
        execution("word main(){word r=0;for(word i=0;i<0;i++){r++;}return r;}", 0);
        const auto live_out = compile("word main(){word r=0;for(word i=0;i<16;i++){r+=i;}return r;}", OptimizationLevel::O2, 0x008000);
        require(count(live_out.ir, IROpcode::HardwareLoop) == 1, "Loop with repaired live-out SSA value was not converted");
        execution("word main(){word r=0;for(word i=0;i<16;i++){r+=i;}return r;}", 120);
        dynamicLoops(); sizeCountdownsAndFlags(); sizeDivisionCache();
        execution("word main(){word a=9;word b=9;word* p=&a;if(*((volatile u8*)0x100))p=&b;return *p;}", 9);
        const auto volatile_table = compile("word main(){volatile word a[4]={1,2,3,4};return a[0];}", OptimizationLevel::O2, 0x008000);
        require(count(volatile_table.ir, IROpcode::MemoryInitialize) == 0, "Volatile stores were batched");
        for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
            const auto checked = compile("word f(volatile word* p){*((volatile byte*)0x100)=(byte)1;return *p;}", level, 0x008000);
            DiscoGSU::Machine fault(linked(checked.object), 0x008000);
            fault.seed(0x702002, 1, false); fault.seed(0x702003, 3, false); fault.run();
            require(fault.reg(6) == 1 && fault.byte(0x700100) == 1 && fault.accesses(0x700100, true) == 1,
                "Pointer-parameter SSA hoisted a failing check before the volatile witness");
        }
        std::cout << "GSU register, initializer, LOOP and automatic CACHE checks passed\n";
        return 0;
    } catch (const CompilerError& e) { std::cerr << e.getMessage() << '\n'; }
      catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
    return 1;
}
