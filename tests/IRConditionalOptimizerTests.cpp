#include "IRConditionalOptimizer.hpp"
#include "IRGlobalOptimizer.hpp"
#include "IRControlFlow.hpp"
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
