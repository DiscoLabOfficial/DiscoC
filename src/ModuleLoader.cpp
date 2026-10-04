#include "ModuleLoader.hpp"
#include "Lexer.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <array>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace {
std::string normalizedPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty() || path.find('\0') != std::string::npos) throw std::runtime_error("Invalid source path.");
    if (path.front() != '/' && !(path.size() > 1 && path[1] == ':')) {
        std::array<char, 4096> directory{};
#ifdef _WIN32
        if (!_getcwd(directory.data(), static_cast<int>(directory.size()))) throw std::runtime_error("Cannot resolve working directory.");
#else
        if (!getcwd(directory.data(), directory.size())) throw std::runtime_error("Cannot resolve working directory.");
#endif
        path = std::string(directory.data()) + "/" + path;
        std::replace(path.begin(), path.end(), '\\', '/');
    }
    const auto prefix = path.front() == '/' ? std::string("/") : path.substr(0, 3);
    std::vector<std::string> parts;
    std::istringstream input(path.substr(prefix.size()));
    std::string part;
    while (std::getline(input, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else parts.push_back(part);
    }
    std::string result = prefix;
    for (const auto& item : parts) { if (result.back() != '/') result += '/'; result += item; }
    return result;
}
bool interfaceDeclaration(const Stmt& statement) {
    if (const auto* function = dynamic_cast<const FunctionDeclStmt*>(&statement))
        return function->is_prototype && function->linkage != Linkage::Internal;
    if (const auto* global = dynamic_cast<const VarDeclStmt*>(&statement))
        return (global->is_extern && !global->initializer && global->linkage != Linkage::Internal) || global->is_constexpr;
    return dynamic_cast<const StructDefStmt*>(&statement) || dynamic_cast<const EnumDeclStmt*>(&statement) ||
        dynamic_cast<const StaticAssertStmt*>(&statement) || dynamic_cast<const TypeAliasDeclStmt*>(&statement);
}
}
LoadedModule ModuleLoader::load(const std::string& path, const CompilerConfig& config) {
    m_config = config; m_total_bytes = 0; m_active.clear(); m_loaded.clear(); m_module_names.clear();
    m_alias_bindings.clear(); m_interface_aliases.clear();
    LoadedModule result; result.config = config;
    loadFile(normalizedPath(path), false, result.declarations);
    result.source_paths.assign(m_loaded.begin(), m_loaded.end());
    return result;
}
void ModuleLoader::loadFile(const std::string& path, bool interface, std::vector<std::unique_ptr<Stmt>>& output) {
    if (m_active.count(path)) throw CompilerError("Cyclic interface import.", 1, 1);
    if (m_loaded.count(path)) return;
    if (m_active.size() >= 32 || m_loaded.size() + m_active.size() >= 128)
        throw CompilerError("Module import depth/count limit exceeded.", 1, 1);
    m_active.insert(path);
    try {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) throw CompilerError("Cannot open module interface/source: " + path, 1, 1);
        const auto size = file.tellg();
        if (size < 0 || size > 16 * 1024 * 1024 || static_cast<std::size_t>(size) > 32 * 1024 * 1024 - m_total_bytes)
            throw CompilerError("Module source exceeds per-file/total byte limit.", 1, 1);
        std::string source(static_cast<std::size_t>(size), '\0');
        file.seekg(0);
        if (!source.empty() && !file.read(&source[0], size)) throw CompilerError("Truncated module source.", 1, 1);
        m_total_bytes += source.size();
        Lexer lexer(source); auto tokens = lexer.scanTokens();
        for (auto& token : tokens) token.source_path = path;
        Parser parser(tokens); parser.getConfigForUpdate() = m_config;
        parser.parsePreamble();
        if (interface && parser.getModuleName().empty()) throw CompilerError("An interface requires a module declaration.", 1, 1);
        if (interface && !parser.getModuleName().empty() && !m_module_names.insert(parser.getModuleName()).second)
            throw CompilerError("Duplicate module name '" + parser.getModuleName() + "'.", 1, 1);
        std::set<std::size_t> exported_aliases;
        for (const auto& import : parser.getImports()) {
            const auto& name = import.lexeme;
            if (name.size() < 4 || name.substr(name.size() - 4) != ".dci" || name.find('\0') != std::string::npos)
                throw CompilerError("Imports require a .dci interface path.", import);
            const auto import_path = normalizedPath(path.substr(0, path.find_last_of('/') + 1) + name);
            loadFile(import_path, true, output);
            for (const auto id : m_interface_aliases.at(import_path)) {
                parser.addImportedAlias(m_alias_bindings.at(id));
                exported_aliases.insert(id);
            }
        }
        auto declarations = parser.parseProgram();
        for (auto& declaration : declarations) {
            if (interface && !interfaceDeclaration(*declaration))
                throw CompilerError("Interfaces allow types/aliases, constexpr, assertions, prototypes and extern storage only.", declaration->token);
            if (const auto* alias = dynamic_cast<const TypeAliasDeclStmt*>(declaration.get())) {
                if (m_alias_bindings.size() >= MaxTypeAliases)
                    throw CompilerError("Compilation type alias count exceeds 4096.", alias->token);
                exported_aliases.insert(m_alias_bindings.size());
                m_alias_bindings.push_back({alias->resolved_type, alias->token});
            }
            output.push_back(std::move(declaration));
        }
        m_interface_aliases.emplace(path, std::vector<std::size_t>(exported_aliases.begin(), exported_aliases.end()));
        m_active.erase(path); m_loaded.insert(path);
    } catch (CompilerError& error) { error.setSourcePath(path); throw; }
}
