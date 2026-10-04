#pragma once
#include "Parser.hpp"
#include <set>

struct LoadedModule {
    std::vector<std::unique_ptr<Stmt>> declarations;
    std::vector<std::string> source_paths;
};

struct ModuleNode {
    std::string path;
    bool is_interface = false;
    std::vector<std::size_t> dependencies;
};

// One build owns one bounded graph. Files are parsed once; each implementation
// transfers its AST to exactly one compiler invocation. Imported APIs are owned
// projections, so analysis never mutates another unit or borrows its AST.
class ModuleLoader {
public:
    void prepare(const std::vector<std::string>& roots, const CompilerConfig& config,
                 const std::vector<std::string>& import_paths = {});
    // Transfer only after dependencies are analyzed, then publish the resolved
    // public API once. The caller owns the returned AST during compilation.
    LoadedModule take(const std::string& path);
    void complete(const std::string& path, const std::vector<std::unique_ptr<Stmt>>& program);
    std::vector<std::string> analysisOrder() const;
    std::vector<std::string> implementationOrder() const;
    std::vector<std::string> sourcePaths() const;
    // Borrowed until the next prepare call or destruction.
    const std::vector<ModuleNode>& graph() const { return m_graph; }
    TargetKind target() const { return m_config.target; }
private:
    struct Record {
        std::vector<std::unique_ptr<Stmt>> declarations, exports;
        std::vector<std::size_t> aliases;
        // prepare -> take -> complete; exports exist only after analysis.
        bool loaded = false, taken = false, completed = false;
    };
    std::size_t findFile(const std::string& path) const;
    std::size_t loadFile(const std::string& path, bool interface, const Token& origin);
    std::string resolveImport(const std::string& owner, const Token& import) const;
    void gather(std::size_t id, std::set<std::size_t>& visited, std::vector<std::size_t>& order) const;
    CompilerConfig m_config;
    std::size_t m_total_bytes = 0;
    std::vector<std::string> m_import_paths;
    std::vector<ModuleNode> m_graph;
    std::vector<std::unique_ptr<Record>> m_records;
    std::vector<std::size_t> m_active, m_order;
    std::set<std::string> m_module_names;
    std::vector<TypeAliasBinding> m_alias_bindings;
};
