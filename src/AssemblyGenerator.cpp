#include "AssemblyGenerator.hpp"
#include <stdexcept>
#include "CompilerError.hpp"

namespace {

bool statementAlwaysTerminates(const Stmt& statement) {
    if (dynamic_cast<const ReturnStmt*>(&statement)) {
        return true;
    }
    if (const auto* block = dynamic_cast<const BlockStmt*>(&statement)) {
        return !block->statements.empty() &&
               statementAlwaysTerminates(*block->statements.back());
    }
    if (const auto* conditional = dynamic_cast<const IfStmt*>(&statement)) {
        return conditional->elseBranch &&
               statementAlwaysTerminates(*conditional->thenBranch) &&
               statementAlwaysTerminates(*conditional->elseBranch);
    }
    return false;
}

} // namespace

AssemblyGenerator::AssemblyGenerator(
    const std::map<std::string, FunctionSymbol>& global_function_symbols,
    const std::map<std::string, Analyzer::LocalSymbolTable>& all_local_symbols,
    const DataSegmentManager& data_manager
) : m_global_function_symbols(global_function_symbols),
    m_all_local_symbols(all_local_symbols),
    m_data_manager(data_manager) {}

std::string AssemblyGenerator::generate(const std::vector<std::unique_ptr<Stmt>>& program) {
    emit(".setcpu \"GSU\"");
    emit(".include \"casfx.inc\"");
    emit("");

    // Data Segment
    emit(".segment \"DATA\"");
    for (const auto& pair : m_data_manager.getEntries()) {
        const auto& entry = pair.second;
        emit(".export " + entry.label, "Export for linker visibility");
        emit(entry.label + ":");
        m_indent_level++;
        std::string directive = (entry.type.base == BaseType::BYTE) ? ".byte" : ".word";
        std::stringstream line;
        line << directive << " ";
        for (size_t i = 0; i < entry.bytes.size(); ) {
            if (entry.type.base == BaseType::BYTE) {
                line << std::to_string(entry.bytes[i]);
                i++;
            } else {
                uint16_t val = entry.bytes[i] | (entry.bytes[i+1] << 8);
                line << std::to_string(val);
                i += 2;
            }
            if (i < entry.bytes.size()) line << ", ";
        }
        emit(line.str());
        m_indent_level--;
    }
    emit("");

    // Code Segment
    emit(".segment \"CODE\"");
    for(const auto& stmt : program) {
        if (dynamic_cast<FunctionDeclStmt*>(stmt.get()) || dynamic_cast<StructDefStmt*>(stmt.get())) {
            stmt->accept(*this);
        }
    }

    return m_out.str();
}

void AssemblyGenerator::emit(const std::string& s, const std::string& comment) {
    if (!s.empty() && s.back() != ':') {
        indent();
    }
    m_out << s;
    if (!comment.empty()) {
        // Pad to a consistent column for comments
        int padding = 40 - static_cast<int>(s.length()) - (m_indent_level * 4);
        if (padding < 1) padding = 1;
        m_out << std::string(padding, ' ') << "; " << comment;
    }
    m_out << "\n";
}

void AssemblyGenerator::indent() {
    for (int i = 0; i < m_indent_level; ++i) {
        m_out << "    ";
    }
}

std::string AssemblyGenerator::newLabel() {
    return "_L" + std::to_string(m_label_counter++);
}

void AssemblyGenerator::analyzeAndSetLocals(FunctionDeclStmt& stmt) {
    const auto function = m_all_local_symbols.find(stmt.token.lexeme);
    if (function == m_all_local_symbols.end()) {
        throw CompilerError("ASM GEN: missing analyzed symbols for function '" +
                            stmt.token.lexeme + "'.",
                            stmt.token.line_number, stmt.token.col_number);
    }
    m_symbolTable = function->second;
    if (stmt.total_local_alloc_size > 0) {
        emit("sub sp, sp, #" + std::to_string(stmt.total_local_alloc_size),
             "Allocate space for all local variables");
    }
}

