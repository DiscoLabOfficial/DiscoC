#include "ModuleLoader.hpp"
#include "ModuleInterface.hpp"
#include "ProjectManifest.hpp"
#include "Lexer.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <sys/stat.h>

namespace {
constexpr std::size_t Missing = std::numeric_limits<std::size_t>::max();
bool interfaceDeclaration(const Stmt& statement) {
    if (const auto* function = dynamic_cast<const FunctionDeclStmt*>(&statement))
        return function->is_prototype && function->linkage != Linkage::Internal;
    if (const auto* global = dynamic_cast<const VarDeclStmt*>(&statement))
        return (global->is_extern && !global->initializer && global->linkage != Linkage::Internal) || global->is_constexpr;
    return dynamic_cast<const StructDefStmt*>(&statement) || dynamic_cast<const EnumDeclStmt*>(&statement) ||
        dynamic_cast<const StaticAssertStmt*>(&statement) || dynamic_cast<const TypeAliasDeclStmt*>(&statement);
}
bool isInterface(const std::string& path) {
    return path.size() >= 4 && path.substr(path.size() - 4) == ".dci";
}
bool sameSpelling(const std::string& left, const std::string& right) {
#if defined(_WIN32) || defined(__DJGPP__)
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
#else
    return left == right;
#endif
}
}

std::size_t ModuleLoader::findFile(const std::string& path) const {
    for (std::size_t index = 0; index < m_graph.size(); ++index)
        // Both names are already absolute/normalized. Avoid repeatedly walking
        // every parent directory for distinct files in a bounded wide graph.
        if (sameSpelling(path, m_graph[index].path) || DiscoProject::sameExistingFile(path, m_graph[index].path)) return index;
    return Missing;
}

void ModuleLoader::prepare(const std::vector<std::string>& roots, const CompilerConfig& config,
                           const std::vector<std::string>& import_paths) {
    m_config = config; m_total_bytes = 0;
    m_graph.clear(); m_records.clear(); m_active.clear(); m_order.clear();
    m_module_names.clear(); m_alias_bindings.clear(); m_import_paths.clear();
    if (roots.empty() || roots.size() > 128) throw CompilerError("Module root count exceeds 128 or is empty.", 1, 1);
    if (import_paths.size() > 64) throw CompilerError("Import path count exceeds 64.", 1, 1);
    try {
        for (const auto& directory : import_paths) m_import_paths.push_back(DiscoProject::absolutePath(directory));
        for (const auto& root : roots) {
            const auto path = DiscoProject::absolutePath(root);
            loadFile(path, isInterface(path), Token(TokenType::LITERAL_STRING, path, 1, 1));
        }
    } catch (...) {
        // Discovery is transactional: no partial graph or AST can be consumed.
        m_graph.clear(); m_records.clear(); m_active.clear(); m_order.clear();
        m_module_names.clear(); m_alias_bindings.clear(); m_import_paths.clear(); m_total_bytes = 0;
        throw;
    }
}

std::string ModuleLoader::resolveImport(const std::string& owner, const Token& import) const {
    std::vector<std::string> directories{DiscoProject::parentPath(owner)};
    directories.insert(directories.end(), m_import_paths.begin(), m_import_paths.end());
    for (const auto& directory : directories) {
        const auto path = DiscoProject::absolutePath(import.lexeme, directory);
        struct stat info{};
        if (stat(path.c_str(), &info) == 0) {
            if ((info.st_mode & S_IFMT) != S_IFREG) throw CompilerError("Imported module must be a regular file: " + path, import);
            return path; // A local file takes precedence; parsing errors never trigger fallback.
        }
    }
    throw CompilerError("Cannot open module interface/source: " + import.lexeme +
        " (searched the importing directory and configured import paths).", import);
}

