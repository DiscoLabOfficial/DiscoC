#include "CodeGenerator.hpp"
#include <stdexcept>
#include <iostream>
#include <cstdint>
#include <algorithm>
#include "Parser.hpp"
#include "CompilerError.hpp"
#include "ObjectFile.hpp"

CodeGenerator::CodeGenerator(
    const std::map<std::string, std::map<std::string, Symbol>>& all_local_symbols,
    const std::map<std::string, FunctionSymbol>& global_function_symbols,
    const DataSegmentManager& data_manager,
    const CompilerConfig& config
):  m_all_local_symbols(all_local_symbols),
    m_global_function_symbols(global_function_symbols),
    m_data_manager(data_manager),
    m_config(config) {}

// The main generation routine, returning an ObjectFile
ObjectFile CodeGenerator::generate(const std::vector<std::unique_ptr<Stmt>>& program) {
    // Start with a fresh object file.
    m_object_file = ObjectFile();
    m_object_file.config = m_config;

    // 1. Generate code for all functions.
    // This will populate the .code_section and add symbols and relocations.
    for (const auto& stmt : program) {
        if (dynamic_cast<FunctionDeclStmt*>(stmt.get())) {
            stmt->accept(*this);
        }
    }

    // 2. Process all rom const data.
    // This populates the .data_section and adds data symbols to the symbol table.
    for (const auto& pair : m_data_manager.getEntries()) {
        const auto& entry = pair.second;
        // The symbol's offset is its starting position in the final data section.
        uint32_t offset_in_data_section = static_cast<uint32_t>(m_object_file.data_section.size());
        m_object_file.symbol_table.push_back({entry.label, SymbolSection::DATA, offset_in_data_section});

        // Append the data itself.
        m_object_file.data_section.insert(m_object_file.data_section.end(), entry.bytes.begin(), entry.bytes.end());
    }

    return m_object_file;
}

// Code to generate code to a temporary buffer and measure its size
std::vector<uint8_t> CodeGenerator::visitAndMeasure(Stmt& stmt) {
    // 1. Save the current state of the main object file and its state.
    ObjectFile original_object_file = m_object_file;
    std::map<std::string, Symbol> original_symbol_table = m_symbolTable;
    int original_stack_offset = m_stackOffset;

    // 2. Create a clean slate for measuring.
    m_object_file = ObjectFile();

    // 3. Visit the statement, which will emit to the new, temporary object file.
    stmt.accept(*this);

    // 4. Get the generated code.
    std::vector<uint8_t> measured_code = m_object_file.code_section;

    // 5. Restore the original state so main generation can continue unaffected.
    m_object_file = original_object_file;
    m_symbolTable = original_symbol_table;
    m_stackOffset = original_stack_offset;

    // 6. Return the measured code.
    return measured_code;
}

// Emit methods now target the object file's code section
void CodeGenerator::emitByte(uint8_t byte) { 
    m_object_file.code_section.push_back(byte);
}
void CodeGenerator::emitWord(uint16_t word) { 
    emitByte(word & 0xFF); 
    emitByte((word >> 8) & 0xFF);
}

void CodeGenerator::patchRelativeBranch(
    std::size_t patch_offset,
    std::size_t target_address,
    const Token& source) {
    const std::int64_t next_instruction = static_cast<std::int64_t>(patch_offset) + 1;
    const std::int64_t distance = static_cast<std::int64_t>(target_address) - next_instruction;
    if (distance < -128 || distance > 127) {
        throw CompilerError(
            "Branch target is out of range for the SuperFX 8-bit relative branch.",
            source.line_number,
            source.col_number);
    }
    m_object_file.code_section.at(patch_offset) =
        static_cast<uint8_t>(static_cast<std::int8_t>(distance));
}

void CodeGenerator::emitLiteral(long value, const Type&) {
    // IWT is the most general, safest AND fastest instruction for any literal word or byte.
    emitByte(static_cast<uint8_t>(OpCode::IWT) | static_cast<uint8_t>(RETURN_VALUE_REGISTER));
    emitWord(static_cast<uint16_t>(value));
}

void CodeGenerator::emitMove(uint8_t destination, uint8_t source) {
    if (source != 0) {
        emitByte(0x20 | source); // WITH source
    }
    emitByte(0x10 | destination); // TO destination
}

void CodeGenerator::emitPush(uint8_t reg) {
    if (reg != 0) {
        emitByte(0x20 | reg); // WITH reg
    }
    emitByte(0x3A); // STW (R10)
    emitByte(0xEA); // DEC R10
    emitByte(0xEA); // DEC R10
}

