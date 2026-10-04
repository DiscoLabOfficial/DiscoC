#include "Optimizer.hpp"
#include "IntegerLiteral.hpp"
#include <cstdint>
#include <limits>
#include <string>

namespace {

bool expressionUsesSymbol(const Expr& expr, SymbolId id) {
    if (const auto* variable = dynamic_cast<const VariableExpr*>(&expr)) {
        return variable->symbol_id == id;
    }
    if (const auto* update = dynamic_cast<const UpdateExpr*>(&expr))
        return expressionUsesSymbol(*update->target, id) || expressionUsesSymbol(*update->value, id);
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        return expressionUsesSymbol(*binary->left, id) ||
               expressionUsesSymbol(*binary->right, id);
    }
    if (const auto* assignment = dynamic_cast<const AssignExpr*>(&expr)) {
        return expressionUsesSymbol(*assignment->name, id) ||
               expressionUsesSymbol(*assignment->value, id);
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        return expressionUsesSymbol(*unary->right, id);
    }
    if (const auto* address = dynamic_cast<const AddressOfExpr*>(&expr)) {
        return expressionUsesSymbol(*address->right, id);
    }
    if (const auto* dereference = dynamic_cast<const DereferenceExpr*>(&expr)) {
        return expressionUsesSymbol(*dereference->right, id);
    }
    if (const auto* subscript = dynamic_cast<const SubscriptExpr*>(&expr)) {
        return expressionUsesSymbol(*subscript->array, id) ||
               expressionUsesSymbol(*subscript->index, id);
    }
    if (const auto* member = dynamic_cast<const MemberAccessExpr*>(&expr)) {
        return expressionUsesSymbol(*member->object, id);
    }
    if (dynamic_cast<const CallExpr*>(&expr)) {
        // Calls may alias locals and overwrite the hardware-loop registers.
        return true;
    }
    if (const auto* cast = dynamic_cast<const CastExpr*>(&expr)) {
        return expressionUsesSymbol(*cast->expression, id);
    }
    return false;
}

bool statementUsesSymbol(const Stmt& stmt, SymbolId id) {
    if (const auto* plot = dynamic_cast<const PlotBlockStmt*>(&stmt))
        return statementUsesSymbol(*plot->body, id);
    if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
        for (const auto& child : block->statements) {
            if (statementUsesSymbol(*child, id)) return true;
        }
        return false;
    }
    if (const auto* conditional = dynamic_cast<const IfStmt*>(&stmt)) {
        return expressionUsesSymbol(*conditional->condition, id) ||
               statementUsesSymbol(*conditional->thenBranch, id) ||
               (conditional->elseBranch && statementUsesSymbol(*conditional->elseBranch, id));
    }
    if (const auto* loop = dynamic_cast<const ForStmt*>(&stmt)) {
        return (loop->initializer && statementUsesSymbol(*loop->initializer, id)) ||
            expressionUsesSymbol(*loop->condition, id) || (loop->increment && expressionUsesSymbol(*loop->increment, id)) || statementUsesSymbol(*loop->body, id);
    }
    if (const auto* loop = dynamic_cast<const WhileStmt*>(&stmt)) {
        return expressionUsesSymbol(*loop->condition, id) ||
               statementUsesSymbol(*loop->body, id);
    }
    if (const auto* hardware_loop = dynamic_cast<const HardwareLoopStmt*>(&stmt)) {
        return expressionUsesSymbol(*hardware_loop->count, id) ||
               statementUsesSymbol(*hardware_loop->body, id);
    }
    if (const auto* switch_stmt = dynamic_cast<const SwitchStmt*>(&stmt)) {
        return expressionUsesSymbol(*switch_stmt->condition, id) ||
               statementUsesSymbol(*switch_stmt->body, id);
    }
    if (const auto* case_stmt = dynamic_cast<const CaseStmt*>(&stmt)) {
        return expressionUsesSymbol(*case_stmt->value, id);
    }
    if (const auto* return_stmt = dynamic_cast<const ReturnStmt*>(&stmt)) {
        return return_stmt->value && expressionUsesSymbol(*return_stmt->value, id);
    }
    if (const auto* declaration = dynamic_cast<const VarDeclStmt*>(&stmt)) {
        if (declaration->initializer && expressionUsesSymbol(*declaration->initializer, id)) return true;
        for (const auto& value : declaration->aggregate_initializers) if (expressionUsesSymbol(*value.value, id)) return true;
        return false;
    }
    if (const auto* expression = dynamic_cast<const ExpressionStmt*>(&stmt)) {
        return expressionUsesSymbol(*expression->expression, id);
    }
    if (const auto* plot = dynamic_cast<const PlotStmt*>(&stmt)) {
        return expressionUsesSymbol(*plot->x, id) || expressionUsesSymbol(*plot->y, id);
    }
    if (const auto* color = dynamic_cast<const SetColorStmt*>(&stmt)) {
        return expressionUsesSymbol(*color->color_value, id);
    }
    if (const auto* mode = dynamic_cast<const CmodeStmt*>(&stmt)) {
        return expressionUsesSymbol(*mode->options_value, id);
    }
    return false;
}