void AssemblyGenerator::visit(FunctionDeclStmt& stmt) {
    if (stmt.is_prototype) return;

    emit("");
    emit(".export " + stmt.token.lexeme, "Export for linker visibility");
    emit(stmt.token.lexeme + ":");
    m_indent_level++;
    
    m_currentFunctionName = stmt.token.lexeme;
	m_functionHasExplicitReturn = false;
    
    // FUNCTION PROLOGUE
    emit("push r11", "Save caller's link register");
    emit("push r9", "Save caller's frame pointer");
    emit("move r9, sp", "Set new frame pointer");

    // This function will now allocate space and set up symbol offsets
    analyzeAndSetLocals(stmt);

    for (const auto& s : stmt.body) {
        s->accept(*this);
    }

    // Emit an implicit function tail only if no explicit return occurred.
    if (!m_functionHasExplicitReturn) {
        emit("move sp, r9", "Deallocate local variables");

        if (m_currentFunctionName == "main") {
            // Match the canonical backend: main halts the program.
            emit("stop", "Implicit end of program");
            emit("nop");
        } else {
            emit("pop r9", "Restore caller's frame pointer");
            emit("pop r11", "Restore caller's link register");
            emit("ret", "Implicit return at end of function");
            emit("nop");
        }
    }

    m_indent_level--;
}

void AssemblyGenerator::visit(ReturnStmt& stmt) {
	m_functionHasExplicitReturn = true;
    if (stmt.value) {
        stmt.value->accept(*this, nullptr);
        dereferenceIfNeeded(*stmt.value);
        emit("", "Return value is in R0");
    }

    // FUNCTION EPILOGUE
    emit("move sp, r9", "Deallocate local variables");
    
    if (m_currentFunctionName == "main") {
        emit("stop", "End of program");
    } else {
        emit("pop r9", "Restore caller's frame pointer");
        emit("pop r11", "Restore caller's link register");
        emit("ret", "Return to caller");
        emit("nop");
    }
}

void AssemblyGenerator::visit(VarDeclStmt& stmt) {
    // Allocation is handled in the function prologue. We only handle initialization here.
    if (stmt.initializer) {
        stmt.initializer->accept(*this, &stmt.type);

        std::string scratch_reg = m_isInPlottingContext ? "r3" : "r1";
        emit("move " + scratch_reg + ", r0", "Value to be stored is now in " + scratch_reg);

        auto var_expr = VariableExpr(stmt.token);
        var_expr.result_type = stmt.type;
        var_expr.symbol_id = stmt.symbol_id;
        var_expr.accept(*this, nullptr); // Leaves address of the variable in R0

        if (stmt.type.base == BaseType::BYTE) {
             emit("stb (r0), " + scratch_reg, "Initialize variable");
        } else {
             emit("stw (r0), " + scratch_reg, "Initialize variable");
        }
    }
}

void AssemblyGenerator::visit(CallExpr& expr, const Type*) {
    auto* callee_var = dynamic_cast<VariableExpr*>(expr.callee.get());
    if (!callee_var) throw CompilerError("Dynamic function calls not supported.", expr.callee->token.line_number, expr.callee->token.col_number);

    emit("", "--- Function Call: " + callee_var->token.lexeme + " ---");
    // 1. Push arguments onto the stack (right-to-left)
    for (int i = static_cast<int>(expr.arguments.size()) - 1; i >= 0; --i) {
        expr.arguments[i]->accept(*this, nullptr);
        dereferenceIfNeeded(*expr.arguments[i]);
        emit("push r0", "Push argument " + std::to_string(i));
    }
    
    // 2. The call
    emit("jal " + callee_var->token.lexeme);

    // 3. Caller stack cleanup
    int bytes_to_pop = static_cast<int>(expr.arguments.size()) * 2;
    if (bytes_to_pop > 0) {
        emit("add sp, sp, #" + std::to_string(bytes_to_pop), "Clean up arguments from stack");
    }
    emit("", "--- End Call ---");
}

