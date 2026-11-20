// libFuzzer target for the compiler pipeline after parsing: semantic analysis,
// AST optimization, IR lowering and verification, IR global optimization and
// GSU code generation at every optimization level.
//
// Oracle: CompilerError and std::runtime_error are the compiler's diagnostics
// for malformed or unsupported source and are expected. Any other exception
// (std::logic_error such as std::out_of_range, std::bad_alloc, ...) escapes and
// aborts, as does an IR verifier rejection of IR lowered from source that the
// analyzer accepted. Sanitizer reports, crashes and timeouts are defects too.
#include "AST.hpp"
#include "Analyzer.hpp"
#include "AssemblyGenerator.hpp"
#include "CompilerError.hpp"
#include "DataSegment.hpp"
#include "IR.hpp"
#include "IRCodeGenerator.hpp"
#include "IRGlobalOptimizer.hpp"
#include "LanguageWarnings.hpp"
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Parser.hpp"
#include "Placement.hpp"
#include "TargetConfig.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Bounds per-input work: every compiler stage is at worst polynomial in the
// source size, so a small cap keeps each execution well below the timeout.
constexpr std::size_t MaxInputBytes = 4096;

[[noreturn]] void reportDefect(const char* stage, const std::string& message) {
    std::fprintf(stderr, "compiler fuzzer defect in %s: %s\n", stage, message.c_str());
    std::abort();
}

using Program = std::vector<std::unique_ptr<Stmt>>;

// Mirrors ModuleLoader::loadFile and runCompiler for one in-memory unit.
// Returns nothing for source the compiler rejects with a diagnostic, or for
// source that needs imported files the fuzzer cannot provide.
std::optional<Program> parseUnit(const std::string& source, const CompilerConfig& config) {
    try {
        Lexer lexer(source);
        const auto tokens = lexer.scanTokens();
        Parser parser(tokens);
        parser.getConfigForUpdate() = config;
        parser.parsePreamble();
        if (!parser.getImports().empty()) return std::nullopt;
        return parser.parseProgram();
    } catch (const CompilerError&) {
        return std::nullopt;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

const std::set<std::string>& allWarnings() {
    static const std::set<std::string> warnings = {"shadowing", "unused-variable", "unused-function",
        "uninitialized", "implicit-fallthrough", "unreachable", "expensive-helper"};
    return warnings;
}

struct AnalyzedUnit {
    Program program;
    DataSegmentManager data;
    std::unique_ptr<Analyzer> analyzer;
    IRModule ir;
};

// Front end through verified IR, in runCompiler's order. The returned unit
// owns everything the code generator borrows.
std::unique_ptr<AnalyzedUnit> analyze(const std::string& source, const CompilerConfig& config) {
    auto parsed = parseUnit(source, config);
    if (!parsed) return nullptr;
    auto unit = std::make_unique<AnalyzedUnit>();
    unit->program = std::move(*parsed);
    try {
        unit->analyzer = std::make_unique<Analyzer>(unit->data, config.target);
        unit->analyzer->analyze(unit->program);
        LanguageWarnings warnings;
        (void)warnings.check(unit->program, allWarnings());
        Optimizer optimizer(config.target);
        optimizer.optimize(unit->program);
        IRLowerer lowerer(config.target);
        unit->ir = lowerer.lower(unit->program);
        unit->ir.bitmap = unit->analyzer->getBitmapConfig();
    } catch (const CompilerError&) {
        return nullptr;
    } catch (const std::runtime_error&) {
        return nullptr;
    }
    try {
        IRVerifier::verify(unit->ir);
    } catch (const CompilerError& error) {
        reportDefect("IR verification of lowered source", error.what());
    }
    return unit;
}

// Optimization passes re-run IRVerifier on their output. The input IR already
// verified, so a verifier rejection there is a miscompilation, not a
// diagnostic, even though it is reported as a CompilerError.
void rejectVerifierFailure(const char* stage, const CompilerError& error) {
    static const std::string prefix = "IR verifier:";
    if (error.getMessage().compare(0, prefix.size(), prefix) == 0) reportDefect(stage, error.what());
}

void generate(const AnalyzedUnit& unit, CompilerConfig config, OptimizationLevel level) {
    config.optimization = level;
    try {
        IRCodeGenerator generator(unit.analyzer->getAllLocalSymbols(), unit.analyzer->getFunctionSymbols(),
            unit.data, config);
        const auto object = generator.generate(unit.ir);
        (void)AssemblyGenerator(object).generate();
    } catch (const CompilerError& error) {
        // Backend limits (cache, ranges, spills) are diagnostics.
        rejectVerifierFailure("GSU code generation", error);
    } catch (const std::runtime_error&) {
    }
    if (!isGlobalOptimization(level)) return;
    // The --check/--emit-ir path optimizes a copy of the verified IR.
    auto optimized = unit.ir;
    try {
        IRGlobalOptimizer::run(optimized, unit.analyzer->getAllLocalSymbols(), level);
        (void)dumpIR(optimized);
    } catch (const CompilerError& error) {
        rejectVerifierFailure("IR global optimization", error);
    } catch (const std::runtime_error&) {
    }
}

CompilerConfig configFor(TargetKind target) {
    CompilerConfig config;
    config.target = target;
    config.warn_on_cache_overflow = false;
    applyPlacement(config, PlacementOptions{});
    return config;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > MaxInputBytes) return -1;
    const std::string source(reinterpret_cast<const char*>(data), size);

    const auto gsu = configFor(TargetKind::GSU);
    if (const auto unit = analyze(source, gsu)) {
        for (const auto level : {OptimizationLevel::Baseline, OptimizationLevel::O1,
                                 OptimizationLevel::O2, OptimizationLevel::Size})
            generate(*unit, gsu, level);
    }

    // SPC700 has a target model and IR lowering but no code generator yet.
    (void)analyze(source, configFor(TargetKind::SPC700));
    return 0;
}