void CodeGenerator::emitPop(uint8_t reg) {
    emitByte(0xDA); // INC R10
    emitByte(0xDA); // INC R10
    if (reg != 0) {
        emitByte(0x20 | reg); // WITH reg
    }
    emitByte(0x4A); // LDW (R10)
}

void CodeGenerator::emitStore(uint8_t address_reg, uint8_t value_reg, bool byte) {
    if (value_reg != 0) {
        emitByte(0x20 | value_reg); // WITH value_reg
    }
    if (byte) {
        emitByte(static_cast<uint8_t>(OpCode::ALT1));
    }
    emitByte(0x30 | address_reg); // STW/STB (address_reg)
}

void CodeGenerator::emitImmediateArithmetic(uint8_t op_base, long value) {
    if (value < 0) {
        throw CompilerError("CodeGen: immediate arithmetic value cannot be negative.", 0, 0);
    }

    auto remaining = static_cast<unsigned long>(value);
    while (remaining > 0) {
        const auto chunk = static_cast<uint8_t>(std::min<unsigned long>(remaining, 15));
        emitByte(static_cast<uint8_t>(OpCode::ALT2));
        emitByte(op_base | chunk);
        remaining -= chunk;
    }
}

void CodeGenerator::emitAdjustStack(std::size_t bytes, bool add) {
    const uint8_t op_base = add ? 0x50 : 0x60;
    while (bytes > 0) {
        const auto chunk = static_cast<uint8_t>(std::min<std::size_t>(bytes, 15));
        emitByte(0x2A); // WITH R10 (SP)
        emitByte(static_cast<uint8_t>(OpCode::ALT2));
        emitByte(op_base | chunk);
        bytes -= chunk;
    }
}

// visit(CallExpr) now creates relocation entries
void CodeGenerator::visit(CallExpr& expr, const Type*) {
	auto* callee_var = dynamic_cast<VariableExpr*>(expr.callee.get());
	if (!callee_var) throw CompilerError("Dynamic function calls not supported.", expr.callee->token.line_number, expr.callee->token.col_number);
    
    // 1. Push arguments
    for (int i = static_cast<int>(expr.arguments.size()) - 1; i >= 0; --i) {
        expr.arguments[i]->accept(*this, nullptr);
        dereferenceIfNeeded(*expr.arguments[i]);
        emitPush(static_cast<uint8_t>(RETURN_VALUE_REGISTER));
    }

    // 2. Emit 'JAL' pseudo-op with a placeholder
    emitByte(0x94); // LINK #4
    
    // The location to patch is the start of the IWT instruction.
    size_t patch_offset = m_object_file.code_section.size();
    emitByte(static_cast<uint8_t>(OpCode::IWT) | 0x0F); // IWT to R15 (PC)
    emitWord(0); // Relocation placeholder
    
    // Record a relocation entry for the linker.
    m_object_file.relocation_table.push_back({
        callee_var->token.lexeme,   // Target symbol name
        SymbolSection::CODE,        // Section to patch
        static_cast<uint32_t>(patch_offset),     // Offset within the code section
        RelocationType::ADDR16_JAL  // Type of relocation needed
    });
    
    // 3. Caller stack cleanup
    int bytes_to_pop = static_cast<int>(expr.arguments.size()) * 2;
    emitAdjustStack(static_cast<std::size_t>(bytes_to_pop), true);
    // The return value is now in R0, as per the ABI.
}

// visit(FunctionDeclStmt) now defines a symbol in the object file
void CodeGenerator::visit(FunctionDeclStmt& stmt) {
    m_symbolTable = m_all_local_symbols.at(stmt.token.lexeme);
    
    uint32_t function_offset = static_cast<uint32_t>(m_object_file.code_section.size());
    m_object_file.symbol_table.push_back({stmt.token.lexeme, SymbolSection::CODE, function_offset});

    m_currentFunctionName = stmt.token.lexeme;

    // FUNCTION PROLOGUE
    // PUSH R11 (link register, assuming JAL was used)
    emitPush(11);
    
    // PUSH R9 (caller's frame pointer)
    emitPush(9);

    // MOVE R9, R10 (new frame pointer = current stack pointer)
    emitMove(9, 10);

    // Allocate space for all local variables
    if (stmt.total_local_alloc_size > 0) {
        emitAdjustStack(static_cast<std::size_t>(stmt.total_local_alloc_size), false);
    }

    // Cache size check
    if (stmt.is_cached && m_config.warn_on_cache_overflow) {
        // We need to wrap the body in a block to visit it.
        auto body_block = BlockStmt(std::move(stmt.body));
        std::vector<uint8_t> measured_code = visitAndMeasure(body_block);
        // Restore the body since it was moved.
        stmt.body = std::move(body_block.statements);

        if (measured_code.size() > 512) {
            std::cerr << "Warning: Cached function '" << stmt.token.lexeme
                      << "' is " << measured_code.size() << " bytes, exceeding the 512-byte cache limit. (at line "
                      << stmt.token.line_number << ")" << std::endl;
        }
    }

    // The body generation is now safe. I hope...
    for (const auto& s : stmt.body) {
        s->accept(*this);
    }

    if (stmt.needs_implicit_return) {
        emitFunctionEpilogue();
    }
}

