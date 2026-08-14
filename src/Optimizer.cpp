#include "Optimizer.hpp"
#include <cstdint>
#include <limits>
#include <string>

namespace {

bool expressionUsesSymbol(const Expr& expr, SymbolId id) {
    if (const auto* variable = dynamic_cast<const VariableExpr*>(&expr)) {
        return variable->symbol_id == id;
    }
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
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        if (expressionUsesSymbol(*call->callee, id)) {
            return true;
        }
        for (const auto& argument : call->arguments) {
            if (expressionUsesSymbol(*argument, id)) {
                return true;
            }
        }
        return false;
    }
    if (const auto* cast = dynamic_cast<const CastExpr*>(&expr)) {
        return expressionUsesSymbol(*cast->expression, id);
    }
    return false;
}

bool statementUsesSymbol(const Stmt& stmt, SymbolId id) {
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
        return declaration->initializer && expressionUsesSymbol(*declaration->initializer, id);
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

bool containsReturn(const Stmt& stmt) {
    if (dynamic_cast<const ReturnStmt*>(&stmt)) return true;
    if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
        for (const auto& child : block->statements) if (containsReturn(*child)) return true;
    } else if (const auto* conditional = dynamic_cast<const IfStmt*>(&stmt)) {
        return containsReturn(*conditional->thenBranch) ||
               (conditional->elseBranch && containsReturn(*conditional->elseBranch));
    } else if (const auto* loop = dynamic_cast<const WhileStmt*>(&stmt)) {
        return containsReturn(*loop->body);
    } else if (const auto* switch_stmt = dynamic_cast<const SwitchStmt*>(&stmt)) {
        return containsReturn(*switch_stmt->body);
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
        SymbolId loop_var_id;
        LiteralExpr* init_val = nullptr;

        if (auto* decl_init = dynamic_cast<VarDeclStmt*>(as_block->statements[0].get())) {
            // Case 1: for (word i = N; ...)
            if (!decl_init->initializer) {
                current_stmt->accept(*this);
                optimized_statements.push_back(std::move(current_stmt));
                continue;
            }
            init_val = dynamic_cast<LiteralExpr*>(decl_init->initializer.get()); 
            loop_var_id = decl_init->symbol_id;
        } else if (auto* expr_init_stmt = dynamic_cast<ExpressionStmt*>(as_block->statements[0].get())) {
            // Assignment-based loops cannot be proven safe without tracking
            // uses outside this block, so leave them as ordinary loops.
            (void)expr_init_stmt;
            current_stmt->accept(*this);
            optimized_statements.push_back(std::move(current_stmt));
            continue;
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
            cond_left->symbol_id != loop_var_id ||
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
        if (!assign_target || assign_target->symbol_id != loop_var_id ||
            !assign_val || assign_val->token.type != TokenType::MINUS ||
            !sub_left || sub_left->symbol_id != loop_var_id ||
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
            if (statementUsesSymbol(*loop_body_block->statements[body_index], loop_var_id)) {
                variable_used_in_body = true;
                break;
            }
        }
        bool variable_used_after_loop = false;
        for (std::size_t following = i + 1; following < block.statements.size(); ++following) {
            if (statementUsesSymbol(*block.statements[following], loop_var_id)) {
                variable_used_after_loop = true;
                break;
            }
        }
        if (variable_used_in_body || variable_used_after_loop ||
            containsBreak(*loop->body) || containsReturn(*loop->body)) {
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
