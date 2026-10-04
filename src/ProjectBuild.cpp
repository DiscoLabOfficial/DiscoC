#include "ProjectBuild.hpp"
#include "ProjectManifest.hpp"
#include "CompilerDriver.hpp"
#include "LinkerDriver.hpp"
#include "ModuleLoader.hpp"
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <utility>

namespace {
std::string objectName(const std::string& source, std::size_t index) {
    auto stem = source.substr(source.find_last_of('/') + 1);
    const auto dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.resize(dot);
    for (auto& c : stem)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) c = '_';
    if (stem.size() > 64) stem.resize(64);
    return std::to_string(index) + '-' + stem + ".o";
}
void projectUsage() {
    std::cout << "Usage: discc build [--config discoc.toml] [options]\n"
        "       discc --project discoc.toml [options]\n"
        "  --target <gsu|spc700> --origin <address> --memory-mapping <lorom|hirom>\n"
        "  --execution-memory <rom|ram> --ram-bank <0|1> --ram-origin <word>\n"
        "  --rom-bank <bank> --stack-pointer <word> --entry <symbol>\n"
        "  --init-runtime | --no-init-runtime\n"
        "  --host-initialized-globals | --no-host-initialized-globals\n"
        "  --output-dir <directory> -o <binary> --emit-asm <file> --no-emit-asm\n"
        "  --check (frontend/IR only, no outputs or linking)\n"
        "  -Wall | -Werror | -W<category> | -Wno-<category>\n";
}
} // namespace