void CodeGenerator::visit(BlockStmt& stmt) {
    for (const auto& s : stmt.statements) s->accept(*this);
}

void CodeGenerator::visit(VarDeclStmt& stmt) {
    // Allocation is now handled in the function prologue.
    // We only handle initialization here. Make sure this is the right function, ok?
    if (stmt.initializer) {
        // 1. Evaluate the initializer. Result is in R0.
        stmt.initializer->accept(*this, &stmt.type);
        dereferenceIfNeeded(*stmt.initializer);

        // 2. Save the value in a scratch register (R1 or R3).
        uint8_t scratch_reg = m_isInPlottingContext ? 3 : 1;
        emitMove(scratch_reg, static_cast<uint8_t>(RETURN_VALUE_REGISTER));

        // 3. Get the address of the variable we are assigning to.
        // This overwrites R0, which is why we saved the value.
        auto var_expr = VariableExpr(stmt.token);
        var_expr.accept(*this, nullptr); // Leaves address of the variable in R0

        // 4. Store the saved value (from scratch_reg) at the address (in R0).
        emitStore(0, scratch_reg, stmt.type.base == BaseType::BYTE);
    }
}

void CodeGenerator::visit(SubscriptExpr& expr, const Type*) {
    expr.index->accept(*this, nullptr);
    dereferenceIfNeeded(*expr.index); 

    if (expr.array->result_type.base == BaseType::WORD) {
        emitByte(static_cast<uint8_t>(OpCode::ALT2));
        emitByte(static_cast<uint8_t>(OpCode::ADD_R));
        emitByte(static_cast<uint8_t>(OpCode::ROL));
    }

    emitPush(static_cast<uint8_t>(RETURN_VALUE_REGISTER));

    expr.array->accept(*this, nullptr);

    // Use R3 as the scratch register if we are in a plotting context.
    uint8_t scratch_reg = m_isInPlottingContext ? 3 : 1; // R3 or R1

    // POP offset into scratch_reg
    emitPop(scratch_reg);
    
    // SUB scratch_reg (R0 = R0 - scratch_reg)
    emitByte(0x60 | scratch_reg);
}

void CodeGenerator::visit(MemberAccessExpr& expr, const Type*) {
    expr.object->accept(*this, nullptr);

    int offset = expr.member_offset;

    if (offset > 0) {
        // Use R3 as the scratch register if we are in a plotting context.
        uint8_t scratch_reg = m_isInPlottingContext ? 3 : 1; // R3 or R1

        // MOVE scratch_reg, R0
        emitMove(scratch_reg, static_cast<uint8_t>(RETURN_VALUE_REGISTER));
        
        // Load offset into R0.
        emitLiteral(offset, {BaseType::WORD, "", 2, false});

        // ADD scratch_reg (R0 = R0 + scratch_reg)
        emitByte(0x50 | scratch_reg);
    }
}

void CodeGenerator::dereferenceIfNeeded(Expr& expr) {
    if (dynamic_cast<LiteralExpr*>(&expr) ||
        dynamic_cast<BinaryExpr*>(&expr) ||
        dynamic_cast<UnaryExpr*>(&expr) ||
        dynamic_cast<AddressOfExpr*>(&expr) || 
        dynamic_cast<CastExpr*>(&expr) ||
        dynamic_cast<CallExpr*>(&expr) ||
        dynamic_cast<DereferenceExpr*>(&expr))
    {
        return; 
    }
    
    if (auto* var = dynamic_cast<VariableExpr*>(&expr)) {
        if (var->token.lexeme == "plot_x") {
            emitMove(static_cast<uint8_t>(RETURN_VALUE_REGISTER), 1);
            return;
        }
        if (var->token.lexeme == "plot_y") {
            emitMove(static_cast<uint8_t>(RETURN_VALUE_REGISTER), 2);
            return;
        }
    }

    if (expr.result_type.base == BaseType::BYTE) {
        emitByte(static_cast<uint8_t>(OpCode::ALT1));
        emitByte(0x40); // LDB (R0) (LDB only exists with ALT1 attached to it, otherwise it is a simple LDW)
    } else {
        emitByte(0x40); // LDW (R0)
    }
}