bool containsBreak(const Stmt& stmt) {
    if (const auto* plot = dynamic_cast<const PlotBlockStmt*>(&stmt)) return containsBreak(*plot->body);
    if (dynamic_cast<const BreakStmt*>(&stmt) || dynamic_cast<const ContinueStmt*>(&stmt)) return true;
    if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
        for (const auto& child : block->statements) if (containsBreak(*child)) return true;
    } else if (const auto* conditional = dynamic_cast<const IfStmt*>(&stmt)) {
        return containsBreak(*conditional->thenBranch) ||
               (conditional->elseBranch && containsBreak(*conditional->elseBranch));
    } else if (dynamic_cast<const WhileStmt*>(&stmt) || dynamic_cast<const HardwareLoopStmt*>(&stmt)) {
        return true;
    } else if (dynamic_cast<const ForStmt*>(&stmt)) {
        return true;
    } else if (const auto* switch_stmt = dynamic_cast<const SwitchStmt*>(&stmt)) {
        return containsBreak(*switch_stmt->body);
    }
    return false;
}

bool containsReturn(const Stmt& stmt) {
    if (const auto* plot = dynamic_cast<const PlotBlockStmt*>(&stmt)) return containsReturn(*plot->body);
    if (dynamic_cast<const ReturnStmt*>(&stmt)) return true;
    if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
        for (const auto& child : block->statements) if (containsReturn(*child)) return true;
    } else if (const auto* conditional = dynamic_cast<const IfStmt*>(&stmt)) {
        return containsReturn(*conditional->thenBranch) ||
               (conditional->elseBranch && containsReturn(*conditional->elseBranch));
    } else if (const auto* loop = dynamic_cast<const WhileStmt*>(&stmt)) {
        return containsReturn(*loop->body);
    } else if (const auto* loop = dynamic_cast<const ForStmt*>(&stmt)) {
        return containsReturn(*loop->body);
    } else if (const auto* switch_stmt = dynamic_cast<const SwitchStmt*>(&stmt)) {
        return containsReturn(*switch_stmt->body);
    }
    return false;
}

bool parseIntegerLiteral(const LiteralExpr& literal, long& value) {
    try {
        std::size_t parsed = 0;
        value = DiscoNumeric::parse(literal.token.lexeme, &parsed, 0);
        return parsed == literal.token.lexeme.size();
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

void Optimizer::optimize(std::vector<std::unique_ptr<Stmt>>& program) {
    for (auto& statement : program) statement = optimize(std::move(statement));
}

std::unique_ptr<Stmt> Optimizer::optimize(std::unique_ptr<Stmt> statement) {
    auto* loop = dynamic_cast<ForStmt*>(statement.get());
    if (loop && supportsCapability(m_target, TargetCapability::HardwareLoops) && !loop->is_cached) {
        const auto* declaration = dynamic_cast<const VarDeclStmt*>(loop->initializer.get());
        const auto* count = declaration ? dynamic_cast<const LiteralExpr*>(declaration->initializer.get()) : nullptr;
        const auto* condition = dynamic_cast<const BinaryExpr*>(loop->condition.get());
        const auto* variable = condition ? dynamic_cast<const VariableExpr*>(condition->left.get()) : nullptr;
        const auto* zero = condition ? dynamic_cast<const LiteralExpr*>(condition->right.get()) : nullptr;
        const auto* assignment = dynamic_cast<const AssignExpr*>(loop->increment.get());
        const auto* target = assignment ? dynamic_cast<const VariableExpr*>(assignment->name.get()) : nullptr;
        const auto* decrement = assignment ? dynamic_cast<const BinaryExpr*>(assignment->value.get()) : nullptr;
        const auto* left = decrement ? dynamic_cast<const VariableExpr*>(decrement->left.get()) : nullptr;
        const auto* one = decrement ? dynamic_cast<const LiteralExpr*>(decrement->right.get()) : nullptr;
        long n = 0, z = 0, d = 0;
        // Removing the induction variable is legal only when it is unobservable.
        // In particular continue/break/return must not escape the hardware body.
        if (declaration && !declaration->type.is_volatile && count && parseIntegerLiteral(*count, n) &&
            n > 0 && n <= 65535 && variable && variable->symbol_id == declaration->symbol_id &&
            condition->token.type == TokenType::GREATER && zero && parseIntegerLiteral(*zero, z) && z == 0 &&
            target && target->symbol_id == declaration->symbol_id && decrement && decrement->token.type == TokenType::MINUS &&
            left && left->symbol_id == declaration->symbol_id && one && parseIntegerLiteral(*one, d) && d == 1 &&
            !statementUsesSymbol(*loop->body, declaration->symbol_id) && !containsBreak(*loop->body) && !containsReturn(*loop->body)) {
            auto literal = std::make_unique<LiteralExpr>(count->token);
            literal->result_type = count->result_type;
            auto replacement = std::make_unique<HardwareLoopStmt>(std::move(literal), std::move(loop->body));
            replacement->body = optimize(std::move(replacement->body));
            return replacement;
        }
    }
    statement->accept(*this);
    return statement;
}

void Optimizer::visit(FunctionDeclStmt& statement) { optimize(statement.body); }
void Optimizer::visit(BlockStmt& statement) { optimize(statement.statements); }
void Optimizer::visit(IfStmt& statement) {
    statement.thenBranch = optimize(std::move(statement.thenBranch));
    if (statement.elseBranch) statement.elseBranch = optimize(std::move(statement.elseBranch));
}
void Optimizer::visit(WhileStmt& statement) { statement.body = optimize(std::move(statement.body)); }
void Optimizer::visit(ForStmt& statement) { statement.body = optimize(std::move(statement.body)); }
