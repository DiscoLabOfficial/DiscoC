#include "IRGlobalOptimizer.hpp"
#include "IRDivModFusion.hpp"
#include "IRControlFlow.hpp"
#include "IRSizeOptimizer.hpp"
#include "IRCodeGenerator.hpp"
#include "LinearScanAllocator.hpp"
#include "AssemblyGenerator.hpp"
#include "GSUInstructionModel.hpp"
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Parser.hpp"

#include <iostream>
#include <set>
#include <stdexcept>

// Private, invocation-only evidence stays out of ObjectFile and public APIs.
struct IRCodeGeneratorTestAccess {
    static void verify() {
        const auto require = [](bool ok, const char* message) { if (!ok) throw std::runtime_error(message); };
        const std::string source = "word main(){return 7;}";
        Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens);
        auto ast = parser.parseProgram(); DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast);
        IRLowerer lowerer; auto module = lowerer.lower(ast); module.functions[0].link_name = "physical_main";
        CompilerConfig config; config.optimization = OptimizationLevel::Size;
        IRCodeGenerator backend(analyzer.getAllLocalSymbols(),analyzer.getFunctionSymbols(),data,config);
        auto emitted = backend.generateOptimized(module,OptimizationLevel::O2,IRCompactionPolicy::Disabled);
        require(emitted.function_metrics.size() == 1 && emitted.function_metrics.count("main") == 1 &&
            !emitted.function_metrics.count("physical_main") && emitted.function_metrics.at("main").speed_score != 0,
            "Scheduled function metrics lost source identity or became a missing zero score");
        const auto owned = emitted.function_metrics;
        auto reference = emitted; reference.speed_score = 100; reference.function_metrics.at("main").speed_score = 100;
        for (const unsigned score : {99u,100u,101u}) {
            auto proposed = reference; proposed.speed_score = score; proposed.function_metrics.at("main").speed_score = score;
            require(backend.admissibleExtension(proposed,reference) == (score <= 100),
                "Extension speed admissibility did not distinguish lower/equal/higher scores");
        }
        auto missing = reference; missing.function_metrics.clear();
        require(!backend.admissibleExtension(missing,reference), "Missing function evidence admitted an extension");
        auto divergent = reference; divergent.speed_score = 50; divergent.function_metrics.at("main").speed_score = 101;
        require(!backend.admissibleExtension(divergent,reference), "A faster total hid a slower function");
        auto balanced_reference = reference; balanced_reference.speed_score = 200;
        balanced_reference.function_metrics.emplace("other",IRCodeGenerator::FunctionMetric{100,true});
        auto balanced = balanced_reference; balanced.speed_score = 151;
        balanced.function_metrics.at("main").speed_score = 101; balanced.function_metrics.at("other").speed_score = 50;
        require(!backend.admissibleExtension(balanced,balanced_reference), "An improvement in another function hid a local loss");
        IRCodeGenerator::SpillIssue issue; issue.function = "main";
        for (const bool strict : {false,true}) {
            config.require_no_spills = strict;
            auto spilling = reference; spilling.issues.push_back(issue); spilling.function_metrics.at("main").no_spills = false;
            auto eligible = reference; eligible.issues.clear(); eligible.function_metrics.at("main").no_spills = true;
            eligible.speed_score = 200; eligible.function_metrics.at("main").speed_score = 200;
            require(backend.admissibleExtension(eligible,spilling) == strict,
                "Strict spill-free improvement did not preserve eligibility precedence");
            spilling.speed_score = 1; spilling.function_metrics.at("main").speed_score = 1;
            require(backend.admissibleExtension(spilling,eligible) == !strict,
                "Strict extension replaced an eligible function with a faster spilling function");
        }
        auto global_spilling = balanced_reference; global_spilling.issues.push_back(issue);
        global_spilling.function_metrics.at("main").no_spills = false;
        auto globally_eligible = balanced_reference; globally_eligible.issues.clear();
        globally_eligible.speed_score = 700; globally_eligible.function_metrics.at("main").speed_score = 200;
        globally_eligible.function_metrics.at("other").speed_score = 500;
        require(backend.admissibleExtension(globally_eligible,global_spilling),
            "Global strict spill-free improvement was blocked by a slower already-eligible function");
        require(!backend.admissibleExtension(global_spilling,globally_eligible),
            "Global strict eligibility regressed before local proxy comparison");
        auto missing_eligible = globally_eligible; missing_eligible.function_metrics.erase("other");
        require(!backend.admissibleExtension(missing_eligible,global_spilling),
            "Strict eligibility improvement bypassed complete function evidence");
        config.require_no_spills = false;
        auto optimized = module; IRGlobalOptimizer::run(optimized,analyzer.getAllLocalSymbols(),OptimizationLevel::O2,IRCompactionPolicy::Disabled);
        backend.m_function_candidates.clear(); backend.m_collect_function_candidates = true;
        IRCodeGenerator::FunctionCandidate greedy; greedy.bytes = 100000; greedy.speed_score = 1000;
        greedy.function.name = "unchanged"; backend.m_function_candidates["main"] = greedy;
        // The greedy map disagrees with the complete incumbent. Reject BEFORE
        // collection, even though the function is smaller/faster than that map.
        const IRCodeGenerator::CandidateReference collection_reference(reference);
        backend.considerFunctionCandidates(optimized,divergent,&collection_reference);
        require(backend.m_function_candidates.at("main").function.name == "unchanged",
            "Rejected extension overwrote a function snapshot or used the greedy reference");
        backend.considerFunctionCandidates(optimized,reference,&collection_reference);
        require(backend.m_function_candidates.at("main").function.name == "main",
            "Equal-score extension was not admitted before byte-first collection");
        for (unsigned mutation = 0; mutation < 3; ++mutation) {
            auto invalid = emitted;
            for (auto i = invalid.object.symbol_table.begin(); i != invalid.object.symbol_table.end();) {
                if ((mutation == 0 && i->name == "physical_main") ||
                    (mutation == 1 && i->name == std::string(1,'\x01') + "main#0")) i = invalid.object.symbol_table.erase(i);
                else { if (mutation == 2 && i->name == "physical_main") i->offset = static_cast<std::uint32_t>(invalid.object.code_section.size() + 1); ++i; }
            }
            backend.recordCandidateMetrics(optimized,invalid);
            require(invalid.function_metrics.empty() && !backend.admissibleExtension(invalid,reference),
                "Invalid symbol/bounds evidence was silently scored as zero");
        }
        auto moved = std::move(emitted);
        backend.m_collect_function_candidates = false;
        backend.generateOptimized(module,OptimizationLevel::Size,IRCompactionPolicy::PointerPromotionOnly);
        require(moved.function_metrics.at("main").speed_score == owned.at("main").speed_score &&
            moved.function_metrics.at("main").no_spills == owned.at("main").no_spills,
            "A later emission changed moved candidate-owned metrics");
        // Exercise cleanup after an exception inside mixed override setup.
        backend.m_function_candidates.clear(); greedy.function = module.functions[0];
        backend.m_function_candidates["main"] = greedy;
        auto incomplete = module; auto absent = module.functions[0]; absent.name = "absent"; absent.link_name.clear();
        incomplete.functions.push_back(absent); bool failed = false;
        try { backend.generateMixedCandidate(incomplete); }
        catch (const CompilerError& error) {
            require(error.getMessage() == "IR codegen: missing bounded function candidate." &&
                error.getLine() == 1 && error.getCol() == 1,
                "Missing mixed candidate produced the wrong diagnostic or source location");
            failed = true;
        }
        require(failed && backend.m_function_policies.empty() && backend.m_function_division_policies.empty() &&
            backend.m_function_pool_policies.empty() && !backend.m_collect_function_candidates && backend.m_allow_auto_cache &&
            backend.m_current_function == nullptr && backend.m_values.empty(),
            "Mixed exception leaked phase overrides or collection/cache flags");
        auto invalid_module = module; invalid_module.functions[0].blocks.clear(); failed = false;
        try { backend.generate(invalid_module); } catch (const CompilerError&) { failed = true; }
        require(failed && !backend.m_collect_function_candidates && backend.m_function_policies.empty(),
            "Generation exception leaked candidate phase state");
        IRCodeGenerator fresh(analyzer.getAllLocalSymbols(),analyzer.getFunctionSymbols(),data,config);
        require(AssemblyGenerator(backend.generate(module)).generate() == AssemblyGenerator(fresh.generate(module)).generate(),
            "Fresh and reused backends diverged after failed candidate emission");
        const std::string loop_source = "word main(){volatile word sum=0;for(word i=0;i<97;i++){if(i&1)sum+=2;else sum++;}return sum;}";
        Lexer loop_lexer(loop_source);
        const auto loop_tokens = loop_lexer.scanTokens(); Parser loop_parser(loop_tokens); auto loop_ast = loop_parser.parseProgram();
        DataSegmentManager loop_data; Analyzer loop_analyzer(loop_data); loop_analyzer.analyze(loop_ast);
        IRLowerer loop_lowerer; const auto loop = loop_lowerer.lower(loop_ast);
        IRCodeGenerator trials(loop_analyzer.getAllLocalSymbols(),loop_analyzer.getFunctionSymbols(),loop_data,config);
        trials.m_collect_function_candidates = true;
        auto trial = trials.generateOptimized(loop,OptimizationLevel::O2);
        IRModule scheduled;
        for (const auto& f : loop.functions) scheduled.functions.push_back(trials.m_function_candidates.at(f.name).function);
        require(trial.speed_score == trials.candidateSpeed(scheduled,trial.object) &&
            trial.function_metrics.at("main").speed_score == trials.candidateSpeed(scheduled,trial.object,&scheduled.functions[0]),
            "CACHE/uncached selection left metrics from a different scheduled object");
        const auto trial_metric = trial.function_metrics.at("main");
        trials.m_force_long_branches = true; trials.m_allow_auto_cache = true;
        auto cached = trials.generateInternal(scheduled);
        require(trials.m_had_cache_probe && cached.speed_score == trials.candidateSpeed(scheduled,cached.object),
            "Candidate metrics fixture lost automatic CACHE probing or its exact emitted score");
        const auto cached_metric = cached.function_metrics.at("main");
        trials.m_allow_auto_cache = false;
        const auto uncached = trials.generateInternal(scheduled);
        require(!trials.m_had_cache_probe && uncached.function_metrics.at("main").speed_score ==
            trials.candidateSpeed(scheduled,uncached.object,&scheduled.functions[0]) &&
            cached.function_metrics.at("main").speed_score == cached_metric.speed_score,
            "Uncached emission changed an earlier CACHE candidate's owned metrics");
        trials.m_collect_function_candidates = false;
        auto mixed = trials.generateMixedCandidate(loop);
        require(mixed.speed_score == trials.candidateSpeed(scheduled,mixed.object) &&
            mixed.function_metrics.at("main").speed_score == trials.candidateSpeed(scheduled,mixed.object,&scheduled.functions[0]) &&
            trial.function_metrics.at("main").speed_score == trial_metric.speed_score,
            "Mixed re-emission reused greedy scores or changed an earlier candidate's evidence");
    }
};

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

