#include "IRGlobalOptimizer.hpp"
#include "IRDivModFusion.hpp"
#include "IRControlFlow.hpp"
#include "IRSizeOptimizer.hpp"
#include "IRCodeGenerator.hpp"
#include "LinearScanAllocator.hpp"
#include "AssemblyGenerator.hpp"
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Parser.hpp"

#include <iostream>
#include <set>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::size_t count(const IRModule& m, IROpcode op) {
    std::size_t n = 0;
    for (const auto& f : m.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions) n += i.opcode == op;
    return n;
}
void reject(const IRModule& m, const char* diagnostic) {
    try { IRVerifier::verify(m); }
    catch (const CompilerError& e) {
        require(e.getMessage().find(diagnostic) != std::string::npos, "Wrong divmod verifier diagnostic"); return;
    }
    throw std::runtime_error("Invalid divmod IR was accepted");
}

// Independently reconstruct simultaneous live sets, rather than trusting the
// allocator's own interference graph after a PHI-affinity recoloring.
void allocation(const IRFunction& f) {
    IRControlFlow cfg(f); LinearScanAllocator allocator; allocator.runGlobal(f, {5,7,8});
    const auto disjoint = [&](const std::set<std::uint32_t>& live) {
        std::set<std::uint8_t> registers; std::set<int> slots;
        for (const auto value : live) {
            const auto* place = allocator.find(IRValueId{value});
            require(place != nullptr, "Missing live allocation");
            if (place->has_register) require(registers.insert(place->physical_register).second, "Recoloring aliased live registers");
            if (place->spill_slot >= 0) require(slots.insert(place->spill_slot).second, "Recoloring aliased live spills");
        }
    };
    for (const auto& b : f.blocks) {
        auto live = cfg.live_out[b.id.value]; disjoint(live);
        for (auto i = b.instructions.rbegin(); i != b.instructions.rend(); ++i) {
            if (i->result.isValid()) live.erase(i->result.value);
            if (i->opcode != IROpcode::Phi) for (const auto v : i->operands) live.insert(v.value);
            disjoint(live);
        }
    }
    LinearScanAllocator again; again.runGlobal(f, {8,7,5});
    for (const auto& item : allocator.locations()) {
        const auto& other = again.locations().at(item.first);
        require(item.second.has_register == other.has_register && item.second.physical_register == other.physical_register &&
                item.second.spill_slot == other.spill_slot, "PHI allocation is nondeterministic");
    }
}

