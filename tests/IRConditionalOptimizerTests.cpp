#include "IRConditionalOptimizer.hpp"
#include "IRGlobalOptimizer.hpp"
#include "IRControlFlow.hpp"
#include "IRCodeGenerator.hpp"
#include "GSUInstructionModel.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Optimizer.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Type word() { return Type{BaseType::WORD, "", 2, false}; }
Type boolean() { return Type{BaseType::BOOL, "", 1, false}; }
std::size_t count(const IRModule& m, IROpcode opcode) {
    std::size_t result = 0;
    for (const auto& f : m.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        result += i.opcode == opcode;
    return result;
}
struct Builder {
    IRModule module;
    explicit Builder(std::size_t blocks) {
        IRFunction f; f.name = "main"; f.return_type = word(); f.entry = IRBlockId{0};
        for (std::size_t n = 0; n < blocks; ++n)
            f.blocks.push_back({IRBlockId{static_cast<std::uint32_t>(n)}, "b" + std::to_string(n), {}});
        module.functions.push_back(std::move(f));
    }
    IRValueId value(std::size_t block, IROpcode opcode, Type type = word(),
                    std::vector<IRValueId> operands = {}, std::string operation = {}, std::int64_t immediate = 0) {
        auto& f = module.functions[0];
        IRInstruction i; i.opcode = opcode; i.type = std::move(type); i.result = IRValueId{++f.value_count};
        i.operands = std::move(operands); i.operation = std::move(operation); i.immediate = immediate;
        if (opcode == IROpcode::Call) i.symbol = "input";
        f.blocks.at(block).instructions.push_back(std::move(i)); return IRValueId{f.value_count};
    }
    IRValueId literal(std::size_t block, std::int64_t value) { return this->value(block, IROpcode::Constant, word(), {}, {}, value); }
    IRValueId phi(std::size_t block, std::vector<IRValueId> values, std::vector<IRBlockId> predecessors) {
        const auto id = value(block, IROpcode::Phi, word(), std::move(values));
        module.functions[0].blocks[block].instructions.back().targets = std::move(predecessors); return id;
    }
    void terminal(std::size_t block, IROpcode opcode, std::vector<IRValueId> operands, std::vector<IRBlockId> targets = {}) {
        IRInstruction i; i.opcode = opcode; i.operands = std::move(operands); i.targets = std::move(targets);
        module.functions[0].blocks.at(block).instructions.push_back(std::move(i));
    }
    void branch(std::size_t block, std::uint32_t target) { terminal(block, IROpcode::Branch, {}, {IRBlockId{target}}); }
    void ret(std::size_t block, IRValueId value) { terminal(block, IROpcode::Return, {value}); }
};
void optimize(IRModule& m) {
    IRConditionalOptimizer::run(m);
    const auto first = dumpIR(m); IRConditionalOptimizer::run(m);
    require(first == dumpIR(m), "SCCP/CFG cleanup is not idempotent");
}
void returnedConstant(const IRModule& m, std::int64_t expected) {
    const auto& f = m.functions[0];
    for (const auto& b : f.blocks) if (b.instructions.back().opcode == IROpcode::Return) {
        const auto result = b.instructions.back().operands[0];
        for (const auto& definition : f.blocks) for (const auto& i : definition.instructions)
            if (i.result.value == result.value && i.opcode == IROpcode::Constant && i.immediate == expected) return;
    }
    throw std::runtime_error("Expected constant return was not propagated");
}
template<class Test> void source(const std::string& text, Test test, bool hardware = false) {
    Lexer lexer(text); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast);
    if (hardware) { Optimizer optimizer; optimizer.optimize(ast); }
    IRLowerer lowerer; auto m = lowerer.lower(ast);
    IRGlobalOptimizer::run(m, analyzer.getAllLocalSymbols()); test(m);
    const auto first = dumpIR(m); IRGlobalOptimizer::run(m, analyzer.getAllLocalSymbols());
    if (first != dumpIR(m)) throw std::runtime_error("Global O2 pipeline is not idempotent after SCCP: " + text + "\nFirst:\n" + first + "\nSecond:\n" + dumpIR(m));
}
template<class Test> void conditionalSource(const std::string& text, Test test, TargetKind target = TargetKind::GSU) {
    Lexer lexer(text); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data, target); analyzer.analyze(ast);
    std::vector<int> frames;
    for (const auto& statement : ast)
        if (const auto* function = dynamic_cast<const FunctionDeclStmt*>(statement.get()))
            if (!function->is_prototype) frames.push_back(function->total_local_alloc_size);
    IRLowerer lowerer(target); auto module = lowerer.lower(ast); IRVerifier::verify(module);
    test(module, frames);
}
std::size_t logicalSlots(const IRModule& module) {
    std::size_t result = 0;
    for (const auto& function : module.functions) for (const auto& block : function.blocks)
        for (const auto& instruction : block.instructions)
            result += instruction.opcode == IROpcode::Address && instruction.operation == "temporary";
    return result;
}
void directConditions() {
    for (const auto target : {TargetKind::GSU, TargetKind::SPC700}) {
        conditionalSource("word f(word a,word b,word c){if(!((a<b && !c) || (b==3 && a!=4)))return 1;else return 2;}",
            [](const IRModule& module, const std::vector<int>& frames) {
                require(logicalSlots(module) == 0 && count(module, IROpcode::Phi) == 0,
                        "Condition-only logical expression allocated a boolean temporary");
                require(count(module, IROpcode::CondBranch) == 4 && count(module, IROpcode::Unary) == 0,
                        "Mixed logical conditions did not branch directly or invert negation targets");
                require(module.functions[0].total_local_alloc_size == frames[0], "Conditional lowering grew the source frame");
            }, target);
        conditionalSource("word f(word a){if(!a)return 1;else return 2;}",
            [](const IRModule& module, const std::vector<int>&) {
                const auto& function = module.functions[0]; const auto& branch = function.blocks[0].instructions.back();
                require(function.blocks.at(branch.targets[0].value).label == "if.else" &&
                        function.blocks.at(branch.targets[1].value).label == "if.then", "Negation did not swap branch targets");
            }, target);
        conditionalSource("word f(word a){if((byte)a)return 1;return 2;}",
            [](const IRModule& module, const std::vector<int>&) {
                require(count(module, IROpcode::Cast) == 1, "Condition lowering discarded a non-truth-preserving narrowing cast");
            }, target);
        conditionalSource("bool f(word a,word b){bool value=a||b;return value && !a;}",
            [](const IRModule& module, const std::vector<int>& frames) {
                require(logicalSlots(module) == 2 && module.functions[0].total_local_alloc_size > frames[0],
                        "Logical values lost their canonical boolean materialization");
                require(count(module, IROpcode::Unary) == 1, "Value-producing negation was treated as control flow");
            }, target);
    }
    conditionalSource("word f(word a,word b){plot{at(3,4);if(a && !b)pixel;"
        "while(a && b){if(a || b)break;}for(word i=0;i<2 && a;i++){if(b || !a)continue;pixel;}flush;}return 0;}",
        [](const IRModule& module, const std::vector<int>& frames) {
            require(logicalSlots(module) == 0 && module.functions[0].total_local_alloc_size == frames[0],
                    "Plot/loop conditions allocated boolean slots");
            for (const auto& block : module.functions[0].blocks) for (const auto& instruction : block.instructions)
                if (instruction.opcode == IROpcode::CondBranch)
                    require(instruction.in_plot_context, "Short-circuit branch lost its plotting context");
        });
    bool rejected = false;
    try { conditionalSource("word f(){if(false && 10/0)return 1;return 2;}", [](const IRModule&, const std::vector<int>&) {}); }
    catch (const CompilerError& error) { rejected = error.getMessage().find("Division by zero") != std::string::npos; }
    require(rejected, "Short-circuit lowering suppressed an existing source diagnostic");
}
std::vector<std::uint8_t> conditionalCode(const std::string& text, OptimizationLevel level) {
    Lexer lexer(text); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer;
    const auto module = lowerer.lower(ast); IRVerifier::verify(module);
    CompilerConfig config; config.optimization = level; config.code_start_address = 0x008000;
    IRCodeGenerator backend(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
    const auto object = backend.generate(module); auto bytes = object.code_section;
    // These fixtures have only same-object code and stack-limit relocations.
    // Reject any new relocation category instead of leaving unlinked operands.
    for (const auto& relocation : object.relocation_table) {
        bool found = false; std::uint32_t value = 0; const std::string prefix = "__disco_stack_limit_";
        if (relocation.target_symbol_name.compare(0, prefix.size(), prefix) == 0) {
            require(relocation.type == RelocationType::ADDR16_RAM, "Unexpected stack-limit relocation");
            value = static_cast<std::uint32_t>(std::stoul(relocation.target_symbol_name.substr(prefix.size()))); found = true;
        } else for (const auto& symbol : object.symbol_table) if (symbol.name == relocation.target_symbol_name) {
            require(symbol.section == SymbolSection::CODE &&
                (relocation.type == RelocationType::ADDR16_IWT || relocation.type == RelocationType::ADDR16_JAL),
                "Unexpected conditional fixture relocation");
            value = object.config.code_start_address + symbol.offset; found = true;
        }
        require(found && value <= 65535, "Missing/out-of-range conditional fixture relocation");
        bytes.at(relocation.patch_offset + 1) = static_cast<std::uint8_t>(value);
        bytes.at(relocation.patch_offset + 2) = static_cast<std::uint8_t>(value >> 8);
    }
    return bytes;
}
void seedWord(DiscoGSU::Machine& machine, std::uint32_t address, std::uint16_t value) {
    machine.seed(address, static_cast<std::uint8_t>(value), false);
    machine.seed(address + 1, static_cast<std::uint8_t>(value >> 8), false);
}
void conditionalExecution() {
    // Entry STOP retains the saved R9/R11 header, unlike a returning callee.
    constexpr std::uint16_t stopped_entry_stack = 0x1ffc;
    const std::string effects = "bool probe(word tag,word value);word main(){word a=*(volatile word*)0x100;"
        "word b=*(volatile word*)0x102;word c=*(volatile word*)0x104;*(volatile word*)0x110=0;"
        "if(!((a && probe(1,b)) || (!b && probe(2,c))) && probe(3,a || c))return 31;return 47;}"
        "bool probe(word tag,word value){*(volatile word*)0x110=*(volatile word*)0x110*4+tag;return (bool)value;}";
    const std::string loops = "word main(){word n=*(volatile word*)0x100;word total=0;"
        "for(word i=0;i<n && i<5;i++){if((i==1 || i==3) && n)continue;total+=i;"
        "for(word k=0;k<2 && n;k++){if(k==0 || !n)continue;total++;}}"
        "word j=0;while(j<4 && !(j>=n)){j++;if(!(j!=2) && n)continue;total+=10*j;}return total;}";
    const std::string faults = "word main(){word gate=*(volatile word*)0x100;word divisor=*(volatile word*)0x102;"
        "word* p=(word*)*(volatile u16*)0x104;*(volatile byte*)0x110=(byte)11;"
        "if(gate && (10/divisor>0 || *p!=0))*(volatile byte*)0x111=(byte)22;"
        "*(volatile byte*)0x112=(byte)33;return 42;}";
    const std::string values = "bool both(word a,word b);bool identity(bool a);word main(){word a=*(volatile word*)0x100;"
        "word b=*(volatile word*)0x102;bool value=a || b;value=identity(a && !b);"
        "return (word)value+2*(word)both(a,b)+4*(word)identity(a || b);}"
        "bool both(word a,word b){return a && b;}bool identity(bool a){return a;}";
    for (const auto level : {OptimizationLevel::Baseline, OptimizationLevel::O1, OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto effect_code = conditionalCode(effects, level), loop_code = conditionalCode(loops, level);
        const auto fault_code = conditionalCode(faults, level), value_code = conditionalCode(values, level);
        const auto narrow_code = conditionalCode("word main(){word a=*(volatile word*)0x100;if((byte)a)return 1;return 2;}", level);
        for (const auto a : {0u, 1u, 256u, 65535u}) for (const auto b : {0u, 1u, 256u, 65535u}) {
            DiscoGSU::Machine value_machine(value_code, 0x008000); seedWord(value_machine, 0x700100, static_cast<std::uint16_t>(a));
            seedWord(value_machine, 0x700102, static_cast<std::uint16_t>(b)); value_machine.run();
            require(value_machine.reg(0) == static_cast<unsigned>((a && !b) + 2*(a && b) + 4*(a || b)) && value_machine.reg(6) == 0,
                    "Assignment/call/return logical values were not canonical booleans");
            for (const auto c : {0u, 1u, 256u, 65535u}) {
                unsigned trace = 0, calls = 0;
                const auto probe = [&](unsigned tag, bool value) { trace = trace*4+tag; ++calls; return value; };
                const bool selected = !((a && probe(1,b != 0)) || (!b && probe(2,c != 0))) && probe(3,a || c);
                DiscoGSU::Machine machine(effect_code, 0x008000); seedWord(machine, 0x700100, static_cast<std::uint16_t>(a));
                seedWord(machine, 0x700102, static_cast<std::uint16_t>(b)); seedWord(machine, 0x700104, static_cast<std::uint16_t>(c)); machine.run();
                require(machine.reg(0) == (selected ? 31 : 47) && machine.word(0x700110) == trace,
                        "Nested logical condition changed evaluation order or its selected branch");
                require(machine.accesses(0x700110, false) == calls && machine.accesses(0x700110, true) == calls+1,
                        "Short-circuit condition duplicated or speculated volatile call effects");
                if (machine.accesses(0x700100, false) != 1 || machine.accesses(0x700102, false) != 1 ||
                    machine.accesses(0x700104, false) != 1 || machine.reg(6) != 0 || machine.reg(10) != stopped_entry_stack)
                    throw std::runtime_error("Logical condition damaged input observations, checked state or stack ownership: level=" +
                        std::to_string(static_cast<unsigned>(level)) + " inputs=" + std::to_string(a) + "," + std::to_string(b) + "," +
                        std::to_string(c) + " reads=" + std::to_string(machine.accesses(0x700100, false)) + "," +
                        std::to_string(machine.accesses(0x700102, false)) + "," + std::to_string(machine.accesses(0x700104, false)) +
                        " r6=" + std::to_string(machine.reg(6)) + " sp=" + std::to_string(machine.reg(10)));
            }
        }
        for (const auto n : {0u, 1u, 2u, 4u, 7u}) {
            unsigned expected = 0;
            for (unsigned i=0; i<n && i<5; ++i) { if (i==1 || i==3) continue; expected += i+1; }
            for (unsigned j=1; j<=4 && j<=n; ++j) if (j!=2) expected += 10*j;
            DiscoGSU::Machine machine(loop_code, 0x008000); seedWord(machine, 0x700100, static_cast<std::uint16_t>(n)); machine.run();
            require(machine.reg(0) == expected && machine.reg(6) == 0 && machine.reg(10) == stopped_entry_stack,
                    "Direct for/while conditions changed nested continue targets or loop results");
        }
        for (unsigned scenario=0; scenario<4; ++scenario) {
            DiscoGSU::Machine machine(fault_code, 0x008000); seedWord(machine, 0x700100, scenario == 0 ? 0 : 1);
            seedWord(machine, 0x700102, scenario < 2 ? 0 : scenario == 2 ? 1 : 20); seedWord(machine, 0x700104, 0); machine.run();
            const bool fault = scenario == 1 || scenario == 3;
            require(machine.byte(0x700110) == 11 && machine.accesses(0x700110, true) == 1,
                    "Condition fault moved before its volatile witness");
            require(machine.byte(0x700112) == (fault ? 0 : 33) && (machine.reg(6) != 0) == fault,
                    "Short-circuit condition speculated a fault or removed a required fail-stop");
            require(machine.byte(0x700111) == (scenario == 2 ? 22 : 0), "Faulting/skipped condition executed its body");
            if (!fault) require(machine.reg(0) == 42 && machine.reg(10) == stopped_entry_stack, "Skipped fault damaged normal return state");
        }
        for (const auto input : {0u, 1u, 256u, 65535u}) {
            DiscoGSU::Machine machine(narrow_code, 0x008000); seedWord(machine, 0x700100, static_cast<std::uint16_t>(input)); machine.run();
            require(machine.reg(0) == ((input & 255u) ? 1 : 2), "Condition bypassed byte-cast truncation");
        }
    }
}
void executableEdges() {
    Builder b(4);
    const auto condition = b.literal(0, 0); b.terminal(0, IROpcode::CondBranch, {condition}, {{1}, {2}});
    const auto wrong = b.literal(1, 999); b.branch(1, 3);
    const auto right = b.literal(2, 42); b.branch(2, 3);
    const auto result = b.phi(3, {wrong, right}, {{1}, {2}}); b.ret(3, result);
    optimize(b.module);
    require(b.module.functions[0].blocks.size() == 1, "Dead predecessor prevented ordinary chain contraction");
    require(count(b.module, IROpcode::Phi) == 0 && count(b.module, IROpcode::CondBranch) == 0, "Executable-edge PHI was not simplified");
    returnedConstant(b.module, 42);
}
void mixedPhis() {
    Builder b(6);
    const auto condition = b.value(0, IROpcode::Call); b.terminal(0, IROpcode::CondBranch, {condition}, {{1}, {2}});
    const auto a = b.literal(1, 7), x = b.value(1, IROpcode::Call); b.branch(1, 3);
    const auto c = b.literal(2, 7), y = b.value(2, IROpcode::Call); b.branch(2, 3);
    const auto fixed = b.phi(3, {a, c}, {{1}, {2}}), varying = b.phi(3, {x, y}, {{1}, {2}});
    const auto seven = b.literal(3, 7), equal = b.value(3, IROpcode::Binary, boolean(), {fixed, seven}, "==");
    b.terminal(3, IROpcode::CondBranch, {equal}, {{4}, {5}});
    const auto sum = b.value(4, IROpcode::Binary, word(), {fixed, varying}, "+"); b.ret(4, sum);
    const auto bad = b.literal(5, -1); b.ret(5, bad);
    optimize(b.module);
    require(count(b.module, IROpcode::Phi) == 1 && count(b.module, IROpcode::CondBranch) == 1, "Constant join did not propagate, or unknown PHI was folded");
    require(count(b.module, IROpcode::Call) == 3, "SCCP removed executable side-effecting calls");
    require(b.module.functions[0].blocks.size() == 4, "Join/return chain was not contracted");
}
void lateBackedge() {
    Builder b(4);
    const auto zero = b.literal(0, 0), one = b.literal(0, 1); b.branch(0, 1);
    // The next value is defined later, on the backedge. Initially the PHI is
    // constant zero; the second iteration must raise it to Variable.
    const auto index = b.phi(1, {zero, IRValueId{5}}, {{0}, {2}});
    const auto equal = b.value(1, IROpcode::Binary, boolean(), {index, zero}, "==");
    b.terminal(1, IROpcode::CondBranch, {equal}, {{2}, {3}});
    const auto next = b.value(2, IROpcode::Binary, word(), {index, one}, "+");
    require(next.value == 5, "Backedge test ID drifted"); b.branch(2, 1); b.ret(3, index);
    optimize(b.module);
    require(count(b.module, IROpcode::Phi) == 1 && count(b.module, IROpcode::CondBranch) == 1 && count(b.module, IROpcode::Binary) == 2,
            "First-iteration constant was incorrectly frozen across an executable backedge");
    require(b.module.functions[0].blocks.size() == 4, "Loop exit disappeared after the backedge changed its condition");
}
void dispatch(std::int64_t selector, bool default_target, std::size_t expected_blocks, std::int64_t expected) {
    Builder b(default_target ? 4 : 3);
    const auto input = b.literal(0, selector);
    b.terminal(0, IROpcode::Switch, {input}, default_target ? std::vector<IRBlockId>{{1}, {2}, {3}} : std::vector<IRBlockId>{{1}, {2}});
    auto& s = b.module.functions[0].blocks[0].instructions.back(); s.case_values = {2, 5}; s.has_default_target = default_target;
    for (std::size_t n = 1; n < b.module.functions[0].blocks.size(); ++n) { const auto v = b.literal(n, static_cast<std::int64_t>(n * 10)); b.ret(n, v); }
    optimize(b.module);
    require(b.module.functions[0].blocks.size() == expected_blocks, "Switch/default/partial dispatch changed reachable paths incorrectly");
    if (expected_blocks == 1) { returnedConstant(b.module, expected); require(count(b.module, IROpcode::Switch) == 0, "Constant switch was not simplified"); }
    else require(count(b.module, IROpcode::Switch) == 1, "Missing-default partial dispatch acquired invented semantics");
}
}
int main() {
    try {
        directConditions(); conditionalExecution();
        executableEdges(); mixedPhis(); lateBackedge(); dispatch(5, true, 1, 20); dispatch(99, true, 1, 30); dispatch(99, false, 3, 0);
        source("word f(word x) { word a; if(x) a=255; else a=255; return (word)(byte)a; }", [](const IRModule& m) { returnedConstant(m, -1); });
        source("word f(word x) { word a; if(x) a=32767; else a=32767; return a+1; }", [](const IRModule& m) { returnedConstant(m, -32768); });
        source("word f(word x) { word a; if(x) a=-32768; else a=-32768; return a / -1; }", [](const IRModule& m) { returnedConstant(m, -32768); });
        source("word f(word x) { word a; if(x) a=-9; else a=-9; return (a >> 1) + a % 4; }", [](const IRModule& m) { returnedConstant(m, -6); });
        source("word f(word x) { bool a; if(x) a=true; else a=true; if(!a) return 5; return 42; }", [](const IRModule& m) { returnedConstant(m, 42); require(count(m, IROpcode::CondBranch)==1, "Bool join did not simplify its branch"); });
        source("enum Mode { A=2, B=3 }; word f(word x) { enum Mode m; if(x) m=A; else m=A; word result=0; switch(m) { case A: result=42; break; default: result=8; break; } return result; }", [](const IRModule& m) { returnedConstant(m, 42); require(count(m,IROpcode::Switch)==0, "Enum constant switch did not simplify"); });
        source("word f() { switch((byte)0) { case 256: return 11; default: return 22; } }", [](const IRModule& m) { returnedConstant(m, 22); });
        source("word f(word x) { byte selector; if(x) selector=0; else selector=0; switch(selector) { case 256: return 11; default: return 22; } }", [](const IRModule& m) { returnedConstant(m, 22); });
        source("word f(word x) { u16 selector; if(x) selector=(u16)65535; else selector=(u16)65535; switch(selector) { case -1: return 11; case 65535: return 22; default: return 33; } }", [](const IRModule& m) { returnedConstant(m, 22); });
        source("word f(word x) { byte selector; if(x) selector=-1; else selector=-1; switch(selector) { case 255: return 11; case -1: return 22; default: return 33; } }", [](const IRModule& m) { returnedConstant(m, 22); });
        source("word f(word x) { word d; if(x) d=0; else d=0; return 42/d; }", [](const IRModule& m) { require(count(m,IROpcode::Binary)==1, "SCCP erased division-by-zero fail-stop"); });
        source("word f(word x) { word d; if(x) d=16; else d=16; return 1 << d; }", [](const IRModule& m) { require(count(m,IROpcode::Binary)==1, "SCCP erased invalid-shift fail-stop"); });
        source("word f() { word x=*(volatile word*)0x100; if(x) return 1; return 2; }", [](const IRModule& m) { require(count(m,IROpcode::CondBranch)==1 && count(m,IROpcode::LoadIndirect)==1, "Volatile input was speculated"); });
        source("word f() { word* p=(word*)0x100; word a=*p; if(a) *p=7; return *p; }", [](const IRModule& m) {
            std::size_t scalar_reads=0;
            for(const auto& b : m.functions[0].blocks) for(const auto& i : b.instructions)
                scalar_reads += i.opcode==IROpcode::LoadIndirect && i.type.pointer_level==0;
            require(count(m,IROpcode::CondBranch)==1 && scalar_reads==2, "Mutable memory became an SSA constant");
        });
        source("word f(word x) { if(1) return x+1; else return 9; }", [](const IRModule& m) { require(m.functions[0].blocks.size()==1, "Straight-line return chain remains split"); });
        source("word f() { word x=0; for(word i=3;i>0;i=i-1) { if(1) x+=2; else x+=9; } return x; }", [](const IRModule& m) {
            require(count(m,IROpcode::HardwareLoop)==1 && count(m,IROpcode::HardwareLoopEnd)==1 && count(m,IROpcode::HardwareLoopLeave)==1,
                    "Hardware-loop scope was fragmented");
            require(count(m,IROpcode::CondBranch)==0 && count(m,IROpcode::Phi)>=1, "Hardware-loop body did not simplify, or its recurrence was folded");
        }, true);
        source("word f() { plot { if(1) { color 5; pixel; } else { color 7; pixel; } flush; } return 42; }", [](const IRModule& m) {
            require(count(m,IROpcode::Plot)==1 && count(m,IROpcode::Rpix)==1 && count(m,IROpcode::SetColor)==1,
                    "Executable graphics effects or discarded RPIX were removed");
        });
        Builder invalid(1); const auto result=invalid.literal(0,42); invalid.ret(0,result);
        invalid.module.functions[0].blocks[0].instructions.back().operands={IRValueId{9}};
        bool rejected=false;
        try { IRConditionalOptimizer::run(invalid.module); } catch(const CompilerError&) { rejected=true; }
        require(rejected, "SCCP accepted malformed input IR");
        Builder identical(2); const auto observed=identical.value(0,IROpcode::Call);
        identical.terminal(0,IROpcode::CondBranch,{observed},{{1},{1}});
        const auto answer=identical.literal(1,42); identical.ret(1,answer); optimize(identical.module);
        require(identical.module.functions[0].blocks.size()==1 && count(identical.module,IROpcode::Call)==1,
                "Identical destinations retained their branch or discarded its observable input");
        Builder large(8193);
        bool bounded=false;
        try { IRConditionalOptimizer::run(large.module); } catch(const CompilerError& e) { bounded=e.getMessage().find("resource limit")!=std::string::npos; }
        require(bounded,"SCCP graph allocation was not bounded before verification");
        const std::string hardware_source="word f() { word x=0; for(word n=3;n>0;n=n-1) { x+=2; } return x; }";
        Lexer lexer(hardware_source);
        const auto tokens=lexer.scanTokens(); Parser parser(tokens); auto ast=parser.parseProgram();
        DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); Optimizer optimizer; optimizer.optimize(ast);
        IRLowerer lowerer; auto hidden=lowerer.lower(ast); bool explicit_required=false;
        require(count(hidden,IROpcode::HardwareLoop)==1,"Hidden-loop fixture did not contain its required backedge");
        std::string hidden_diagnostic;
        try { IRConditionalOptimizer::run(hidden); } catch(const CompilerError& e) { hidden_diagnostic=e.getMessage(); explicit_required=hidden_diagnostic.find("exposed hardware-loop")!=std::string::npos; }
        if (!explicit_required) throw std::runtime_error("SCCP hidden-loop precondition: " + hidden_diagnostic + "\n" + dumpIR(hidden));
        std::cout << "SCCP executable edges, typed constants, PHIs, CFG, loops and effect checks passed\n";
    } catch(const CompilerError& e) { std::cerr << e.getMessage() << '\n'; return 1; }
      catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
