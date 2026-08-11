#include "Optimizer.hpp"
#include <cstdint>
#include <limits>
#include <string>

namespace {

bool expressionUsesName(const Expr& expr, const std::string& name) {
    if (const auto* variable = dynamic_cast<const VariableExpr*>(&expr)) {
        return variable->token.lexeme == name;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        return expressionUsesName(*binary->left, name) ||
               expressionUsesName(*binary->right, name);
    }
    if (const auto* assignment = dynamic_cast<const AssignExpr*>(&expr)) {
        return expressionUsesName(*assignment->name, name) ||
               expressionUsesName(*assignment->value, name);
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        return expressionUsesName(*unary->right, name);
    }
    if (const auto* address = dynamic_cast<const AddressOfExpr*>(&expr)) {
        return expressionUsesName(*address->right, name);
    }
    if (const auto* dereference = dynamic_cast<const DereferenceExpr*>(&expr)) {
        return expressionUsesName(*dereference->right, name);
    }
    if (const auto* subscript = dynamic_cast<const SubscriptExpr*>(&expr)) {
        return expressionUsesName(*subscript->array, name) ||
               expressionUsesName(*subscript->index, name);
    }
    if (const auto* member = dynamic_cast<const MemberAccessExpr*>(&expr)) {
        return expressionUsesName(*member->object, name);
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        if (expressionUsesName(*call->callee, name)) {
            return true;
        }
        for (const auto& argument : call->arguments) {
            if (expressionUsesName(*argument, name)) {
                return true;
            }
        }
        return false;
    }
    if (const auto* cast = dynamic_cast<const CastExpr*>(&expr)) {
        return expressionUsesName(*cast->expression, name);
    }
    return false;
}

bool statementUsesName(const Stmt& stmt, const std::string& name) {
    if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
        for (const auto& child : block->statements) {
            if (statementUsesName(*child, name)) return true;
        }
        return false;
    }
    if (const auto* conditional = dynamic_cast<const IfStmt*>(&stmt)) {
        return expressionUsesName(*conditional->condition, name) ||
               statementUsesName(*conditional->thenBranch, name) ||
               (conditional->elseBranch && statementUsesName(*conditional->elseBranch, name));
    }
    if (const auto* loop = dynamic_cast<const WhileStmt*>(&stmt)) {
        return expressionUsesName(*loop->condition, name) ||
               statementUsesName(*loop->body, name);
    }
    if (const auto* hardware_loop = dynamic_cast<const HardwareLoopStmt*>(&stmt)) {
        return expressionUsesName(*hardware_loop->count, name) ||
               statementUsesName(*hardware_loop->body, name);
    }
    if (const auto* switch_stmt = dynamic_cast<const SwitchStmt*>(&stmt)) {
        return expressionUsesName(*switch_stmt->condition, name) ||
               statementUsesName(*switch_stmt->body, name);
    }
    if (const auto* case_stmt = dynamic_cast<const CaseStmt*>(&stmt)) {
        return expressionUsesName(*case_stmt->value, name);
    }
    if (const auto* return_stmt = dynamic_cast<const ReturnStmt*>(&stmt)) {
        return return_stmt->value && expressionUsesName(*return_stmt->value, name);
    }
    if (const auto* declaration = dynamic_cast<const VarDeclStmt*>(&stmt)) {
        return declaration->initializer && expressionUsesName(*declaration->initializer, name);
    }
    if (const auto* expression = dynamic_cast<const ExpressionStmt*>(&stmt)) {
        return expressionUsesName(*expression->expression, name);
    }
    if (const auto* plot = dynamic_cast<const PlotStmt*>(&stmt)) {
        return expressionUsesName(*plot->x, name) || expressionUsesName(*plot->y, name);
    }
    if (const auto* color = dynamic_cast<const SetColorStmt*>(&stmt)) {
        return expressionUsesName(*color->color_value, name);
    }
    if (const auto* mode = dynamic_cast<const CmodeStmt*>(&stmt)) {
        return expressionUsesName(*mode->options_value, name);
    }
    return false;
}

