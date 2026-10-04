#include "ModuleLoader.hpp"
#include "ModuleInterface.hpp"
#include "ProjectManifest.hpp"
#include "Analyzer.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct Fixture {
    fs::path directory;
    Fixture() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 16; ++attempt) {
            directory = fs::temp_directory_path() / ("discoc-module-tests-" + std::to_string(stamp) + '-' + std::to_string(attempt));
            if (fs::create_directory(directory)) return;
        }
        throw std::runtime_error("Cannot create private module test directory");
    }
    ~Fixture() { std::error_code error; fs::remove_all(directory, error); }
    std::string path(const std::string& name) const { return (directory / name).generic_string(); }
    void write(const std::string& name, const std::string& source) const {
        fs::create_directories((directory / name).parent_path());
        std::ofstream file(path(name), std::ios::binary);
        file << source; file.flush();
        require(static_cast<bool>(file), "Cannot write module fixture");
    }
};
template <typename Operation>
void reject(Operation operation, const std::string& diagnostic) {
    try { operation(); }
    catch (const CompilerError& error) {
        require(error.getMessage().find(diagnostic) != std::string::npos, "Wrong module diagnostic: " + error.getMessage());
        return;
    }
    throw std::runtime_error("Expected module diagnostic: " + diagnostic);
}
std::size_t node(const ModuleLoader& loader, const std::string& path) {
    const auto& graph = loader.graph();
    for (std::size_t id = 0; id < graph.size(); ++id)
        if (DiscoProject::samePath(path, graph[id].path)) return id;
    throw std::runtime_error("Missing graph node: " + path);
}
void graphAndOwnership() {
    Fixture files;
    files.write("types.dc", "constexpr internal word PRIVATE = 2; constexpr word SIZE = PRIVATE + 1;\n"
        "@packed struct Value { byte bytes[PRIVATE]; word total; }; type Value = struct Value;\n"
        "enum Mode { ENABLED = PRIVATE }; word counter = 7; rom const word weights[] = {46,103};\n");
    files.write("math.dc", "import \"types.dc\"; internal word helper(word value) { return value + 1; }\n"
        "export word add(word a, word b) { counter++; return a + b; }\n");
    files.write("graphics.dc", "import \"./types.dc\"; word read_counter() { return counter; }\n");
    files.write("main.dc", "import \"math.dc\"; import \"graphics.dc\"; import \"./math.dc\";\n"
        "static_assert(SIZE == 3); static_assert(sizeof(Value) == 4); static_assert(ENABLED == 2);\n"
        "void main() { Value value; value.total = add(46,103); counter = read_counter(); }\n");
    ModuleLoader loader; CompilerConfig config;
    loader.prepare({files.path("main.dc"), files.path("math.dc")}, config);
    require(loader.graph().size() == 4, "Shared dependencies/explicit roots must be parsed once");
    const auto main = node(loader, files.path("main.dc"));
    const auto math = node(loader, files.path("math.dc"));
    const auto graphics = node(loader, files.path("graphics.dc"));
    const auto types = node(loader, files.path("types.dc"));
    require(loader.graph()[main].dependencies == std::vector<std::size_t>{math, graphics}, "Duplicate edge was not removed");
    require(loader.graph()[math].dependencies == std::vector<std::size_t>{types} &&
            loader.graph()[graphics].dependencies == std::vector<std::size_t>{types}, "Diamond edges are incorrect");
    const auto order = loader.implementationOrder();
    require(order == std::vector<std::string>{files.path("types.dc"), files.path("math.dc"), files.path("graphics.dc"), files.path("main.dc")},
            "Dependency-first compilation order is not deterministic");
    bool premature = false;
    try { (void)loader.take(files.path("main.dc")); } catch (const std::logic_error&) { premature = true; }
    require(premature, "Unanalyzed dependency interfaces were made visible");
    for (const auto& path : order) {
        auto loaded = loader.take(path);
        if (DiscoProject::samePath(path, files.path("main.dc"))) {
            std::size_t additions = 0, counters = 0, weights = 0;
            for (const auto& declaration : loaded.declarations) {
                require(declaration->token.lexeme != "PRIVATE" && declaration->token.lexeme != "helper", "Internal declaration leaked");
                if (const auto* function = dynamic_cast<const FunctionDeclStmt*>(declaration.get())) {
                    if (function->token.lexeme == "add") {
                        ++additions;
                        require(function->is_imported && function->is_prototype && function->body.empty(), "Imported function has a body");
                    }
                }
                if (const auto* global = dynamic_cast<const VarDeclStmt*>(declaration.get())) {
                    if (global->token.lexeme == "counter") { ++counters; require(global->is_extern && !global->initializer, "Imported RAM has storage"); }
                }
                if (const auto* data = dynamic_cast<const ConstDataStmt*>(declaration.get())) {
                    if (data->token.lexeme == "weights") { ++weights; require(data->is_extern && data->initializers.empty() && data->type.array_size == 2, "Imported ROM has storage or lost its extent"); }
                }
            }
            require(additions == 1 && counters == 1 && weights == 1, "Shared API was duplicated");
        }
        DataSegmentManager data; Analyzer analyzer(data, config.target);
        analyzer.analyze(loaded.declarations);
        loader.complete(path, loaded.declarations);
    }
    bool repeated = false;
    try { (void)loader.take(files.path("main.dc")); } catch (const std::logic_error&) { repeated = true; }
    require(repeated, "An AST was transferred twice");

    // Cached ASTs/exports own their text: later file changes cannot reparse or
    // mutate the snapshot, and different importers never share mutable nodes.
    loader.prepare({files.path("main.dc")}, config);
    fs::remove(files.path("types.dc"));
    auto cached = loader.take(files.path("types.dc"));
    require(cached.declarations.size() == 7, "Discovery did not retain the parsed AST");
    DataSegmentManager cached_data; Analyzer cached_analyzer(cached_data, config.target);
    cached_analyzer.analyze(cached.declarations); loader.complete(files.path("types.dc"), cached.declarations);
    auto left = loader.take(files.path("math.dc"));
    auto right = loader.take(files.path("graphics.dc"));
    require(left.declarations.front().get() != right.declarations.front().get(), "Importers share mutable AST ownership");
    left.declarations.front()->token.lexeme = "changed";
    require(right.declarations.front()->token.lexeme == "SIZE", "Mutating one interface changed another importer");
}
void pathsCyclesAndLimits() {
    Fixture files; CompilerConfig config; ModuleLoader loader;
    files.write("src/main.dc", "import \"lib.dc\"; void main() {}\n");
    files.write("one/lib.dc", "word first() { return 1; }\n");
    files.write("two/lib.dc", "word second() { return 2; }\n");
    loader.prepare({files.path("src/main.dc")}, config, {files.path("one"), files.path("two")});
    require(loader.graph().size() == 2 && DiscoProject::samePath(loader.graph()[1].path, files.path("one/lib.dc")), "Search path order was lost");
    files.write("src/lib.dc", "word local() { return 3; }\n");
    loader.prepare({files.path("src/main.dc")}, config, {files.path("one")});
    require(DiscoProject::samePath(loader.graph()[1].path, files.path("src/lib.dc")), "Importing directory did not win");
    files.write("src/lib.dc", "word local(\n");
    reject([&] { loader.prepare({files.path("src/main.dc")}, config, {files.path("one")}); }, "Expect type");
    require(loader.graph().empty() && loader.analysisOrder().empty(), "Failed discovery left a consumable graph");
    files.write("a.dc", "import \"b.dc\";\n");
    files.write("b.dc", "// cycle\nimport \"./a.dc\";\n");
    try { loader.prepare({files.path("a.dc")}, config); throw std::runtime_error("Cycle accepted"); }
    catch (const CompilerError& error) {
        require(error.getMessage().find("Cyclic module import:") != std::string::npos &&
                error.getMessage().find("a.dc ->") != std::string::npos && error.getMessage().find("b.dc ->") != std::string::npos,
                "Cycle diagnostic has no dependency chain");
        require(DiscoProject::samePath(error.getSourcePath(), files.path("b.dc")) && error.getLine() == 2, "Cycle source location is incorrect");
    }
    require(loader.graph().empty(), "Cycle left partial graph state");
    files.write("a.dc", "void main() {}\n");
    loader.prepare({files.path("a.dc")}, config); require(loader.graph().size() == 1, "Graph cannot be reused after failure");
    fs::create_hard_link(files.path("a.dc"), files.path("alias.dc"));
    files.write("root.dc", "import \"a.dc\"; import \"alias.dc\";\n");
    loader.prepare({files.path("root.dc")}, config);
    require(loader.graph().size() == 2 && loader.graph()[0].dependencies.size() == 1, "Physical path aliases were compiled twice");
    reject([&] { loader.prepare({files.path("a.dc")}, config, std::vector<std::string>(65, files.path("one"))); }, "Import path count exceeds 64");
    for (int id = 0; id < 33; ++id)
        files.write("depth" + std::to_string(id) + ".dc", id == 32 ? "" : "import \"depth" + std::to_string(id + 1) + ".dc\";\n");
    loader.prepare({files.path("depth1.dc")}, config); require(loader.graph().size() == 32, "Depth boundary rejected");
    reject([&] { loader.prepare({files.path("depth0.dc")}, config); }, "depth/count limit");
    std::vector<std::string> roots;
    for (int id = 0; id < 128; ++id) {
        const auto name = "wide" + std::to_string(id) + ".dc"; files.write(name, ""); roots.push_back(files.path(name));
    }
    loader.prepare(roots, config); require(loader.graph().size() == 128, "Count boundary rejected");
    files.write("wide0.dc", "import \"a.dc\";\n");
    reject([&] { loader.prepare(roots, config); }, "depth/count limit");
    fs::create_directory(files.path("directory.dc"));
    reject([&] { loader.prepare({files.path("directory.dc")}, config); }, "regular file");
}
} // namespace
int main() {
    try { graphAndOwnership(); pathsCyclesAndLimits(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Module graph and ownership checks passed\n"; return 0;
}