int buildProject(const std::vector<std::string>& arguments) {
    using namespace DiscoProject;
    try {
        if (arguments.size() < 2 || arguments.size() > 1024) throw std::runtime_error("Invalid project command.");
        std::string path, output_directory;
        bool check = false;
        std::vector<std::string> compile_flags, link_flags;
        const std::set<std::string> shared = {"--target", "--origin", "--memory-mapping", "--execution-memory"};
        const std::set<std::string> link_values = {"--ram-bank", "--ram-origin", "--rom-bank", "--stack-pointer", "--entry", "--emit-asm", "-o"};
        const std::set<std::string> link_switches = {"--init-runtime", "--no-init-runtime", "--host-initialized-globals", "--no-host-initialized-globals", "--no-emit-asm"};
        for (std::size_t index = arguments[1] == "build" ? 2 : 1; index < arguments.size(); ++index) {
            const auto flag = arguments[index];
            const auto next = [&]() -> std::string {
                if (index + 1 == arguments.size() || arguments[index + 1].empty()) throw std::runtime_error(flag + " requires a value.");
                return arguments[++index];
            };
            if (flag == "--help" || flag == "-h") { projectUsage(); return 0; }
            if (flag == "--config" || flag == "--project") {
                if (!path.empty()) throw std::runtime_error("Only one project manifest can be specified.");
                path = next();
            } else if (flag == "--output-dir") output_directory = absolutePath(next());
            else if (flag == "--check") check = true;
            else if (shared.count(flag)) {
                const auto value = next();
                compile_flags.insert(compile_flags.end(), {flag, value});
                link_flags.insert(link_flags.end(), {flag, value});
            } else if (link_values.count(flag)) {
                const auto value = next(); link_flags.insert(link_flags.end(), {flag, value});
            } else if (link_switches.count(flag)) link_flags.push_back(flag);
            else if (flag.compare(0, 2, "-W") == 0) compile_flags.push_back(flag);
            else throw std::runtime_error("Unknown project-build option: " + flag);
        }
        if (path.empty()) path = "discoc.toml";
        const auto manifest = Manifest::read(absolutePath(path));
        const auto* list = manifest.find("project.sources");
        if (!list || list->strings.empty()) throw std::runtime_error("Project build requires a nonempty project.sources array.");
        const auto base = parentPath(manifest.path);
        if (output_directory.empty()) output_directory = absolutePath(manifest.string("output.directory", "build"), base);
        std::vector<std::string> sources, objects;
        for (const auto& name : list->strings) {
            const auto source = absolutePath(name, base);
            if (source.size() < 3 || source.substr(source.size() - 3) != ".dc") throw std::runtime_error("Project sources must be .dc implementations, not interfaces/objects.");
            for (const auto& previous : sources)
                if (samePath(source, previous)) throw std::runtime_error("Duplicate project source: " + name);
            objects.push_back(absolutePath(objectName(source, sources.size()), output_directory));
            sources.push_back(source);
        }
        std::vector<std::string> compile{"discc", "--config", manifest.path};
        compile.insert(compile.end(), compile_flags.begin(), compile_flags.end());
        compile = configurationArguments(compile, Consumer::Compiler, &manifest);
        auto target = TargetKind::GSU;
        for (std::size_t index = 1; index < compile.size(); ++index) {
            if (compile[index] == "--target") target = parseTarget(compile.at(++index));
            else if (optionTakesValue(compile[index], Consumer::Compiler)) ++index;
        }
        if (!check && target != TargetKind::GSU)
            throw std::runtime_error("Target 'spc700' has a target model but no code-generation backend yet; use build --check.");
        std::vector<std::string> link{"discld", "--config", manifest.path, "-o",
            absolutePath(manifest.string("output.binary", manifest.string("project.name", "new") + ".bin"), output_directory)};
        if (manifest.find("output.assembly"))
            link.insert(link.end(), {"--emit-asm", absolutePath(manifest.string("output.assembly"), output_directory)});
        link.insert(link.end(), link_flags.begin(), link_flags.end());
        link = configurationArguments(link, Consumer::Linker, &manifest);
        std::string binary, assembly;
        for (std::size_t index = 1; index < link.size(); ++index) {
            if (link[index] == "-o") binary = link.at(++index);
            else if (link[index] == "--emit-asm") assembly = link.at(++index);
            else if (link[index] == "--no-emit-asm") assembly.clear();
            else if (optionTakesValue(link[index], Consumer::Linker)) ++index;
        }
        if (!check) {
            auto outputs = objects; outputs.push_back(binary);
            if (!assembly.empty()) outputs.push_back(assembly);
            protectManifest(manifest, outputs);
            for (std::size_t index = 0; index < outputs.size(); ++index)
                for (std::size_t other = 0; other < index; ++other)
                    if (samePath(outputs[index], outputs[other])) throw std::runtime_error("Project output paths must be different.");
            // Discover imported interfaces before writing any object, including
            // imports used only by a later compilation unit. This syntax-only
            // preflight also prevents output aliases from destroying inputs.
            CompilerConfig config; config.target = target;
            std::set<std::string> inputs;
            for (const auto& source : sources) {
                ModuleLoader loader;
                const auto loaded = loader.load(source, config);
                inputs.insert(loaded.source_paths.begin(), loaded.source_paths.end());
            }
            for (const auto& input : inputs)
                for (const auto& output : outputs)
                    if (samePath(input, output)) throw std::runtime_error("Project output must not overwrite a source/interface.");
            createDirectories(output_directory);
            createDirectories(parentPath(binary));
            if (!assembly.empty()) createDirectories(parentPath(assembly));
        }
        for (std::size_t index = 0; index < sources.size(); ++index) {
            auto unit = compile;
            unit.push_back(sources[index]);
            if (check) unit.push_back("--check");
            else unit.insert(unit.end(), {"-o", objects[index]});
            if (runCompiler(std::move(unit)) != 0) return 1; // Never link stale objects after a failure.
        }
        if (check) { std::cout << "Project frontend/IR checks passed (no linking).\n"; return 0; }
        link.insert(link.end(), objects.begin(), objects.end());
        return runLinker(std::move(link));
    } catch (const CompilerError& error) {
        std::cerr << error.getSourcePath() << ':' << error.getLine() << ':' << error.getCol()
                  << ": error: " << error.getMessage() << '\n'; return 1;
    } catch (const std::exception& error) {
        std::cerr << "Project Error: " << error.what() << '\n'; return 1;
    }
}