template<class Check> void check(const std::string& source, Check test, bool hardware = false) {
    Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast);
    if (hardware) { Optimizer optimizer; optimizer.optimize(ast); }
    IRLowerer lowerer; const auto original = lowerer.lower(ast); auto optimized = original;
    IRGlobalOptimizer::run(optimized, analyzer.getAllLocalSymbols());
    test(original, optimized);
    for (const auto& function : optimized.functions) allocation(function);
    const auto dump = dumpIR(optimized); IRGlobalOptimizer::run(optimized, analyzer.getAllLocalSymbols());
    if (dump != dumpIR(optimized)) throw std::runtime_error("Divmod/global pipeline is not idempotent\nFirst:\n" + dump + "Second:\n" + dumpIR(optimized));
    CompilerConfig config; config.optimization = OptimizationLevel::O2;
    IRCodeGenerator backend(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
    const auto object = backend.generate(original);
    IRCodeGenerator repeated(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
    const auto other = repeated.generate(original);
    require(object.code_section == other.code_section && AssemblyGenerator(object).generate() == AssemblyGenerator(other).generate(),
            "Size optimization emitted nondeterministic bytes or metadata");
}

void divmod() {
    check("word f(word a, word b) { word q=a/b; if(a<0 && a%b!=0) q--; return q; }",
        [](const IRModule&, const IRModule& m) {
            require(count(m, IROpcode::DivMod) == 1 && count(m, IROpcode::DivModResult) == 1, "Conditional floor_div did not fuse");
            auto invalid = m;
            for (auto& b : invalid.functions[0].blocks) for (auto& i : b.instructions)
                if (i.opcode == IROpcode::DivModResult) i.operation = "/";
            reject(invalid, "other component");
            invalid = m;
            for (auto& b : invalid.functions[0].blocks) for (auto& i : b.instructions)
                if (i.opcode == IROpcode::DivMod) i.opcode = IROpcode::Binary;
            reject(invalid, "matching pair");
            invalid = m;
            for (auto& b : invalid.functions[0].blocks) for (auto& i : b.instructions)
                if (i.opcode == IROpcode::DivModResult) i.type.is_unsigned = true;
            reject(invalid, "other component");
            invalid = m;
            for (auto& b : invalid.functions[0].blocks) for (auto& i : b.instructions)
                if (i.opcode == IROpcode::DivMod) i.operation = "+";
            reject(invalid, "divmod requires");
            invalid = m;
            for (auto& b : invalid.functions[0].blocks) for (auto& i : b.instructions)
                if (i.opcode == IROpcode::DivMod) i.type.sizeInBytes = 1;
            reject(invalid, "divmod requires");
            invalid = m;
            for (auto& b : invalid.functions[0].blocks) for (auto& i : b.instructions)
                if (i.opcode == IROpcode::DivModResult) i.operands = {i.result};
            reject(invalid, "used before");
        });
    check("u16 f(u16 a,u16 b) { u16 r=a%b; u16 q=a/b; return q+r; }", [](const IRModule&, const IRModule& m) {
        require(count(m, IROpcode::DivMod) == 1 && count(m, IROpcode::DivModResult) == 1, "Unsigned/remainder-first pair did not fuse");
        for (const auto& b : m.functions[0].blocks) for (const auto& i : b.instructions)
            if (i.opcode == IROpcode::DivMod) require(i.operation == "%", "Primary remainder was silently changed to quotient");
    });
    check("void tick(word x); word f(word a,word b) { word q=a/b; tick(q); return q+a%b; }", [](const IRModule&, const IRModule& m) {
        require(count(m, IROpcode::DivMod) == 1 && count(m, IROpcode::DivModResult) == 1, "Pair was lost across a call");
    });
    check("word f(word a,word b,word c) { word q=a/b; if(c) return q+a%b; return q-a%b; }", [](const IRModule&, const IRModule& m) {
        require(count(m, IROpcode::DivMod) == 1 && count(m, IROpcode::DivModResult) == 2, "Dominating pair was not reused in both children");
    });
    check("word f(word a,word b,word c) { if(c) return a/b; return a%b; }", [](const IRModule&, const IRModule& m) {
        require(count(m, IROpcode::DivMod) == 0, "Sibling divisions were incorrectly speculated");
    });
    check("word f(word a,word b) { word q=a/b; a++; return q+a%b; }", [](const IRModule&, const IRModule& m) {
        require(count(m, IROpcode::DivMod) == 0, "Changed SSA operand incorrectly reused an old remainder");
    });
    check("word f(word b) { word q=*(volatile word*)0x100/b; return q+*(volatile word*)0x100%b; }", [](const IRModule&, const IRModule& m) {
        require(count(m, IROpcode::DivMod) == 0, "Volatile reads were equated for fusion");
    });
    check("word f(word a) { return a/8+a%8+a/-8+a%-8; }", [](const IRModule&, const IRModule& m) {
        require(count(m, IROpcode::DivMod) == 0, "Divmod displaced cheaper power-of-two selection");
    });
    check("word f(word a) { return a/7+a%7; }", [](const IRModule&, const IRModule& m) {
        require(count(m, IROpcode::DivMod) == 1, "Distinct equal-constant SSA operands did not fuse");
    });
    check("word f(word a,word b) { word result=0; for(word i=3;i>0;i=i-1) { word q=a/b; result+=q+a%b; } return result; }",
        [](const IRModule&, const IRModule& m) {
            require(count(m, IROpcode::DivMod) == 1 && count(m, IROpcode::HardwareLoopEnd) == 1, "Hardware loop lost its divmod pair/backedge");
        }, true);
}

void phiStress() {
    for (unsigned width = 2; width <= 9; ++width) {
        std::string source = "void tick(word n); word f(word seed) {";
        for (unsigned n = 0; n < width; ++n) source += "word v" + std::to_string(n) + "=seed+" + std::to_string(n) + ";";
        source += "for(word i=0;i<7;i++){word t=v0;";
        for (unsigned n = 0; n + 1 < width; ++n) source += "v" + std::to_string(n) + "=v" + std::to_string(n+1) + ";";
        source += "v" + std::to_string(width-1) + "=t;if(i&1)tick(v0);}";
        source += "word sum=0;";
        for (unsigned n = 0; n < width; ++n) source += "sum+=v" + std::to_string(n) + ";";
        source += "return sum;}";
        check(source, [](const IRModule&, const IRModule& m) {
            require(count(m, IROpcode::Phi) >= 3 && count(m, IROpcode::Call) == 1, "PHI-cycle stress stopped exercising calls/backedges");
        });
    }
    check("word f(word seed) { word a=seed;word b=seed+1;word c=seed+2; for(word i=4;i>0;i=i-1) { word t=a; a=b;b=c;c=t; } return a+b+c; }",
        [](const IRModule&, const IRModule& m) { require(count(m, IROpcode::HardwareLoopEnd) == 1, "PHI stress lost its hardware backedge"); }, true);
}

ObjectFile compile(const std::string& source, OptimizationLevel level) {
    Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; const auto module = lowerer.lower(ast);
    CompilerConfig config; config.optimization = level;
    IRCodeGenerator backend(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
    return backend.generate(module);
}
std::size_t occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0, position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) { ++count; position += needle.size(); }
    return count;
}
void sharedTails() {
    const std::string source = "word f(word a) { if(a==1)return 11; if(a==2)return 22; return 33; }";
    const auto baseline = compile(source, OptimizationLevel::O1), optimized = compile(source, OptimizationLevel::O2);
    require(occurrences(AssemblyGenerator(baseline).generate(), "jmp r11") == 3 &&
            occurrences(AssemblyGenerator(optimized).generate(), "jmp r11") == 1,
            "Multiple return sites did not share a function-local epilogue");
    const std::string guarded = "word f(volatile word* p) { return *p + *p + *p; }";
    const auto before = compile(guarded, OptimizationLevel::O1), after = compile(guarded, OptimizationLevel::O2);
    // Unknown, volatile memory still needs each access check. Sharing terminal
    // alignment islands must reduce their STOPs, not eliminate the accesses.
    const auto a = AssemblyGenerator(before).generate(), b = AssemblyGenerator(after).generate();
    require(occurrences(b, "ibt r6, #$01 ") >= 1 && occurrences(b, "ibt r6, #$01 ") < occurrences(a, "ibt r6, #$01 ") &&
            occurrences(b, "ldw (") >= 3,
            "Alignment islands were not shared locally or recreated beyond byte-branch range");
}