void CodeGenerator::visit(AssignExpr& expr, const Type*) {
    expr.name->accept(*this, nullptr);
    
    emitPush(static_cast<uint8_t>(RETURN_VALUE_REGISTER));

    expr.value->accept(*this, nullptr);
    dereferenceIfNeeded(*expr.value);

    // Use R3 as the scratch register if we are in a plotting context.
    uint8_t scratch_reg = m_isInPlottingContext ? 3 : 1; // R3 or R1

    // POP scratch_reg
    emitPop(scratch_reg);

    emitStore(scratch_reg, static_cast<uint8_t>(RETURN_VALUE_REGISTER),
              expr.result_type.base == BaseType::BYTE);
}

void CodeGenerator::visit(ReturnStmt& stmt) {
    if (stmt.value) {
        stmt.value->accept(*this, nullptr);
        dereferenceIfNeeded(*stmt.value);
    }
    
    emitFunctionEpilogue();
}

void CodeGenerator::emitFunctionEpilogue() {
    // FUNCTION EPILOGUE
    // 1. Deallocate locals by resetting SP to FP
    emitMove(10, 9);

    if (m_currentFunctionName == "main") {
        emitByte(static_cast<uint8_t>(OpCode::STOP));
        emitByte(static_cast<uint8_t>(OpCode::NOP));
    } else {
        // Restore the caller's frame and link registers before returning.
        emitPop(9);
        emitPop(11);
        emitByte(0x9B); // JMP R11 (equivalent to RET)
        emitByte(static_cast<uint8_t>(OpCode::NOP));
    }
}

void CodeGenerator::visit(LiteralExpr& expr, const Type*) {
    long value = std::stol(expr.token.lexeme, nullptr, 0);
    emitLiteral(value, expr.result_type);
}

void CodeGenerator::visit(UnaryExpr& expr, const Type*) {
    if (auto* literal = dynamic_cast<LiteralExpr*>(expr.right.get())) {
        long value = std::stol(literal->token.lexeme);
        emitLiteral(-value, expr.result_type);
    } else {
        expr.right->accept(*this, nullptr);
        emitByte(0x4F); emitByte(0xD0); // NOT + INC R0
    }
}

void CodeGenerator::visit(AddressOfExpr& expr, const Type*) {
    // The operand of '&' is an L-value. We just need to visit it.
    // Our L-value visitors (like visit(VariableExpr)) are designed to
    // leave the final ADDRESS in R0, which is exactly what '&' needs to do. If I think is like that.
    expr.right->accept(*this, nullptr);
}