void AssemblyGenerator::visit(BinaryExpr& expr, const Type*) {
    auto try_optimize_immediate = [&](Expr* non_literal_side, LiteralExpr* literal_side) -> bool {
        const long value = std::stol(literal_side->token.lexeme, nullptr, 0);
        std::string operation;
        switch (expr.token.type) {
            case TokenType::PLUS:  operation = "add"; break;
            case TokenType::MINUS: operation = "sub"; break;
            case TokenType::STAR:  operation = "mult"; break;
            default: return false;
        }
        if (value < 0 || value > 15) return false;

        non_literal_side->accept(*this, nullptr);
        dereferenceIfNeeded(*non_literal_side);
        emit(operation + " r0, r0, #" + std::to_string(value));
        return true;
    };

    if (!dynamic_cast<LiteralExpr*>(expr.left.get()) &&
        dynamic_cast<LiteralExpr*>(expr.right.get()) &&
        try_optimize_immediate(expr.left.get(), dynamic_cast<LiteralExpr*>(expr.right.get()))) {
        return;
    }
    if (dynamic_cast<LiteralExpr*>(expr.left.get()) &&
        !dynamic_cast<LiteralExpr*>(expr.right.get()) &&
        (expr.token.type == TokenType::PLUS || expr.token.type == TokenType::STAR) &&
        try_optimize_immediate(expr.right.get(), dynamic_cast<LiteralExpr*>(expr.left.get()))) {
        return;
    }

    // Standard RPN-style evaluation
    expr.right->accept(*this, nullptr);
    dereferenceIfNeeded(*expr.right);
    emit("push r0", "Save RHS");
    expr.left->accept(*this, nullptr);
    dereferenceIfNeeded(*expr.left);

    // Choose a safe scratch register and pop the RHS into it.
    std::string scratch_reg = m_isInPlottingContext ? "r3" : "r1";
    emit("pop " + scratch_reg, "Load RHS into " + scratch_reg);

    std::string op;
    switch (expr.token.type) {
        case TokenType::PLUS:    op = "add r0, r0, " + scratch_reg; break;
        case TokenType::MINUS:   op = "sub r0, r0, " + scratch_reg; break;
        case TokenType::STAR:    op = "mult r0, r0, " + scratch_reg; break;
        case TokenType::GREATER:
        case TokenType::LESS:
        case TokenType::EQUAL_EQUAL:
        case TokenType::BANG_EQUAL:
             op = "cmp r0, " + scratch_reg; break;
        default: throw CompilerError("Assembly generation for operator '" + expr.token.lexeme + "' not implemented.", expr.token.line_number, expr.token.col_number);
    }
    emit(op);
}

void AssemblyGenerator::visit(IfStmt& stmt) {
    std::string elseLabel = newLabel();
    std::string endLabel = newLabel();

    stmt.condition->accept(*this, nullptr);
    auto* cond = dynamic_cast<BinaryExpr*>(stmt.condition.get());
    if (!cond) throw CompilerError("If condition must be a binary expression for now.", stmt.condition->token.line_number, stmt.condition->token.col_number);
    
    std::string branch_op;
    switch(cond->token.type) {
        case TokenType::GREATER:     branch_op = "bmi"; break;
        case TokenType::LESS:        branch_op = "bpl"; break;
        case TokenType::EQUAL_EQUAL: branch_op = "bne"; break; // Branch if Not Equal
        case TokenType::BANG_EQUAL:  branch_op = "beq"; break; // Branch if Equal
        default: throw CompilerError("Unsupported operator in if condition.", cond->token.line_number, cond->token.col_number);
    }
    
    emit(branch_op + " " + (stmt.elseBranch ? elseLabel : endLabel));
    emit("nop"); // Fill the branch delay slot

    m_indent_level++;
    stmt.thenBranch->accept(*this);
    m_indent_level--;

    if (stmt.elseBranch && !statementAlwaysTerminates(*stmt.thenBranch)) {
        emit("bra " + endLabel);
        emit("nop"); // Fill the branch delay slot
    }
    if (stmt.elseBranch) {
        emit(elseLabel + ":");
        m_indent_level++;
        stmt.elseBranch->accept(*this);
        m_indent_level--;
    }
    
    emit(endLabel + ":");
}