bool containsBreak(const Stmt& stmt) {
    if (dynamic_cast<const BreakStmt*>(&stmt)) return true;
    if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
        for (const auto& child : block->statements) if (containsBreak(*child)) return true;
    } else if (const auto* conditional = dynamic_cast<const IfStmt*>(&stmt)) {
        return containsBreak(*conditional->thenBranch) ||
               (conditional->elseBranch && containsBreak(*conditional->elseBranch));
    } else if (const auto* loop = dynamic_cast<const WhileStmt*>(&stmt)) {
        return containsBreak(*loop->body);
    } else if (const auto* switch_stmt = dynamic_cast<const SwitchStmt*>(&stmt)) {
        return containsBreak(*switch_stmt->body);
    }
    return false;
}

bool parseIntegerLiteral(const LiteralExpr& literal, long& value) {
    try {
        std::size_t parsed = 0;
        value = std::stol(literal.token.lexeme, &parsed, 0);
        return parsed == literal.token.lexeme.size();
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

// The main entry point for the optimization pass.
void Optimizer::optimize(std::vector<std::unique_ptr<Stmt>>& program) {
    for (auto& stmt : program) {
        stmt->accept(*this);
    }
}

// Visit a function and optimize its body.
void Optimizer::visit(FunctionDeclStmt& stmt) {
    auto bodyBlock = std::make_unique<BlockStmt>(std::move(stmt.body));
    bodyBlock->accept(*this);
    stmt.body = std::move(bodyBlock->statements);
}

// Visit an if statement and optimize its branches.
void Optimizer::visit(IfStmt& stmt) {
    stmt.thenBranch->accept(*this);
    if (stmt.elseBranch) {
        stmt.elseBranch->accept(*this);
    }
}

// Visit a while loop and optimize its body.
void Optimizer::visit(WhileStmt& stmt) {
    stmt.body->accept(*this);
}

// The core of the optimization logic lives here.
void Optimizer::visit(BlockStmt& block) {
    std::vector<std::unique_ptr<Stmt>> optimized_statements;
    for (size_t i = 0; i < block.statements.size(); ++i) {
        auto& current_stmt = block.statements[i];

        // PATTERN MATCHING LOGIC
        // A `for` loop is desugared into a BlockStmt containing an
        // initializer statement and a WhileStmt.
        auto* as_block = dynamic_cast<BlockStmt*>(current_stmt.get());
        if (!as_block || as_block->statements.size() != 2) {
            // It's not a desugared for-loop, or not the right shape.
            // Recursively optimize it and add to our new list.
            current_stmt->accept(*this);
            optimized_statements.push_back(std::move(current_stmt));
            continue;
        }
        
        // 1. Check for Initializer: `word i = N;`
        std::string loop_var_name;
        LiteralExpr* init_val = nullptr;

        if (auto* decl_init = dynamic_cast<VarDeclStmt*>(as_block->statements[0].get())) {
            // Case 1: for (word i = N; ...)
            if (!decl_init->initializer) continue; // Not an optimizable pattern
            init_val = dynamic_cast<LiteralExpr*>(decl_init->initializer.get()); 
            loop_var_name = decl_init->token.lexeme;
        } else if (auto* expr_init_stmt = dynamic_cast<ExpressionStmt*>(as_block->statements[0].get())) {
            // Case 2: i = N;
            auto* assign_init = dynamic_cast<AssignExpr*>(expr_init_stmt->expression.get());
            if (!assign_init) continue;
            auto* assign_target = dynamic_cast<VariableExpr*>(assign_init->name.get());
            if (!assign_target) continue;
            init_val = dynamic_cast<LiteralExpr*>(assign_init->value.get());
            loop_var_name = assign_target->token.lexeme;
        } else {
            // Initializer is neither a declaration nor an expression, so it can't match.
            continue; // This should not be reachable with a valid for-loop
        }

        long init_count = 0;
        if (!init_val || !parseIntegerLiteral(*init_val, init_count) ||
            init_count < 1 || init_count > std::numeric_limits<std::uint16_t>::max()) {
            current_stmt->accept(*this);
            optimized_statements.push_back(std::move(current_stmt));
            continue;
        }

        // 2. Check for Loop: `while (i > 0) { ...; i = i - 1; }`
        auto* loop = dynamic_cast<WhileStmt*>(as_block->statements[1].get());
        if (!loop) {
            current_stmt->accept(*this);
            optimized_statements.push_back(std::move(current_stmt));
            continue;
        }

        // 3. Check Condition: `i > 0`
        auto* condition = dynamic_cast<BinaryExpr*>(loop->condition.get());
        auto* cond_left = condition ? dynamic_cast<VariableExpr*>(condition->left.get()) : nullptr;
        auto* cond_right = condition ? dynamic_cast<LiteralExpr*>(condition->right.get()) : nullptr;
        long condition_value = 0;
        if (!condition || !cond_left || !cond_right ||
            !parseIntegerLiteral(*cond_right, condition_value) ||
            cond_left->token.lexeme != loop_var_name ||
            condition->token.type != TokenType::GREATER || condition_value != 0) {
            current_stmt->accept(*this);
            optimized_statements.push_back(std::move(current_stmt));
            continue;
        }

        // 4. Check Increment: `i = i - 1`
        auto* loop_body_block = dynamic_cast<BlockStmt*>(loop->body.get());
        if (!loop_body_block || loop_body_block->statements.empty()) {
            current_stmt->accept(*this);
            optimized_statements.push_back(std::move(current_stmt));
            continue;
        }
        auto* increment_stmt = dynamic_cast<ExpressionStmt*>(loop_body_block->statements.back().get());
        auto* assign_expr = increment_stmt ? dynamic_cast<AssignExpr*>(increment_stmt->expression.get()) : nullptr;
        auto* assign_target = assign_expr ? dynamic_cast<VariableExpr*>(assign_expr->name.get()) : nullptr;
        auto* assign_val = assign_expr ? dynamic_cast<BinaryExpr*>(assign_expr->value.get()) : nullptr;
        auto* sub_left = assign_val ? dynamic_cast<VariableExpr*>(assign_val->left.get()) : nullptr;
        auto* sub_right = assign_val ? dynamic_cast<LiteralExpr*>(assign_val->right.get()) : nullptr;

        long decrement_value = 0;
        if (!assign_target || assign_target->token.lexeme != loop_var_name ||
            !assign_val || assign_val->token.type != TokenType::MINUS ||
            !sub_left || sub_left->token.lexeme != loop_var_name ||
            !sub_right || !parseIntegerLiteral(*sub_right, decrement_value) || decrement_value != 1) {
            current_stmt->accept(*this);
            optimized_statements.push_back(std::move(current_stmt));
            continue;
        }

        // The transformation removes both the initializer's assignment and
        // the decrement.  It is only valid when the induction variable is
        // completely unobservable and no control-flow edge escapes the
        // hardware loop.  This conservative check intentionally rejects
        // aliases, calls, nested uses, and post-loop observations.
        bool variable_used_in_body = false;
        for (std::size_t body_index = 0; body_index + 1 < loop_body_block->statements.size(); ++body_index) {
            if (statementUsesName(*loop_body_block->statements[body_index], loop_var_name)) {
                variable_used_in_body = true;
                break;
            }
        }
        bool variable_used_after_loop = false;
        for (std::size_t following = i + 1; following < block.statements.size(); ++following) {
            if (statementUsesName(*block.statements[following], loop_var_name)) {
                variable_used_after_loop = true;
                break;
            }
        }
        if (variable_used_in_body || variable_used_after_loop || containsBreak(*loop->body)) {
            current_stmt->accept(*this);
            optimized_statements.push_back(std::move(current_stmt));
            continue;
        }

        // SUCCESS! We have a match!
        // Remove the `i = i - 1` statement from the body.
        loop_body_block->statements.pop_back();

        // Create the new HardwareLoopStmt node.
        auto count_literal = std::make_unique<LiteralExpr>(init_val->token);
        auto new_loop = std::make_unique<HardwareLoopStmt>(std::move(count_literal), std::move(loop->body));
        
        // Recursively optimize the new body, just in case of nested loops.
        new_loop->body->accept(*this);
        
        optimized_statements.push_back(std::move(new_loop));
    }
    // Replace the old statements with the new, possibly optimized list.
    block.statements = std::move(optimized_statements);
}