void CodeGenerator::visit(DereferenceExpr& expr, const Type*) {
    if (expr.right->result_type.is_far) {
        // Use R3/R4 for bank/offset registers if we are in a plotting context.
        uint8_t bank_reg = m_isInPlottingContext ? 3 : 1;
        uint8_t offset_reg = m_isInPlottingContext ? 4 : 2;
        // If R3 is our bank_reg, we need a different temp register for the 'p' address. Use R5.
        uint8_t p_addr_save_reg = m_isInPlottingContext ? 5 : 3;

        expr.right->accept(*this, nullptr);

        // Load bank byte from [stack_addr_of_p] into bank_reg.
        emitByte(0x20 | bank_reg); // WITH bank_reg
        emitByte(static_cast<uint8_t>(OpCode::ALT1)); 
        emitByte(0x40); // LDB (R0) -> bank_reg = bank_byte (LDB only exists with ALT1 attached to it, otherwise it is a simple LDW)

        // MOVE p_addr_save_reg, R0 (save address of p)
        emitMove(p_addr_save_reg, static_cast<uint8_t>(RETURN_VALUE_REGISTER));

        emitLiteral(2, {BaseType::BYTE, "", 1, false}); // R0 = 2

        // ADD p_addr_save_reg -> R0 now holds address of p+2
        emitByte(0x50 | p_addr_save_reg);

        // Load 16-bit offset from [address of p+2] into offset_reg.
        emitByte(0x20 | offset_reg); // WITH offset_reg
        emitByte(0x40);              // LDW (R0) -> offset_reg = 16-bit offset

        if (expr.result_type.space == AddressSpace::ROM) {
            emitByte(static_cast<uint8_t>(OpCode::ALT3));
            emitByte(0x20 | bank_reg); // WITH bank_reg
            emitByte(static_cast<uint8_t>(OpCode::GETC));
        } else {
            emitByte(static_cast<uint8_t>(OpCode::ALT2));
            emitByte(0x20 | bank_reg); // WITH bank_reg
            emitByte(static_cast<uint8_t>(OpCode::GETC));
        }

        if (expr.result_type.space == AddressSpace::ROM) {
            // MOVE R14, offset_reg
            emitMove(14, offset_reg);
            if (expr.result_type.base == BaseType::BYTE) {
                emitByte(static_cast<uint8_t>(OpCode::GETB));
            } else {
                emitByte(static_cast<uint8_t>(OpCode::GETB));
                emitByte(0xDE);
                emitByte(static_cast<uint8_t>(OpCode::ALT1));
                emitByte(static_cast<uint8_t>(OpCode::GETB));
            }
        } else { // RAM access
            if (expr.result_type.base == BaseType::BYTE) {
                emitByte(static_cast<uint8_t>(OpCode::ALT1));
                emitByte(0x40 | offset_reg); // LDB (offset_reg) (LDB only exists with ALT1 attached to it, otherwise it is a simple LDW)
            } else {
                emitByte(0x40 | offset_reg); // LDW (offset_reg)
            }
        }

    } else {
        expr.right->accept(*this, nullptr);
        dereferenceIfNeeded(*expr.right);
        if (expr.result_type.space == AddressSpace::ROM) {
            emitMove(14, static_cast<uint8_t>(RETURN_VALUE_REGISTER));
            if (expr.result_type.base == BaseType::BYTE) {
                emitByte(static_cast<uint8_t>(OpCode::GETB));
            } else {
                emitByte(static_cast<uint8_t>(OpCode::GETB));
                emitByte(0xDE);
                emitByte(static_cast<uint8_t>(OpCode::ALT1));
                emitByte(static_cast<uint8_t>(OpCode::GETB));
            }
        } else {
            if (expr.result_type.base == BaseType::BYTE) {
                emitByte(static_cast<uint8_t>(OpCode::ALT1)); emitByte(0x40); // LDB (R0) (LDB only exists with ALT1 attached to it, otherwise it is a simple LDW)
            } else {
                emitByte(0x40); // LDW (R0)
            }
        }
    }
}

void CodeGenerator::visit(StructDefStmt&) {} 
void CodeGenerator::visit(ConstDataStmt&) {}

void CodeGenerator::visit(BinaryExpr& expr, const Type*) {
    auto try_optimize_immediate = [&](Expr* non_literal_side, LiteralExpr* literal_side) -> bool {
        long value = std::stol(literal_side->token.lexeme, nullptr, 0);
        uint8_t op_base = 0; uint8_t alt_prefix = 0;
        switch (expr.token.type) {
            case TokenType::PLUS:  if (value >= 0 && value <= 15) { op_base = 0x50; alt_prefix = 0x3E; } break;
            case TokenType::MINUS: if (value >= 0 && value <= 15) { op_base = 0x60; alt_prefix = 0x3E; } break;
            case TokenType::STAR:  if (value >= 0 && value <= 15) { op_base = 0x80; alt_prefix = 0x3E; } break;
            default: return false;
        }
        if (op_base != 0) {
            non_literal_side->accept(*this, nullptr);
            dereferenceIfNeeded(*non_literal_side);
            emitByte(alt_prefix);
            emitByte(op_base | static_cast<uint8_t>(value));
            return true;
        }
        return false;
    };
    if (!dynamic_cast<LiteralExpr*>(expr.left.get()) && dynamic_cast<LiteralExpr*>(expr.right.get())) {
        if (try_optimize_immediate(expr.left.get(), dynamic_cast<LiteralExpr*>(expr.right.get()))) return;
    }
    if (dynamic_cast<LiteralExpr*>(expr.left.get()) && !dynamic_cast<LiteralExpr*>(expr.right.get())) {
        if (expr.token.type == TokenType::PLUS || expr.token.type == TokenType::STAR) {
            if (try_optimize_immediate(expr.right.get(), dynamic_cast<LiteralExpr*>(expr.left.get()))) return;
        }
    }

    expr.right->accept(*this, nullptr);
    dereferenceIfNeeded(*expr.right);
    
    emitPush(static_cast<uint8_t>(RETURN_VALUE_REGISTER));

    expr.left->accept(*this, nullptr);
    dereferenceIfNeeded(*expr.left);
    
    // Use R3 as the scratch register if we are in a plotting context.
    uint8_t scratch_reg = m_isInPlottingContext ? 3 : 1; // R3 or R1

    // POP scratch_reg
    emitPop(scratch_reg);

    uint8_t op_base = 0;
    uint8_t alt_prefix = 0;
    switch (expr.token.type) {
        case TokenType::PLUS:    op_base = 0x50; break;
        case TokenType::MINUS:   op_base = 0x60; break;
        case TokenType::STAR:    op_base = 0x80; break;
        case TokenType::GREATER:
        case TokenType::LESS:
        case TokenType::EQUAL_EQUAL:
        case TokenType::BANG_EQUAL:
            op_base = 0x60;
            alt_prefix = static_cast<uint8_t>(OpCode::ALT3);
            break;
        default: throw CompilerError("CodeGen: Unknown binary operator.", expr.token.line_number, expr.token.col_number);
    }

    if (alt_prefix != 0) emitByte(alt_prefix);
    emitByte(op_base | scratch_reg); // Op with scratch_reg
}