void AssemblyGenerator::visit(VariableExpr& expr, const Type*) {
    if (expr.symbol_id.isValid() && m_symbolTable.count(expr.symbol_id)) {
        const Symbol& symbol = m_symbolTable.at(expr.symbol_id);
        // Generate FP-relative address calculation
        emit("move r0, r9", "Load base address from frame pointer");
        if (symbol.stackOffset != 0) {
            // ca65 macros handle negative immediates, but this is safer, I hope
            if (symbol.stackOffset > 0) {
                emit("add r0, r0, #" + std::to_string(symbol.stackOffset), "Access parameter");
            } else {
                emit("sub r0, r0, #" + std::to_string(std::abs(symbol.stackOffset)), "Access local variable");
            }
        }
        emit("", "Address of '" + expr.token.lexeme + "' is now in r0");

    } else if (m_data_manager.hasSymbol(expr.token.lexeme)) {
        emit("iwt r0, #" + expr.token.lexeme, "Load address of global '" + expr.token.lexeme + "'");
    } else if (m_global_function_symbols.count(expr.token.lexeme)) {
        emit("iwt r0, #" + expr.token.lexeme, "Load address of function '" + expr.token.lexeme + "'");
    } else {
        throw CompilerError("ASM GEN: Undeclared variable '" + expr.token.lexeme + "'.", expr.token.line_number, expr.token.col_number);
    }
}

void AssemblyGenerator::dereferenceIfNeeded(Expr& expr, bool is_for_assignment) {
    if (dynamic_cast<LiteralExpr*>(&expr) || 
        dynamic_cast<BinaryExpr*>(&expr) || 
        dynamic_cast<UnaryExpr*>(&expr) ||
        dynamic_cast<AddressOfExpr*>(&expr) || 
        dynamic_cast<CallExpr*>(&expr) || 
        dynamic_cast<CastExpr*>(&expr)) {
        return;
    }
    if (!is_for_assignment) {
        if (expr.result_type.base == BaseType::BYTE) {
             emit("moveb r0, (r0)", "Dereference byte pointer");
        } else {
             emit("movew r0, (r0)", "Dereference word pointer");
        }
    }
}

void AssemblyGenerator::visit(AssignExpr& expr, const Type*) {
    // 1. Evaluate the address of the left-hand side.
    expr.name->accept(*this, nullptr);
    dereferenceIfNeeded(*expr.name, true); // Get address of L-value into R0.
    emit("push r0", "Save L-value address");

    // 2. Evaluate the value of the right-hand side.
    expr.value->accept(*this, nullptr);
    dereferenceIfNeeded(*expr.value, false); // Get value of R-value into R0.
    
    // Choose a safe scratch register to pop the L-value address into.
    std::string scratch_reg = m_isInPlottingContext ? "r3" : "r1";
    emit("pop " + scratch_reg, "Load L-value address into " + scratch_reg);
    
    if (expr.result_type.base == BaseType::BYTE) {
        emit("moveb (" + scratch_reg + "), r0");
    } else {
        emit("movew (" + scratch_reg + "), r0");
    }
}

// --- Other visitors (simplified for brevity, can be expanded) ---
void AssemblyGenerator::visit(LiteralExpr& expr, const Type*) { emit("iwt r0, #" + expr.token.lexeme); }

void AssemblyGenerator::visit(ExpressionStmt& stmt) {
    stmt.expression->accept(*this, nullptr);
}

void AssemblyGenerator::visit(BlockStmt& stmt) {
    for(const auto& s : stmt.statements) {
        s->accept(*this);
    }
}

void AssemblyGenerator::visit(StructDefStmt&) { /* No assembly for struct def */ }
void AssemblyGenerator::visit(ConstDataStmt&) { /* Handled in main generate loop */ }

// More stubs to be filled in as needed... Seems wasteful but that's how it is, it seems...
void AssemblyGenerator::visit(UnaryExpr& expr, const Type*) {
    if (auto* literal = dynamic_cast<LiteralExpr*>(expr.right.get())) {
        emit("iwt r0, #-" + literal->token.lexeme, "Negated literal");
        return;
    }

    expr.right->accept(*this, nullptr);
    emit("not", "Negate value");
    emit("inc r0");
}