ObjectFile compile(const std::string& source, OptimizationLevel level, bool require_no_spills = false) {
    Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; const auto module = lowerer.lower(ast);
    CompilerConfig config; config.optimization = level; config.require_no_spills = require_no_spills;
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

void moduleBranchRelaxation() {
    const auto function = [](unsigned n) {
        std::string source = "word f" + std::to_string(n) + "(word x){if(x){";
        for (unsigned k = 0; k < 24; ++k)
            source += "*(volatile word*)0x100=x+" + std::to_string(k) + ";";
        return source + "}if(x==1)return 17;return 19;}";
    };
    std::string source;
    for (unsigned n = 0; n < 12; ++n) source += function(n);
    for (const auto level : {OptimizationLevel::O1, OptimizationLevel::O2}) {
        const auto single = compile(function(0), level), many = compile(source, level);
        require(many.code_section.size() == 12 * single.code_section.size(),
            "Module-wide branch widening depends on function count or expanded unrelated short branches");
        require(occurrences(AssemblyGenerator(many).generate(), "iwt r15") >= 12,
            "Relaxation stress no longer needs a long branch in each function");
    }
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

void reusableDivisionContexts() {
    const std::string source =
        "word signed_div(word a,word b,word c){return a/b+c/b;} "
        "unsigned word unsigned_div(unsigned word a,unsigned word b,unsigned word c){return a/b+c/b;} "
        "word main(){return signed_div(*(volatile word*)0x100,*(volatile word*)0x102,*(volatile word*)0x104)"
        "+(word)unsigned_div(*(volatile unsigned word*)0x106,*(volatile unsigned word*)0x108,*(volatile unsigned word*)0x10a);}";
    Lexer lexer(source);
    const auto tokens = lexer.scanTokens();
    Parser parser(tokens);
    auto ast = parser.parseProgram();
    DataSegmentManager data;
    Analyzer analyzer(data);
    analyzer.analyze(ast);
    IRLowerer lowerer;
    const auto module = lowerer.lower(ast);
    CompilerConfig config;
    config.optimization = OptimizationLevel::Size;
    IRCodeGenerator backend(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
    const auto expected = backend.generate(module);
    for (const auto* suffix : {"s", "u"}) {
        std::size_t kernels = 0;
        for (const auto& symbol : expected.symbol_table)
            kernels += symbol.name == std::string(1, '\x01') + "__disco_os_divmod_" + suffix;
        require(kernels == 1, "Signed and unsigned divisions must own distinct private helper contexts");
    }
    const auto assembly = AssemblyGenerator(expected).generate();
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        const auto actual = backend.generate(module);
        require(actual.code_section == expected.code_section && actual.data_section == expected.data_section &&
                AssemblyGenerator(actual).generate() == assembly,
                "Reusing a backend retained a temporary helper borrow or stale label state");
    }
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

void noSpillCandidates() {
    for (const auto level : {OptimizationLevel::Baseline,OptimizationLevel::O1,OptimizationLevel::O2,OptimizationLevel::Size}) {
        for (const auto source : {"word main(){return 42;}",
            "void opaque(word* p,word n);void allowed(word n){word local=42;opaque(&local,7);}"}) {
            const auto ordinary = compile(source,level), strict = compile(source,level,true);
            require(AssemblyGenerator(ordinary).generate()==AssemblyGenerator(strict).generate(),
                "No-spill validation changed a spill-free object or rejected explicit local/ABI storage");
        }
    }
    const std::string pressure =
        "word sink(word a,word b,word c,word d,word e,word f,word g,word h);"
        "word pressure(){word sum=0;for(word i=0;i<32;i++)sum+=*(volatile word*)0x200;"
        "return sum+sink(*(volatile word*)0x100,*(volatile word*)0x102,*(volatile word*)0x104,*(volatile word*)0x106,"
        "*(volatile word*)0x108,*(volatile word*)0x10a,*(volatile word*)0x10c,*(volatile word*)0x10e);}"
        "word last_spill_free(){return 42;}";
    require(AssemblyGenerator(compile(pressure,OptimizationLevel::O2)).generate().find("cache ;")!=std::string::npos,
        "Cached/uncached spill-witness fixture lost its automatic CACHE trial");
    for (const auto level : {OptimizationLevel::Baseline,OptimizationLevel::O1,OptimizationLevel::O2,OptimizationLevel::Size}) {
        bool rejected = false;
        try { compile(pressure,level,true); }
        catch (const CompilerError& error) {
            const auto& message = error.getMessage();
            require(message.find("--require-no-spills")!=std::string::npos && message.find("function 'pressure'")!=std::string::npos &&
                message.find("first value %")!=std::string::npos && message.find("mapped IR value(s)")!=std::string::npos &&
                message.find("unique 16-bit slot(s)")!=std::string::npos,
                "No-spill failure lost its selected candidate's function/value/category/physical-slot evidence");
            rejected = true;
        }
        require(rejected,"A later spill-free function/CACHE alternative erased a real spill witness");
    }
    for (const auto level : {OptimizationLevel::O2,OptimizationLevel::Size}) {
        bool rejected = false;
        try { compile("word paired(word a,word b){return a/b+a%b;}",level,true); }
        catch (const CompilerError& error) {
            require(error.getMessage().find("auxiliary divmod spill homes")!=std::string::npos,
                "No-spill diagnostic did not identify the compiler-generated secondary divmod word");
            rejected = true;
        }
        require(rejected,"An auxiliary divmod frame word escaped no-spill validation");
    }
    // O2's hardware counter leaves registers for the carried values and the
    // multiply temporary. The smaller size-policy countdown spills. Strict
    // Os must keep the earlier larger eligible object, not the last
    // candidate's mutable emitter state or the smallest bytes.
    // Three carried words plus the software counter exceed the widened pool.
    // The hardware counter still leaves a register for every carried word.
    const std::string loop = "word main(){word a=*(volatile word*)0x100;word b=*(volatile word*)0x102;"
        "word c=*(volatile word*)0x104;"
        "for(word i=0;i<32;i++){a+=b*3;b+=c*3;c+=a*3;}return a^b^c;}";
    const auto ordinary = compile(loop,OptimizationLevel::Size), strict = compile(loop,OptimizationLevel::Size,true);
    require(strict.code_section.size()>ordinary.code_section.size(),
        "Eligible-candidate fixture no longer skips a smaller ineligible Os object");
    require(AssemblyGenerator(strict).generate().find("loop ;")!=std::string::npos &&
            AssemblyGenerator(ordinary).generate().find("loop ;")==std::string::npos,
        "Strict Os did not preserve its spill-free hardware-counter candidate");
    require(AssemblyGenerator(strict).generate()==AssemblyGenerator(compile(loop,OptimizationLevel::Size,true)).generate(),
        "Eligible-candidate selection or spill evidence is nondeterministic");
    const auto linked = [](const ObjectFile& object) {
        auto bytes = object.code_section;
        for (const auto& relocation : object.relocation_table) {
            std::uint32_t address = 0; bool found = false;
            const std::string prefix = "__disco_stack_limit_";
            if (relocation.target_symbol_name.compare(0,prefix.size(),prefix)==0) {
                address=static_cast<std::uint32_t>(std::stoul(relocation.target_symbol_name.substr(prefix.size()))); found=true;
            }
            for (const auto& symbol : object.symbol_table) if (symbol.name==relocation.target_symbol_name && symbol.section==SymbolSection::CODE) {
                address=0x8006+symbol.offset; found=true;
            }
            require(found,"Missing strict-candidate execution relocation");
            bytes.at(relocation.patch_offset+1)=static_cast<std::uint8_t>(address);
            bytes.at(relocation.patch_offset+2)=static_cast<std::uint8_t>(address >> 8);
        }
        bytes.insert(bytes.begin(), {0xfc,0xef,0xbe,0xfd,0xfe,0xca});
        return bytes;
    };
    for (const auto* object : {&ordinary,&strict}) for (const unsigned initial : {0u,1u,32768u,65535u}) {
        DiscoGSU::Machine machine(linked(*object),0x8000);
        std::uint16_t values[] = {static_cast<std::uint16_t>(initial),5,7};
        for (unsigned n=0;n<3;++n) {
            machine.seed(0x700100+n*2,static_cast<std::uint8_t>(values[n]),false);
            machine.seed(0x700101+n*2,static_cast<std::uint8_t>(values[n] >> 8),false);
        }
        for (unsigned n=0;n<32;++n) {
            values[0]=static_cast<std::uint16_t>(values[0]+values[1]*3);
            values[1]=static_cast<std::uint16_t>(values[1]+values[2]*3);
            values[2]=static_cast<std::uint16_t>(values[2]+values[0]*3);
        }
        machine.run();
        // main deliberately STOPs with its own frame installed; unlike an
        // ordinary callee it does not pop the saved frame/link words.
        if (machine.reg(0)!=(values[0]^values[1]^values[2]) || machine.reg(6)!=0 || machine.reg(10)!=0x1ffc || machine.reg(9)!=0x1ffc ||
                machine.reg(11)!=0x5555 || machine.reg(12)!=0xbeef || machine.reg(13)!=0xcafe) {
            throw std::runtime_error("Spill-candidate execution/ABI mismatch: bytes=" + std::to_string(object->code_section.size()) +
                " input=" + std::to_string(initial) + " R0=" + std::to_string(machine.reg(0)) +
                " expected=" + std::to_string(values[0]^values[1]^values[2]) + " R6=" + std::to_string(machine.reg(6)) +
                " R9=" + std::to_string(machine.reg(9)) +
                " R10=" + std::to_string(machine.reg(10)) + " R11=" + std::to_string(machine.reg(11)) +
                " R12=" + std::to_string(machine.reg(12)) + " R13=" + std::to_string(machine.reg(13)));
        }
    }
}

void nearRomRegisterScratch() {
    for (const bool rom : {false, true}) {
        const unsigned width = rom ? 4 : 5;
        std::string source = "word main(){";
        for (unsigned n = 0; n < width; ++n)
            source += "word a" + std::to_string(n) + "=*(volatile word*)" + std::to_string(256 + n * 2) + ";";
        if (rom) source += "plot{color *(rom byte*)0x9000;pixel;flush;}";
        source += "return a0";
        for (unsigned n = 1; n < width; ++n) source += "+a" + std::to_string(n);
        source += ";}";
        for (const auto level : {OptimizationLevel::O1, OptimizationLevel::O2, OptimizationLevel::Size}) {
            const auto object = compile(source, level);
            if (!rom && isGlobalOptimization(level)) require(AssemblyGenerator(object).generate().find("to r4 ") != std::string::npos,
                "Near-ROM scratch reservation unnecessarily removed safe near-RAM R4 allocation");
            auto bytes = object.code_section;
            require(object.data_section.empty(), "Near-ROM scratch fixture unexpectedly acquired DATA");
            for (const auto& relocation : object.relocation_table) {
                std::uint32_t address = 0; bool found = false;
                const std::string prefix = "__disco_stack_limit_";
                if (relocation.target_symbol_name.compare(0, prefix.size(), prefix) == 0) {
                    address = static_cast<std::uint32_t>(std::stoul(relocation.target_symbol_name.substr(prefix.size()))); found = true;
                }
                if (relocation.target_symbol_name == "__disco_near_rom_bank") { address = 0; found = true; }
                for (const auto& symbol : object.symbol_table) if (symbol.name == relocation.target_symbol_name && symbol.section == SymbolSection::CODE) {
                    address = 0x8006 + symbol.offset; found = true;
                }
                require(found, "Missing near-ROM scratch fixture relocation");
                bytes.at(relocation.patch_offset + 1) = static_cast<std::uint8_t>(address);
                if (relocation.type != RelocationType::ADDR24_BANK)
                    bytes.at(relocation.patch_offset + 2) = static_cast<std::uint8_t>(address >> 8);
            }
            bytes.insert(bytes.begin(), {0xfc,0xef,0xbe,0xfd,0xfe,0xca});
            for (const auto initial : {0u, 32767u, 65535u}) {
                DiscoGSU::Machine machine(bytes, 0x008000);
                unsigned expected = initial;
                for (unsigned n = 0; n < width; ++n) {
                    const auto value = n ? n + 1 : initial;
                    if (n) expected += value;
                    machine.seed(0x700100 + n * 2, static_cast<std::uint8_t>(value), false);
                    machine.seed(0x700101 + n * 2, static_cast<std::uint8_t>(value >> 8), false);
                }
                machine.seed(0x009000, 149, true); machine.run();
                require(machine.reg(0) == static_cast<std::uint16_t>(expected) && machine.reg(6) == 0 &&
                    machine.reg(9) == 0x1ffc && machine.reg(10) == 0x1ffc && machine.reg(12) == 0xbeef && machine.reg(13) == 0xcafe,
                    "Near-ROM guard scratch corrupted a live scalar home or ABI restoration");
                for (unsigned n = 0; n < width; ++n) require(machine.accesses(0x700100 + n * 2, false) == 1 &&
                    machine.accesses(0x700100 + n * 2, true) == 0, "Near-ROM scratch reservation changed volatile reads");
                if (rom) require(machine.graphicsState("--plots") == 1 && machine.metrics().rom_reads == 1,
                    "Near-ROM reservation regression did not execute its guarded GETC/PLOT path");
            }
        }
    }
}
}

int main() {
    try {
        IRCodeGeneratorTestAccess::verify();
        divmod(); phiStress(); sharedTails(); moduleBranchRelaxation(); boundedFusion(); sizePolicy(); reusableDivisionContexts(); noSpillCandidates(); nearRomRegisterScratch();
        std::cout << "O2/Os divmod, exact-tail sharing, PHI interference and deterministic codegen checks passed\n";
    } catch (const CompilerError& e) { std::cerr << e.getMessage() << '\n'; return 1; }
      catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