void CodeGenerator::visit(VariableExpr& expr, const Type*) {
    // Plotting registers are special and not on the stack.
    if (expr.token.lexeme == "plot_x" || expr.token.lexeme == "plot_y") {
        return;
    }

    if (m_symbolTable.count(expr.token.lexeme)) {
        const Symbol& symbol = m_symbolTable.at(expr.token.lexeme);
        int offset = symbol.stackOffset;

        // 1. Load Frame Pointer into R0
        emitMove(static_cast<uint8_t>(RETURN_VALUE_REGISTER), 9);

        // 2. Add or subtract the offset
        if (offset != 0) {
            if (offset > 0) { // It's a parameter, ADD
                emitImmediateArithmetic(0x50, offset);
            } else { // It's a local, SUB
                emitImmediateArithmetic(0x60, std::abs(offset));
            }
        }
        // The final address is now in R0. If I remember it correctly.
        return;
    }
    
    if (m_data_manager.hasSymbol(expr.token.lexeme)) {
        size_t patch_offset = m_object_file.code_section.size();
        emitByte(static_cast<uint8_t>(OpCode::IWT) | static_cast<uint8_t>(RETURN_VALUE_REGISTER));
        emitWord(0);
        m_object_file.relocation_table.push_back({
            expr.token.lexeme, SymbolSection::CODE, (uint32_t)patch_offset, RelocationType::ADDR16_IWT
        });
        return;
    }
    
    if (m_global_function_symbols.count(expr.token.lexeme)) {
        size_t patch_offset = m_object_file.code_section.size();
        emitByte(static_cast<uint8_t>(OpCode::IWT) | static_cast<uint8_t>(RETURN_VALUE_REGISTER));
        emitWord(0);
        m_object_file.relocation_table.push_back({
            expr.token.lexeme, SymbolSection::CODE, (uint32_t)patch_offset, RelocationType::ADDR16_IWT
        });
        return;
    }
    throw CompilerError("CodeGen Error: Undeclared variable '" + expr.token.lexeme + "'.", expr.token.line_number, expr.token.col_number);
}

void CodeGenerator::visit(IfStmt& stmt) {
    stmt.condition->accept(*this, nullptr);
    dereferenceIfNeeded(*stmt.condition);
    
    auto* conditionExpr = dynamic_cast<BinaryExpr*>(stmt.condition.get());
    if (!conditionExpr) throw CompilerError("If condition must be a binary expression.", stmt.condition->token.line_number, stmt.condition->token.col_number);

    uint8_t branchOpcode;
    switch (conditionExpr->token.type) {
        case TokenType::GREATER:     branchOpcode = static_cast<uint8_t>(OpCode::BMI); break;
        case TokenType::LESS:        branchOpcode = static_cast<uint8_t>(OpCode::BPL); break;
        case TokenType::EQUAL_EQUAL: branchOpcode = static_cast<uint8_t>(OpCode::BNE); break;
        case TokenType::BANG_EQUAL:  branchOpcode = static_cast<uint8_t>(OpCode::BEQ); break;
        default: throw CompilerError("Unsupported operator in if condition.", conditionExpr->token.line_number, conditionExpr->token.col_number);
    }
    
    emitByte(branchOpcode);
    size_t else_jump_patch = m_object_file.code_section.size();
    emitByte(0x00);
    emitByte(static_cast<uint8_t>(OpCode::NOP));

    stmt.thenBranch->accept(*this);
    
    size_t end_jump_patch = 0;
    if (stmt.elseBranch) {
        emitByte(static_cast<uint8_t>(OpCode::BRA));
        end_jump_patch = m_object_file.code_section.size();
        emitByte(0x00);
        emitByte(static_cast<uint8_t>(OpCode::NOP));
    }
    
    size_t else_block_address = m_object_file.code_section.size();
    patchRelativeBranch(else_jump_patch, else_block_address, stmt.token);
    
    if (stmt.elseBranch) {
        stmt.elseBranch->accept(*this);
        size_t end_if_address = m_object_file.code_section.size();
        patchRelativeBranch(end_jump_patch, end_if_address, stmt.token);
    }
}

