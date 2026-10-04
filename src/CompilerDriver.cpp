#include "CompilerDriver.hpp"
#include "ProjectManifest.hpp"
#include <iostream>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>
#include "ModuleLoader.hpp"
#include "Placement.hpp"
#include "LanguageWarnings.hpp"
#include "Analyzer.hpp"
#include "Optimizer.hpp"
#include "AssemblyGenerator.hpp"
#include "ASTPrinter.hpp"
#include "IRCodeGenerator.hpp"

namespace {
void usage() {
    std::cout << "Usage: discc [options] <file.dc> [-o output]\n"
        "  --config <discoc.toml>     Load project target/placement defaults\n"
        "  build [--config manifest] Compile and link project sources\n"
        "  --project <manifest>      Alias for build --config\n"
        "  --target <gsu|spc700>      Select target (default gsu)\n"
        "  --check                   Analyze and verify IR without emitting code\n"
        "  --emit-ast | --emit-ir | --emit-asm\n"
        "  --memory-mapping <lorom|hirom> (default lorom)\n"
        "  --execution-memory <rom|ram>  Select default execution region\n"
        "  --origin <24-bit-address>     Explicit fixed link origin\n"
        "  -Wall | -Werror | -Wno-<category> | -W<category>\n";
}
std::uint32_t addressArgument(const std::string& value) {
    std::size_t consumed = 0;
    if (value.empty() || value.front() == '-') throw std::runtime_error("Origin must fit in 24 bits.");
    const auto number = std::stoull(value, &consumed, 0);
    if (consumed != value.size() || number > 0xffffff) throw std::runtime_error("Origin must fit in 24 bits.");
    return static_cast<std::uint32_t>(number);
}
}
int runCompiler(std::vector<std::string> arguments) {
    std::string input, output;
    bool emit_ast = false, emit_ir = false, emit_asm = false, check = false, warnings_as_errors = false;
    CompilerConfig config;
    PlacementOptions placement;
    const std::set<std::string> known_warnings = {"shadowing", "unused-variable", "unused-function",
        "uninitialized", "implicit-fallthrough", "unreachable", "expensive-helper"};
    auto enabled_warnings = known_warnings;
    enabled_warnings.erase("expensive-helper");
    try {
        const auto manifest = DiscoProject::commandManifest(arguments, DiscoProject::Consumer::Compiler);
        arguments = DiscoProject::configurationArguments(arguments, DiscoProject::Consumer::Compiler, manifest.get());
        const auto argc = static_cast<int>(arguments.size());
        if (argc < 2) { usage(); return 1; }
        for (int index = 1; index < argc; ++index) {
            const std::string argument = arguments[index];
            const auto next = [&]() -> std::string {
                if (index + 1 >= argc) throw std::runtime_error(argument + " requires a value.");
                return arguments[++index];
            };
            if (argument == "--help" || argument == "-h") { usage(); return 0; }
            if (argument == "-o") output = next();
            else if (argument == "--target") {
                config.target = DiscoProject::parseTarget(next());
            } else if (argument == "--origin") { placement.origin = addressArgument(next()); placement.explicit_origin = true; }
            else if (argument == "--memory-mapping") {
                const auto name = next();
                if (name == "lorom") placement.mapping = MemoryMapping::LoROM;
                else if (name == "hirom") placement.mapping = MemoryMapping::HiROM;
                else throw std::runtime_error("Unsupported memory mapping; use lorom or hirom.");
            } else if (argument == "--execution-memory") {
                const auto name = next();
                if (name == "rom") placement.execution = PlacementOptions::Execution::Rom;
                else if (name == "ram") placement.execution = PlacementOptions::Execution::Ram;
                else throw std::runtime_error("Execution memory must be 'rom' or 'ram'.");
            } else if (argument == "--emit-ast") emit_ast = true;
            else if (argument == "--emit-ir") emit_ir = true;
            else if (argument == "--emit-asm") emit_asm = true;
            else if (argument == "--check") check = true;
            else if (argument == "-Wall") enabled_warnings = known_warnings;
            else if (argument == "-Werror") warnings_as_errors = true;
            else if (argument == "-Wno-cache-overflow") config.warn_on_cache_overflow = false;
            else if (argument.compare(0, 2, "-W") == 0) {
                const bool disable = argument.compare(0, 5, "-Wno-") == 0;
                const auto name = argument.substr(disable ? 5 : 2);
                if (!known_warnings.count(name)) throw std::runtime_error("Unknown warning category: " + name);
                if (disable) enabled_warnings.erase(name); else enabled_warnings.insert(name);
            } else if (!argument.empty() && argument.front() == '-') throw std::runtime_error("Unknown option: " + argument);
            else {
                if (!input.empty()) throw std::runtime_error("Only one input file can be specified.");
                input = argument;
            }
        }
        if (input.empty()) throw std::runtime_error("No input file specified.");
        applyPlacement(config, placement);
        ModuleLoader loader;
        auto loaded = loader.load(input, config);
        auto& program = loaded.declarations;
        DataSegmentManager data;
        Analyzer analyzer(data, config.target);
        analyzer.analyze(program);
        LanguageWarnings warnings;
        bool warning_failure = false;
        for (const auto& warning : warnings.check(program, enabled_warnings)) {
            if (!enabled_warnings.count(warning.category)) continue;
            std::cerr << (warning.source.source_path.empty() ? input : warning.source.source_path) << ':'
                << warning.source.line_number << ':' << warning.source.col_number << ": "
                << (warnings_as_errors ? "error" : "warning") << ": " << warning.message << " [-W" << warning.category << "]\n";
            warning_failure = warning_failure || warnings_as_errors;
        }
        if (warning_failure) return 1;
        Optimizer optimizer(config.target); optimizer.optimize(program);
        if (emit_ast) {
            ASTPrinter printer;
            std::cout << "Abstract Syntax Tree\n";
            for (const auto& statement : program) std::cout << printer.print(*statement) << '\n';
            return 0;
        }
        IRLowerer lowerer(config.target); auto ir = lowerer.lower(program);
        IRVerifier::verify(ir);
        if (emit_ir) { std::cout << dumpIR(ir); return 0; }
        if (check) return 0;
        if (config.target != TargetKind::GSU)
            throw CompilerError(std::string("Target '") + targetName(config.target) + "' has a target model but no code-generation backend yet.", 1, 1);
        if (output.empty()) {
            const auto dot = input.find_last_of('.');
            output = (dot == std::string::npos ? input : input.substr(0, dot)) + (emit_asm ? ".s" : ".o");
        }
        for (const auto& source : loaded.source_paths)
            if (DiscoProject::samePath(source, output)) throw std::runtime_error("Output path must not overwrite an input source/interface.");
        if (manifest) DiscoProject::protectManifest(*manifest, {output});
        IRCodeGenerator generator(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
        auto object = generator.generate(ir);
        if (emit_asm) {
            std::ofstream file(output);
            if (!file) throw std::runtime_error("Failed to open assembly output: " + output);
            file << AssemblyGenerator(object).generate(); file.flush();
            if (!file) throw std::runtime_error("Failed to write assembly output: " + output);
        } else object.write(output);
        std::cout << "Successfully generated " << output << '\n';
    } catch (const CompilerError& error) {
        std::cerr << (error.getSourcePath().empty() ? input : error.getSourcePath()) << ':' << error.getLine() << ':'
            << error.getCol() << ": error: " << error.getMessage() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n'; return 1;
    }
    return 0;
}