void AssemblyGenerator::visit(AddressOfExpr& expr, const Type*) {
    // Our L-value visitors now correctly compute the address and leave it in R0.
    // So, visiting the operand is all we need to do.
    expr.right->accept(*this, nullptr);
}

void AssemblyGenerator::visit(DereferenceExpr& expr, const Type*) {
    expr.right->accept(*this, nullptr);
    dereferenceIfNeeded(*expr.right, false); // This gets the value of the pointer itself into R0
    
    // Now R0 holds an address. We need to load from that address.
    if (expr.result_type.base == BaseType::BYTE) {
        emit("moveb r0, (r0)", "Dereference byte pointer");
    }
    else {
        emit("movew r0, (r0)", "Dereference word pointer");
    }
}

void AssemblyGenerator::visit(SubscriptExpr&, const Type*) { emit("; SubscriptExpr not fully implemented in ASM gen"); }
void AssemblyGenerator::visit(MemberAccessExpr&, const Type*) { emit("; MemberAccessExpr not fully implemented in ASM gen"); }
void AssemblyGenerator::visit(WhileStmt& stmt) {
    std::string startLabel = newLabel();
    std::string endLabel   = newLabel();

    emit(startLabel + ":", "while begin");

    // Evaluate condition
    stmt.condition->accept(*this, nullptr);

    // For now, mirror IfStmt constraints: condition must be a BinaryExpr.
    auto* cond = dynamic_cast<BinaryExpr*>(stmt.condition.get());
    if (!cond) {
        throw CompilerError(
            "While condition must be a binary expression for now.",
            stmt.condition->token.line_number,
            stmt.condition->token.col_number
        );
    }

    // Condition uses: cmp r0, <scratch>
    // Branch on NOT(condition) to exit loop.
    std::string branch_op;
    switch (cond->token.type) {
        case TokenType::GREATER:     branch_op = "bmi"; break;
        case TokenType::LESS:        branch_op = "bpl"; break;
        case TokenType::EQUAL_EQUAL: branch_op = "bne"; break; // not (==)
        case TokenType::BANG_EQUAL:  branch_op = "beq"; break; // not (!=)
        default:
            throw CompilerError(
                "Unsupported operator in while condition.",
                cond->token.line_number,
                cond->token.col_number
            );
    }

    emit(branch_op + " " + endLabel);
    emit("nop"); // fill branch delay slot

    // Body
    m_indent_level++;
    stmt.body->accept(*this);
    m_indent_level--;

    // Loop back
    emit("bra " + startLabel);
    emit("nop"); // fill branch delay slot

    emit(endLabel + ":", "while end");
}

void AssemblyGenerator::visit(SetColorStmt& stmt) {
    // Evaluate color value into R0
    stmt.color_value->accept(*this, nullptr);
    dereferenceIfNeeded(*stmt.color_value);
    emit("color");
}

void AssemblyGenerator::visit(PlotStmt& stmt) {
    // Backend convention: evaluate Y -> R0, move to R2; evaluate X -> R0, move to R1; then PLOT.
    stmt.y->accept(*this, nullptr);
    dereferenceIfNeeded(*stmt.y);
    emit("move r2, r0", "R2 = Plot Y");

    stmt.x->accept(*this, nullptr);
    dereferenceIfNeeded(*stmt.x);
    emit("move r1, r0", "R1 = Plot X");

    emit("plot");
}

// Implement PlotBegin/End to toggle the context flag.
void AssemblyGenerator::visit(PlotBeginStmt&) {
    m_isInPlottingContext = true;
    emit("; --- Plotting context BEGIN ---");
}

void AssemblyGenerator::visit(PlotEndStmt&) {
    m_isInPlottingContext = false;
    emit("; --- Plotting context END ---");
}

void AssemblyGenerator::visit(CmodeStmt&) {
    emit("; CmodeStmt not fully implemented in ASM gen");
}