void CodeGenerator::visit(WhileStmt& stmt) {
    size_t loopTopAddress = m_object_file.code_section.size();

    stmt.condition->accept(*this, nullptr);
    dereferenceIfNeeded(*stmt.condition);

    auto* conditionExpr = dynamic_cast<BinaryExpr*>(stmt.condition.get());
    if (!conditionExpr) throw CompilerError("While condition must be a binary expression.", stmt.condition->token.line_number, stmt.condition->token.col_number);

    uint8_t branchOpcode;
    switch (conditionExpr->token.type) {
        case TokenType::GREATER:     branchOpcode = static_cast<uint8_t>(OpCode::BMI); break;
        case TokenType::LESS:        branchOpcode = static_cast<uint8_t>(OpCode::BPL); break;
        case TokenType::EQUAL_EQUAL: branchOpcode = static_cast<uint8_t>(OpCode::BNE); break;
        case TokenType::BANG_EQUAL:  branchOpcode = static_cast<uint8_t>(OpCode::BEQ); break;
        default: throw CompilerError("Unsupported operator in while condition.", conditionExpr->token.line_number, conditionExpr->token.col_number);
    }

    emitByte(branchOpcode);
    size_t exitJumpPlaceholder = m_object_file.code_section.size();
    emitByte(0); 
    emitByte(static_cast<uint8_t>(OpCode::NOP)); 

    // Cache size check
    if (stmt.is_cached && m_config.warn_on_cache_overflow) {
        // The body might not be a block, so we can't easily wrap it.
        // Instead, we just measure the body directly.
        std::vector<uint8_t> measured_code = visitAndMeasure(*stmt.body);
        if (measured_code.size() > 512) {
             std::cerr << "Warning: Body of cached 'while' loop is " << measured_code.size()
                      << " bytes, exceeding the 512-byte cache limit. (at line "
                      << stmt.token.line_number << ")" << std::endl;
        }
    }
    
    stmt.body->accept(*this);

    emitByte(static_cast<uint8_t>(OpCode::BRA));
    const std::size_t loop_back_patch = m_object_file.code_section.size();
    emitByte(0);
    patchRelativeBranch(loop_back_patch, loopTopAddress, stmt.token);
    emitByte(static_cast<uint8_t>(OpCode::NOP));

    size_t loopEndAddress = m_object_file.code_section.size();
    patchRelativeBranch(exitJumpPlaceholder, loopEndAddress, stmt.token);
}

void CodeGenerator::visit(HardwareLoopStmt& stmt) {
    long count = std::stol(stmt.count->token.lexeme, nullptr, 0);

    emitByte(static_cast<uint8_t>(OpCode::IWT) | 0x0C); // IWT to R12
    emitWord(static_cast<uint16_t>(count));

    emitByte(0x2D); // WITH R13
    emitByte(0x1F); // TO R15
    
    stmt.body->accept(*this);

    emitByte(0x3C); // LOOP

    emitByte(static_cast<uint8_t>(OpCode::NOP));
}

void CodeGenerator::visit(ExpressionStmt& stmt) {
    stmt.expression->accept(*this, nullptr);
}

void CodeGenerator::visit(PlotStmt& stmt) {
    // This function is the one that USES R1 and R2.
    stmt.y->accept(*this, nullptr);
    dereferenceIfNeeded(*stmt.y);
    emitMove(2, static_cast<uint8_t>(RETURN_VALUE_REGISTER));

    stmt.x->accept(*this, nullptr);
    dereferenceIfNeeded(*stmt.x);
    emitMove(1, static_cast<uint8_t>(RETURN_VALUE_REGISTER));

    emitByte(0x4C); // PLOT opcode
}

void CodeGenerator::visit(PlotBeginStmt&) {
    m_isInPlottingContext = true;
}

void CodeGenerator::visit(PlotEndStmt&) {
    m_isInPlottingContext = false;
}

void CodeGenerator::visit(SetColorStmt& stmt) {
    stmt.color_value->accept(*this, nullptr);
    dereferenceIfNeeded(*stmt.color_value);
    emitByte(static_cast<uint8_t>(OpCode::COLOR_R));
}

void CodeGenerator::visit(CmodeStmt& stmt) {
    stmt.options_value->accept(*this, nullptr);
    dereferenceIfNeeded(*stmt.options_value);
    emitByte(static_cast<uint8_t>(OpCode::ALT1));
    emitByte(0x4E); // COLOR opcode
}

