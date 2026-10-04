#pragma once
#include "AST.hpp"
#include <set>
#include <map>

struct LanguageWarning { std::string category, message; Token source; };

// Conservative source-level definite assignment, not interprocedural alias analysis.
// The pass is read-only and never changes emitted code or suppresses diagnostics.
class LanguageWarnings {
public:
    std::vector<LanguageWarning> check(const std::vector<std::unique_ptr<Stmt>>& program, const std::set<std::string>& enabled);
private:
    using Initialized = std::set<SymbolId>;
    void warn(const std::string& category, const std::string& message, const Token& source);
    void expression(const Expr& value, bool reading = true);
    void statement(const Stmt& value);
    void statements(const std::vector<std::unique_ptr<Stmt>>& values, bool switch_body = false);
    bool terminates(const Stmt& value) const;
    Initialized m_initialized;
    std::map<SymbolId, Token> m_locals;
    std::set<SymbolId> m_used;
    std::vector<std::map<std::string, SymbolId>> m_scopes;
    std::set<std::string> m_called;
    std::vector<LanguageWarning> m_warnings;
    std::set<std::string> m_enabled;
};
