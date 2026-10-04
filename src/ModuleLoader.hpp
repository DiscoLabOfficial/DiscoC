#pragma once
#include "Parser.hpp"
#include <set>

struct LoadedModule {
    std::vector<std::unique_ptr<Stmt>> declarations;
    std::vector<std::string> source_paths;
    CompilerConfig config;
};

// AST tokens own their names and source paths. No node borrows parser/token storage.
class ModuleLoader {
public:
    LoadedModule load(const std::string& path, const CompilerConfig& config);
private:
    void loadFile(const std::string& path, bool interface, std::vector<std::unique_ptr<Stmt>>& output);
    CompilerConfig m_config;
    std::size_t m_total_bytes = 0;
    std::set<std::string> m_active, m_loaded, m_module_names;
    // Export IDs index this compilation-owned table; they never borrow nodes
    // or entries invalidated by AST/table vector growth.
    std::vector<TypeAliasBinding> m_alias_bindings;
    std::map<std::string, std::vector<std::size_t>> m_interface_aliases;
};