void CodeGenerator::visit(RpixStmt&) {
    emitByte(static_cast<uint8_t>(OpCode::ALT1));
    emitByte(0x4C); // PLOT opcode
}

std::string CodeGenerator::newLabel() {
    return "_L" + std::to_string(m_label_counter++);
}

void CodeGenerator::visit(SwitchStmt& stmt) {
    std::vector<size_t> break_patch_locations; 
    m_break_labels.push_back(&break_patch_locations);

    DefaultStmt* default_stmt = nullptr;
    std::vector<CaseStmt*> case_stmts;
    std::map<CaseStmt*, size_t> case_jump_patches;
    std::map<std::string, size_t> label_addresses;

    for (const auto& s : stmt.body->statements) {
        if (auto* cs = dynamic_cast<CaseStmt*>(s.get())) case_stmts.push_back(cs);
        else if (auto* ds = dynamic_cast<DefaultStmt*>(s.get())) default_stmt = ds;
    }

    stmt.condition->accept(*this, nullptr);
    dereferenceIfNeeded(*stmt.condition);
    
    for (CaseStmt* cs : case_stmts) {
        // Use R3 for comparison if plotting, since well, GSU uses registers in a different way
        uint8_t scratch_reg = m_isInPlottingContext ? 3 : 1;
        emitMove(scratch_reg, static_cast<uint8_t>(RETURN_VALUE_REGISTER));
        
        cs->value->accept(*this, nullptr); // Literal value is now in R0
        
        emitByte(static_cast<uint8_t>(OpCode::ALT3)); // alt3
        emitByte(0x60 | scratch_reg); // CMP scratch_reg
        
        emitByte(static_cast<uint8_t>(OpCode::BEQ));
        case_jump_patches[cs] = m_object_file.code_section.size();
        emitByte(0x00);
        emitByte(static_cast<uint8_t>(OpCode::NOP));
    }
    
    emitByte(static_cast<uint8_t>(OpCode::BRA));
    size_t default_jump_patch = m_object_file.code_section.size();
    emitByte(0x00);
    emitByte(static_cast<uint8_t>(OpCode::NOP));

    for (const auto& s : stmt.body->statements) {
        if (auto* cs = dynamic_cast<CaseStmt*>(s.get())) {
            label_addresses[cs->label_name] = m_object_file.code_section.size();
        } else if (auto* ds = dynamic_cast<DefaultStmt*>(s.get())) {
            label_addresses[ds->label_name] = m_object_file.code_section.size();
        } else {
            s->accept(*this);
        }
    }
    size_t end_label_address = m_object_file.code_section.size();

    for (auto const& [cs, patch_addr] : case_jump_patches) {
        size_t target_addr = label_addresses.at(cs->label_name);
        patchRelativeBranch(patch_addr, target_addr, cs->token);
    }
    
    size_t default_target_addr = default_stmt ? label_addresses.at(default_stmt->label_name) : end_label_address;
    patchRelativeBranch(
        default_jump_patch,
        default_target_addr,
        default_stmt ? default_stmt->token : stmt.token);

    for (size_t patch_addr : break_patch_locations) {
        patchRelativeBranch(patch_addr, end_label_address, stmt.token);
    }

    m_break_labels.pop_back();
}

void CodeGenerator::visit(CaseStmt&) {}
void CodeGenerator::visit(DefaultStmt&) {}

void CodeGenerator::visit(BreakStmt&) {
    if (m_break_labels.empty() || m_break_labels.back() == nullptr) {
        throw std::runtime_error("Internal codegen error: break context is invalid.");
    }
    emitByte(static_cast<uint8_t>(OpCode::BRA));
    size_t patch_loc = m_object_file.code_section.size();
    emitByte(0x00);
    emitByte(static_cast<uint8_t>(OpCode::NOP));
    m_break_labels.back()->push_back(patch_loc);
}

void CodeGenerator::visit(CastExpr& expr, const Type*) {
    expr.expression->accept(*this, nullptr);
    dereferenceIfNeeded(*expr.expression);

    const Type& sourceType = expr.expression->result_type;
    const Type& targetType = expr.cast_to_type;

    if (targetType.sizeInBytes == 2 && sourceType.sizeInBytes == 1) {
        // Widening a byte to a word must define the high byte, of course.
        // - signed byte  -> word : sign-extend
        // - unsigned byte-> word : zero-extend
        if (!sourceType.is_unsigned) {
            emitByte(static_cast<uint8_t>(OpCode::SEX));
        }
    }
}