void boundedFusion() {
    IRModule module; IRFunction f; f.name = "main"; f.entry = IRBlockId{0}; f.return_type = Type{BaseType::WORD, "", 2};
    f.blocks.push_back({IRBlockId{0}, "entry", {}});
    const auto append = [&](IROpcode opcode, std::int64_t value, std::vector<IRValueId> inputs, const char* op) {
        IRInstruction i; i.opcode = opcode; i.type = f.return_type; i.result = IRValueId{++f.value_count};
        i.immediate = value; i.operands = std::move(inputs); i.operation = op;
        const auto result = i.result; f.blocks[0].instructions.push_back(std::move(i)); return result;
    };
    const auto divisor = append(IROpcode::Constant, 3, {}, ""); IRValueId last;
    for (unsigned n = 0; n < 300; ++n) {
        const auto dividend = append(IROpcode::Constant, n, {}, "");
        append(IROpcode::Binary, 0, {dividend, divisor}, "/");
        last = append(IROpcode::Binary, 0, {dividend, divisor}, "%");
    }
    IRInstruction ret; ret.opcode = IROpcode::Return; ret.operands = {last}; f.blocks[0].instructions.push_back(ret);
    module.functions.push_back(std::move(f)); IRDivModFusion::run(module);
    require(count(module, IROpcode::DivMod) == 256 && count(module, IROpcode::DivModResult) == 256,
            "Divmod secondary-frame resource cap was not respected");
    IRVerifier::verify(module);
    const auto first = dumpIR(module); IRDivModFusion::run(module);
    require(first == dumpIR(module), "A second fusion invocation exceeded its per-function pair cap");
}

