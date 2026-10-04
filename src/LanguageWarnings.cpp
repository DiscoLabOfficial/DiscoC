#include "LanguageWarnings.hpp"
#include <algorithm>
#include <iterator>

void LanguageWarnings::warn(const std::string& category, const std::string& message, const Token& source) {
    if (m_enabled.count(category) && m_warnings.size() < 256) m_warnings.push_back({category, message, source});
}
void LanguageWarnings::expression(const Expr& value, bool reading) {
    if (const auto* pixel = dynamic_cast<const ReadPixelExpr*>(&value)) {
        if (pixel->x) { expression(*pixel->x); expression(*pixel->y); }
        return;
    }
    if (const auto* variable = dynamic_cast<const VariableExpr*>(&value)) {
        m_used.insert(variable->symbol_id);
        if (reading && m_locals.count(variable->symbol_id) && !m_initialized.count(variable->symbol_id) &&
            value.result_type.array_size == 0 && value.result_type.base != BaseType::STRUCT)
            warn("uninitialized", "Possible read of uninitialized local '" + value.token.lexeme + "'.", value.token);
    } else if (const auto* assignment = dynamic_cast<const AssignExpr*>(&value)) {
        expression(*assignment->name, false); expression(*assignment->value);
        if (const auto* variable = dynamic_cast<const VariableExpr*>(assignment->name.get())) m_initialized.insert(variable->symbol_id);
    } else if (const auto* update = dynamic_cast<const UpdateExpr*>(&value)) {
        expression(*update->target); expression(*update->value);
        if (const auto* variable = dynamic_cast<const VariableExpr*>(update->target.get())) m_initialized.insert(variable->symbol_id);
    } else if (const auto* binary = dynamic_cast<const BinaryExpr*>(&value)) {
        expression(*binary->left); const auto incoming = m_initialized;
        expression(*binary->right);
        if (binary->token.type == TokenType::AND_AND || binary->token.type == TokenType::OR_OR) m_initialized = incoming;
        if ((binary->token.type == TokenType::SLASH || binary->token.type == TokenType::PERCENT) && !binary->is_constant)
            warn("expensive-helper", "Division/remainder uses a software runtime sequence on this target.", binary->token);
    } else if (const auto* unary = dynamic_cast<const UnaryExpr*>(&value)) expression(*unary->right);
    else if (const auto* address = dynamic_cast<const AddressOfExpr*>(&value)) expression(*address->right, false);
    else if (const auto* dereference = dynamic_cast<const DereferenceExpr*>(&value)) expression(*dereference->right);
    else if (const auto* subscript = dynamic_cast<const SubscriptExpr*>(&value)) { expression(*subscript->array); expression(*subscript->index); }
    else if (const auto* member = dynamic_cast<const MemberAccessExpr*>(&value)) expression(*member->object, false);
    else if (const auto* cast = dynamic_cast<const CastExpr*>(&value)) expression(*cast->expression);
    else if (const auto* call = dynamic_cast<const CallExpr*>(&value)) {
        m_called.insert(call->callee->token.lexeme);
        for (const auto& argument : call->arguments) expression(*argument);
    }
    // sizeof/alignof operands are unevaluated and therefore not reads.
}
bool LanguageWarnings::terminates(const Stmt& value) const {
    if (dynamic_cast<const ReturnStmt*>(&value) || dynamic_cast<const BreakStmt*>(&value) || dynamic_cast<const ContinueStmt*>(&value)) return true;
    if (const auto* block = dynamic_cast<const BlockStmt*>(&value)) {
        for (const auto& child : block->statements) if (terminates(*child)) return true;
    }
    if (const auto* plot = dynamic_cast<const PlotBlockStmt*>(&value)) return terminates(*plot->body);
    if (const auto* conditional = dynamic_cast<const IfStmt*>(&value))
        return conditional->elseBranch && terminates(*conditional->thenBranch) && terminates(*conditional->elseBranch);
    return false;
}
void LanguageWarnings::statements(const std::vector<std::unique_ptr<Stmt>>& values, bool switch_body) {
    bool unreachable = false, has_arm = false, arm_has_code = false, marked = false;
    const auto switch_incoming = m_initialized;
    for (const auto& value : values) {
        const bool label = dynamic_cast<const CaseStmt*>(value.get()) || dynamic_cast<const DefaultStmt*>(value.get());
        if (label && switch_body) {
            if (has_arm && arm_has_code && !unreachable && !marked)
                warn("implicit-fallthrough", "Switch arm falls through; add fallthrough; or break;.", value->token);
            unreachable = false; has_arm = true; arm_has_code = false; marked = false;
            m_initialized = switch_incoming;
        } else {
            if (unreachable) warn("unreachable", "Unreachable statement.", value->token);
            arm_has_code = true; marked = dynamic_cast<const FallthroughStmt*>(value.get()) != nullptr;
        }
        statement(*value);
        unreachable = unreachable || terminates(*value);
    }
}
void LanguageWarnings::statement(const Stmt& value) {
    if (const auto* block = dynamic_cast<const BlockStmt*>(&value)) {
        m_scopes.emplace_back(); statements(block->statements); m_scopes.pop_back();
    } else if (const auto* declaration = dynamic_cast<const VarDeclStmt*>(&value)) {
        if (declaration->is_global) return;
        for (const auto& scope : m_scopes)
            if (scope.count(value.token.lexeme)) { warn("shadowing", "Local shadows an outer declaration: " + value.token.lexeme, value.token); break; }
        if (declaration->initializer) expression(*declaration->initializer);
        for (const auto& initializer : declaration->aggregate_initializers) expression(*initializer.value);
        m_locals.emplace(declaration->symbol_id, value.token);
        m_scopes.back().emplace(value.token.lexeme, declaration->symbol_id);
        if (declaration->initializer || !declaration->aggregate_initializers.empty() || declaration->is_constexpr)
            m_initialized.insert(declaration->symbol_id);
    } else if (const auto* conditional = dynamic_cast<const IfStmt*>(&value)) {
        expression(*conditional->condition); const auto incoming = m_initialized;
        statement(*conditional->thenBranch); const auto after_then = m_initialized;
        m_initialized = incoming; if (conditional->elseBranch) statement(*conditional->elseBranch);
        Initialized both;
        std::set_intersection(after_then.begin(), after_then.end(), m_initialized.begin(), m_initialized.end(), std::inserter(both, both.end()));
        if (terminates(*conditional->thenBranch)) both = m_initialized;
        else if (conditional->elseBranch && terminates(*conditional->elseBranch)) both = after_then;
        m_initialized = std::move(both);
    } else if (const auto* loop = dynamic_cast<const ForStmt*>(&value)) {
        m_scopes.emplace_back(); if (loop->initializer) statement(*loop->initializer);
        expression(*loop->condition); const auto incoming = m_initialized; statement(*loop->body);
        // continue may bypass assignments in the body: the increment cannot assume them.
        m_initialized = incoming; if (loop->increment) expression(*loop->increment);
        m_initialized = incoming; m_scopes.pop_back();
    } else if (const auto* loop = dynamic_cast<const WhileStmt*>(&value)) {
        expression(*loop->condition); const auto incoming = m_initialized; statement(*loop->body); m_initialized = incoming;
    } else if (const auto* selection = dynamic_cast<const SwitchStmt*>(&value)) {
        expression(*selection->condition); const auto incoming = m_initialized;
        m_scopes.emplace_back();
        statements(selection->body->statements, true); m_scopes.pop_back(); m_initialized = incoming;
    } else if (const auto* loop = dynamic_cast<const HardwareLoopStmt*>(&value)) { expression(*loop->count); statement(*loop->body); }
    else if (const auto* result = dynamic_cast<const ReturnStmt*>(&value)) { if (result->value) expression(*result->value); }
    else if (const auto* operation = dynamic_cast<const ExpressionStmt*>(&value)) expression(*operation->expression);
    else if (const auto* plot = dynamic_cast<const PlotBlockStmt*>(&value)) statement(*plot->body);
    else if (const auto* color = dynamic_cast<const SetColorStmt*>(&value)) expression(*color->color_value);
    else if (const auto* mode = dynamic_cast<const CmodeStmt*>(&value)) expression(*mode->options_value);
}
std::vector<LanguageWarning> LanguageWarnings::check(const std::vector<std::unique_ptr<Stmt>>& program, const std::set<std::string>& enabled) {
    m_enabled = enabled;
    m_warnings.clear(); m_called.clear();
    for (const auto& value : program) {
        const auto* function = dynamic_cast<const FunctionDeclStmt*>(value.get());
        if (!function || function->is_prototype) continue;
        m_initialized.clear(); m_locals.clear(); m_used.clear(); m_scopes.clear(); m_scopes.emplace_back();
        for (const auto& parameter : function->params) {
            m_scopes.back().emplace(parameter.name.lexeme, parameter.symbol_id);
            m_initialized.insert(parameter.symbol_id);
        }
        statements(function->body);
        for (const auto& local : m_locals) if (!m_used.count(local.first))
            warn("unused-variable", "Unused local '" + local.second.lexeme + "'.", local.second);
    }
    for (const auto& value : program)
        if (const auto* function = dynamic_cast<const FunctionDeclStmt*>(value.get()))
            if (function->linkage == Linkage::Internal && !function->is_prototype && !m_called.count(function->token.lexeme))
                warn("unused-function", "Unused internal function '" + function->token.lexeme + "'.", function->token);
    return m_warnings;
}