void AssemblyGenerator::visit(RpixStmt&) { emit("; RpixStmt not fully implemented in ASM gen"); }
void AssemblyGenerator::visit(HardwareLoopStmt& stmt) {
    // Mirrors the canonical backend's hardware-loop setup:
    //   IWT R12, #count
    //   WITH R13
    //   TO R15
    //   <body>
    //   LOOP
    //   NOP

    const std::string& countLexeme = stmt.count->token.lexeme;

    emit("iwt r12, #" + countLexeme, "Hardware loop count");
    emit("with r13");
    emit("to r15", "MOVE R15, R13");

    m_indent_level++;
    stmt.body->accept(*this);
    m_indent_level--;

    emit("loop");
    emit("nop");
}
void AssemblyGenerator::visit(SwitchStmt& stmt) {
    std::string end_label = newLabel();
    m_break_labels.push_back(end_label);

    DefaultStmt* default_stmt = nullptr;
    std::vector<CaseStmt*> case_stmts;

    // Pass 1: Find all case/default statements and assign them unique labels
    for (const auto& s : stmt.body->statements) {
        if (auto* cs = dynamic_cast<CaseStmt*>(s.get())) {
            cs->label_name = newLabel();
            case_stmts.push_back(cs);
        } else if (auto* ds = dynamic_cast<DefaultStmt*>(s.get())) {
            ds->label_name = newLabel();
            default_stmt = ds;
        }
    }
    std::string default_target_label = default_stmt ? default_stmt->label_name : end_label;

    // Generate the jump logic
    emit("", "--- Switch Statement ---");
    // 1. Evaluate the switch condition, result is in R0.
    stmt.condition->accept(*this, nullptr);
    dereferenceIfNeeded(*stmt.condition);
    
    // Choose a safe scratch register for comparison.
    std::string scratch_reg = m_isInPlottingContext ? "r3" : "r1";
    
    for (const auto* cs : case_stmts) {
        emit("push r0", "Save switch condition value");
        cs->value->accept(*this, nullptr); // Literal value is now in R0
        emit("pop " + scratch_reg, "Restore switch condition value to " + scratch_reg);
        emit("cmp " + scratch_reg + ", r0", "Compare condition with case value");
        emit("beq " + cs->label_name);
        emit("nop");
    }
    
    emit("bra " + default_target_label);
    emit("nop");

    emit("", "--- Switch Body ---");
    m_indent_level++;
    for (const auto& s : stmt.body->statements) {
        if (auto* cs = dynamic_cast<CaseStmt*>(s.get())) {
            emit(cs->label_name + ":", "case " + cs->value->token.lexeme);
        } else if (auto* ds = dynamic_cast<DefaultStmt*>(s.get())) {
            emit(ds->label_name + ":", "default");
        } else {
            s->accept(*this);
        }
    }
    m_indent_level--;

    emit(end_label + ":", "End of switch");
    m_break_labels.pop_back();
}

void AssemblyGenerator::visit(CaseStmt&) {
    // This visitor does nothing on its own.
    // The logic is handled entirely by visit(SwitchStmt).
}

void AssemblyGenerator::visit(DefaultStmt&) {
    // This visitor does nothing on its own.
    // The logic is handled entirely by visit(SwitchStmt).
}

void AssemblyGenerator::visit(BreakStmt&) {
    if (m_break_labels.empty()) {
        throw std::runtime_error("Internal codegen error: break label stack is empty.");
    }
    emit("bra " + m_break_labels.back(), "break;");
    emit("nop"); // Fill the branch delay slot
}

void AssemblyGenerator::visit(CastExpr& expr, const Type*) {
    // First, evaluate the expression being cast. Its value ends up in R0.
    expr.expression->accept(*this, nullptr);
    dereferenceIfNeeded(*expr.expression);

    const Type& sourceType = expr.expression->result_type;
    const Type& targetType = expr.cast_to_type;

    // This condition is now explicit and works because the Analyzer has done its job.
    if (targetType.sizeInBytes == 2 && sourceType.sizeInBytes == 1) {
        if (sourceType.is_unsigned) {
            // Do nothing. The 'moveb' pseudo-op (which becomes LDB) already
            // zero-extends the value into the destination register. The 'lob'
            // instruction would be redundant.
        } else {
            emit("sex", "Cast to word (sign-extend)");
        }
    }
    // For narrowing or same-size casts, we emit no assembly instruction.
}