void sizePolicy() {
    const auto tail = [](const std::string& source, unsigned expected) {
        Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
        DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; auto module = lowerer.lower(ast);
        IRGlobalOptimizer::run(module, analyzer.getAllLocalSymbols(), OptimizationLevel::Size);
        require(count(module, IROpcode::Call) == expected, "Os suffix sharing changed nonidentical calls or missed identical tails");
        const auto once = dumpIR(module); IRSizeOptimizer::run(module);
        require(once == dumpIR(module), "Os exact-tail sharing is not idempotent");
    };
    tail("void tick(word a); word f(word s,word a){if(s){tick(a);return 42;}else{tick(a);return 42;}}", 1);
    tail("void tick(word a); word f(word s,word a){if(s){tick(a);return 42;}else{tick(a+1);return 42;}}", 2);
    tail("void tick(word a); word f(word s){if(s){tick(*(volatile word*)0x100);return 42;}else{tick(*(volatile word*)0x100);return 42;}}", 2);
    const std::string repeated = "word f(word a,word b,word c){return a/b+c/b;} word main(){return f(*(volatile word*)0x100,*(volatile word*)0x102,*(volatile word*)0x104);}";
    const auto speed = compile(repeated, OptimizationLevel::O2), size = compile(repeated, OptimizationLevel::Size);
    require(size.code_section.size() < speed.code_section.size(), "Outlined divisions did not shrink repeated helper code");
    std::size_t kernels = 0;
    for (const auto& symbol : size.symbol_table) kernels += symbol.name == std::string(1, '\x01') + "__disco_os_divmod_s";
    require(kernels == 1, "Repeated signed divisions did not share one private kernel");
    const auto single = compile("word f(word a,word b){return a/b;}", OptimizationLevel::Size);
    for (const auto& symbol : single.symbol_table)
        require(symbol.name.find("__disco_os_divmod_") == std::string::npos, "One division acquired a larger outlined helper");
    const auto cached = compile("@cache word f(word a){return a+1;}", OptimizationLevel::Size);
    require(cached.code_section.front() == 2, "Os removed an explicit CACHE instruction");
    for (const auto& symbol : cached.symbol_table)
        require(symbol.name.find("__disco_cache_align16") == std::string::npos, "Os added CACHE padding metadata");
    require(AssemblyGenerator(size).generate() == AssemblyGenerator(compile(repeated, OptimizationLevel::Size)).generate(),
        "Os machine candidate selection is nondeterministic");
}
}

int main() {
    try {
        divmod(); phiStress(); sharedTails(); boundedFusion(); sizePolicy();
        std::cout << "O2/Os divmod, exact-tail sharing, PHI interference and deterministic codegen checks passed\n";
    } catch (const CompilerError& e) { std::cerr << e.getMessage() << '\n'; return 1; }
      catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