std::size_t ModuleLoader::loadFile(const std::string& path, bool interface, const Token& origin) {
    auto id = findFile(path);
    if (id != Missing) {
        if (m_graph.at(id).is_interface != interface)
            throw CompilerError("A file cannot be imported as both source and interface.", origin);
        if (std::find(m_active.begin(), m_active.end(), id) != m_active.end()) {
            std::string chain;
            for (const auto active : m_active) chain += m_graph.at(active).path + " -> ";
            chain += m_graph.at(id).path;
            throw CompilerError(std::string(interface ? "Cyclic interface import: " : "Cyclic module import: ") + chain, origin);
        }
        if (m_records.at(id)->loaded) return id;
    }
    if (m_active.size() >= 32 || m_graph.size() >= 128)
        throw CompilerError("Module import depth/count limit exceeded.", origin);
    id = m_graph.size();
    m_graph.push_back({path, interface, {}});
    m_records.push_back(std::make_unique<Record>());
    m_active.push_back(id);
    try {
        struct stat info{};
        if (stat(path.c_str(), &info) == 0 && (info.st_mode & S_IFMT) != S_IFREG)
            throw CompilerError("Module must be a regular file: " + path, origin);
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) throw CompilerError("Cannot open module interface/source: " + path, origin);
        const auto size = file.tellg();
        if (size < 0 || size > 16 * 1024 * 1024 || static_cast<std::size_t>(size) > 32 * 1024 * 1024 - m_total_bytes)
            throw CompilerError("Module source exceeds per-file/total byte limit.", origin);
        std::string source(static_cast<std::size_t>(size), '\0');
        file.seekg(0);
        if (!source.empty() && !file.read(&source[0], size)) throw CompilerError("Truncated module source.", origin);
        m_total_bytes += source.size();
        Lexer lexer(source); auto tokens = lexer.scanTokens();
        for (auto& token : tokens) token.source_path = path;
        Parser parser(tokens); parser.getConfigForUpdate() = m_config; parser.parsePreamble();
        if (interface && parser.getModuleName().empty()) throw CompilerError("An interface requires a module declaration.", 1, 1);
        if (interface && !m_module_names.insert(parser.getModuleName()).second)
            throw CompilerError("Duplicate module name '" + parser.getModuleName() + "'.", 1, 1);
        std::set<std::size_t> aliases;
        for (const auto& import : parser.getImports()) {
            const auto dependency = resolveImport(path, import);
            const auto dependency_id = loadFile(dependency, isInterface(dependency), import);
            // Recursive insertion may grow the graph; retain IDs, not references.
            auto& edges = m_graph.at(id).dependencies;
            if (std::find(edges.begin(), edges.end(), dependency_id) == edges.end()) edges.push_back(dependency_id);
            for (const auto alias : m_records.at(dependency_id)->aliases) {
                parser.addImportedAlias(m_alias_bindings.at(alias)); aliases.insert(alias);
            }
        }
        auto declarations = parser.parseProgram();
        auto& record = *m_records.at(id);
        for (const auto& declaration : declarations) {
            if (interface && !interfaceDeclaration(*declaration))
                throw CompilerError("Interfaces allow types/aliases, constexpr, assertions, prototypes and extern storage only.", declaration->token);
            if (const auto* alias = dynamic_cast<const TypeAliasDeclStmt*>(declaration.get())) {
                if (m_alias_bindings.size() >= MaxTypeAliases) throw CompilerError("Compilation type alias count exceeds 4096.", alias->token);
                aliases.insert(m_alias_bindings.size());
                m_alias_bindings.push_back({alias->resolved_type, alias->token});
            }
        }
        record.declarations = std::move(declarations);
        record.aliases.assign(aliases.begin(), aliases.end());
        record.loaded = true; m_active.pop_back(); m_order.push_back(id);
        return id;
    } catch (CompilerError& error) { error.setSourcePath(path); throw; }
}

void ModuleLoader::gather(std::size_t id, std::set<std::size_t>& visited, std::vector<std::size_t>& order) const {
    if (!visited.insert(id).second) return;
    for (const auto dependency : m_graph.at(id).dependencies) gather(dependency, visited, order);
    order.push_back(id);
}

LoadedModule ModuleLoader::take(const std::string& path) {
    const auto id = findFile(DiscoProject::absolutePath(path));
    if (id == Missing || !m_records.at(id)->loaded || m_records.at(id)->taken)
        throw std::logic_error("Module AST must be transferred exactly once after discovery.");
    LoadedModule result;
    std::set<std::size_t> visited; std::vector<std::size_t> order;
    gather(id, visited, order);
    for (const auto dependency : order) {
        result.source_paths.push_back(m_graph.at(dependency).path);
        if (dependency == id) continue;
        if (!m_records.at(dependency)->completed)
            throw std::logic_error("Module dependencies must be analyzed before their importer.");
        for (const auto& declaration : m_records.at(dependency)->exports)
            if (auto api = moduleInterface(*declaration, m_graph.at(dependency).is_interface))
                result.declarations.push_back(std::move(api));
    }
    auto& record = *m_records.at(id);
    for (auto& declaration : record.declarations) result.declarations.push_back(std::move(declaration));
    record.declarations.clear(); record.taken = true;
    return result;
}

void ModuleLoader::complete(const std::string& path, const std::vector<std::unique_ptr<Stmt>>& program) {
    const auto id = findFile(DiscoProject::absolutePath(path));
    if (id == Missing || !m_records.at(id)->taken || m_records.at(id)->completed)
        throw std::logic_error("A transferred module must be published exactly once after analysis.");
    std::vector<std::unique_ptr<Stmt>> exports;
    for (const auto& declaration : program)
        if (!declaration->is_imported)
            if (auto api = moduleInterface(*declaration, m_graph.at(id).is_interface)) exports.push_back(std::move(api));
    m_records.at(id)->exports = std::move(exports);
    m_records.at(id)->completed = true;
}

std::vector<std::string> ModuleLoader::implementationOrder() const {
    std::vector<std::string> result;
    for (const auto id : m_order) if (!m_graph.at(id).is_interface) result.push_back(m_graph.at(id).path);
    return result;
}
std::vector<std::string> ModuleLoader::analysisOrder() const {
    std::vector<std::string> result;
    for (const auto id : m_order) result.push_back(m_graph.at(id).path);
    return result;
}
std::vector<std::string> ModuleLoader::sourcePaths() const {
    std::vector<std::string> result;
    for (const auto& node : m_graph) result.push_back(node.path);
    return result;
}
