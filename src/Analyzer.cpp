#include "Analyzer.hpp"
#include "ABI.hpp"
#include <cstdint>
#include <stdexcept>
#include <set>
#include <limits>
#include "CompilerError.hpp"

namespace {

std::int64_t parseIntegerLiteral(const Token& token, const std::string& context) {
    try {
        std::size_t parsed = 0;
        const auto value = std::stoll(token.lexeme, &parsed, 0);
        if (parsed != token.lexeme.size()) throw std::invalid_argument("trailing characters");
        return value;
    } catch (const std::exception&) {
        throw CompilerError("Invalid integer literal for " + context + ".",
                            token.line_number, token.col_number);
    }
}

} // namespace

Analyzer::Analyzer(DataSegmentManager& dataManager) : m_data_manager(dataManager) {}

const std::map<std::string, FunctionSymbol>& Analyzer::getFunctionSymbols() const {
    return m_function_symbols;
}

const std::map<std::string, Analyzer::LocalSymbolTable>& Analyzer::getAllLocalSymbols() const {
    return m_all_local_symbols;
}

SymbolId Analyzer::createSymbolId() {
    if (m_next_symbol_id == SymbolId::Invalid) {
        throw std::runtime_error("Analyzer ran out of stable symbol identifiers.");
    }
    return SymbolId{m_next_symbol_id++};
}

void Analyzer::registerFunctionSymbol(FunctionDeclStmt& stmt) {
    if (m_function_symbols.count(stmt.token.lexeme)) {
        throw CompilerError("Function '" + stmt.token.lexeme + "' already declared.", stmt.token.line_number, stmt.token.col_number);
    }
    FunctionSymbol symbol;
    symbol.returnType = stmt.returnType;
    for (const auto& param : stmt.params) {
        symbol.paramTypes.push_back(param.type);
    }
    m_function_symbols[stmt.token.lexeme] = symbol;
}

void Analyzer::registerStructSymbol(StructDefStmt& stmt) {
    if (m_struct_symbols.count(stmt.token.lexeme)) {
        throw CompilerError("Struct '" + stmt.token.lexeme + "' already defined.", stmt.token.line_number, stmt.token.col_number);
    }

    StructSymbol struct_symbol;
    struct_symbol.name = stmt.token.lexeme;
    int current_offset = 0;

    for (auto& member : stmt.members) {
        if (struct_symbol.members.count(member.name.lexeme)) {
            throw CompilerError("Duplicate member '" + member.name.lexeme + "' in struct '" + stmt.token.lexeme + "'.", member.name.line_number, member.name.col_number);
        }
        
        if (member.type.base == BaseType::STRUCT) {
            if (!m_struct_symbols.count(member.type.structName)) {
                throw CompilerError("Struct member '" + member.name.lexeme + "' has unknown type 'struct " + member.type.structName + "'.", member.name.line_number, member.name.col_number);
            }
            member.type.sizeInBytes = m_struct_symbols.at(member.type.structName).totalSize;
        } else if (member.type.pointer_level > 0) {
            member.type.sizeInBytes = 2;
        } else if (member.type.base == BaseType::BYTE) {
            member.type.sizeInBytes = 1;
        } else if (member.type.base == BaseType::WORD) {
            member.type.sizeInBytes = 2;
        }

        if (current_offset % 2 != 0) current_offset++;
        if (member.type.sizeInBytes < 0 ||
            current_offset > std::numeric_limits<int>::max() - member.type.sizeInBytes) {
            throw CompilerError("Struct layout exceeds the supported size limit.",
                                member.name.line_number, member.name.col_number);
        }
        
        struct_symbol.members[member.name.lexeme] = {member.type, current_offset};
        current_offset += member.type.sizeInBytes;
    }
    
    struct_symbol.totalSize = ((current_offset + 1) / 2) * 2;
    m_struct_symbols[stmt.token.lexeme] = struct_symbol;
}

void Analyzer::registerRomSymbol(ConstDataStmt& stmt) {
    if (m_data_manager.hasSymbol(stmt.token.lexeme)) {
         throw CompilerError("ROM symbol '" + stmt.token.lexeme + "' already declared.", stmt.token.line_number, stmt.token.col_number);
    }
    m_data_manager.add(stmt);
}

void Analyzer::beginScope() {
    m_scopes.emplace_back();
}

void Analyzer::endScope() {
    m_scopes.pop_back();
}

void Analyzer::analyze(const std::vector<std::unique_ptr<Stmt>>& program) {
    m_scopes.clear();
    m_function_symbols.clear();
    m_struct_symbols.clear();
    m_all_local_symbols.clear();
    m_next_symbol_id = 1;
    m_break_context_stack.clear();
    m_case_values_stack.clear();
    m_isInPlottingContext = false;

    for (const auto& stmt : program)
        if (auto* s = dynamic_cast<StructDefStmt*>(stmt.get())) registerStructSymbol(*s);
    for (const auto& stmt : program)
        if (auto* func = dynamic_cast<FunctionDeclStmt*>(stmt.get())) registerFunctionSymbol(*func);
    for (const auto& stmt : program)
        if (auto* data = dynamic_cast<ConstDataStmt*>(stmt.get())) registerRomSymbol(*data);
    for (const auto& stmt : program) {
        stmt->accept(*this);
    }
}

void Analyzer::analyzeExpr(Expr& expr, const Type* context) {
    expr.accept(*this, context);
}

void Analyzer::visit(FunctionDeclStmt& stmt) {
    if (stmt.is_prototype) return;

    // Create a new local symbol table for this function in our main map.
    m_current_function_locals = &m_all_local_symbols[stmt.token.lexeme];
    m_current_function_locals->clear();
    
    m_scopes.clear();
    m_currentFunctionType = stmt.returnType;
    beginScope();

    // STEP 1: Process parameters. They have POSITIVE offsets from the frame pointer.
    // The layout is: [FP+4]=Param1, [FP+2]=ReturnAddr, [FP]=Old_FP
    int paramOffset = GSUAbi::FirstParameterOffset;
    for (auto& param : stmt.params) {
        if (m_scopes.back().count(param.name.lexeme)) {
            throw CompilerError("Duplicate parameter name '" + param.name.lexeme + "'.", param.name.line_number, param.name.col_number);
        }

        // Calculate and store the size for the parameter's type.
        if (param.type.pointer_level > 0) param.type.sizeInBytes = 2;
        else if (param.type.base == BaseType::BYTE) param.type.sizeInBytes = 1;
        else if (param.type.base == BaseType::WORD) param.type.sizeInBytes = 2;
        else if (param.type.base == BaseType::STRUCT) {
            if (!m_struct_symbols.count(param.type.structName)) {
                 throw CompilerError("Parameter '" + param.name.lexeme + "' has unknown type 'struct " + param.type.structName + "'.", param.name.line_number, param.name.col_number);
            }
            param.type.sizeInBytes = m_struct_symbols.at(param.type.structName).totalSize;
        }

        const auto symbol_id = createSymbolId();
        Symbol symbol{param.type, paramOffset, "", symbol_id};
        m_current_function_locals->emplace(symbol_id, symbol);
        param.symbol_id = symbol_id;
        m_scopes.back()[param.name.lexeme] = symbol_id;
        paramOffset += static_cast<int>(GSUAbi::ParameterSlotSize);
    }

    // STEP 2: Process the function body to find local variables and their sizes.
    // We will calculate their NEGATIVE offsets from the frame pointer.
    auto bodyBlock = std::make_unique<BlockStmt>(std::move(stmt.body));
    bodyBlock->accept(*this);
    stmt.body = std::move(bodyBlock->statements);
    endScope();

    stmt.needs_implicit_return = blockCanFallThrough(stmt.body);
    if (stmt.returnType.base != BaseType::VOID && stmt.needs_implicit_return) {
        throw CompilerError(
            "Non-void function may reach the end without returning a value.",
            stmt.token.line_number,
            stmt.token.col_number);
    }

    // STEP 3: Calculate the total allocation size for all locals in this function.
    // This is needed by the code generators for the function prologue.
    int total_local_size = 0;
    for (const auto& pair : *m_current_function_locals) {
        const auto& symbol = pair.second;
        // Only sum up locals (negative offsets).
        if (symbol.stackOffset < 0) {
            if (symbol.type.sizeInBytes <= 0) {
                throw CompilerError("Local variable has an invalid size.",
                                    Token(TokenType::UNKNOWN, "", 0, 0).line_number,
                                    Token(TokenType::UNKNOWN, "", 0, 0).col_number);
            }
            int var_size = ((symbol.type.sizeInBytes + 1) / 2) * 2;
            if (symbol.type.array_size > 0) {
                 int element_size = symbol.type.sizeInBytes;
                 if (symbol.type.array_size > std::numeric_limits<int>::max() / element_size) {
                     throw CompilerError("Local allocation exceeds the supported size limit.",
                                         Token(TokenType::UNKNOWN, "", 0, 0).line_number,
                                         Token(TokenType::UNKNOWN, "", 0, 0).col_number);
                 }
                 var_size = ((symbol.type.array_size * element_size + 1) / 2) * 2;
            }
            if (var_size > std::numeric_limits<int>::max() - total_local_size) {
                throw CompilerError("Local allocation exceeds the supported size limit.",
                                    Token(TokenType::UNKNOWN, "", 0, 0).line_number,
                                    Token(TokenType::UNKNOWN, "", 0, 0).col_number);
            }
            total_local_size += var_size;
        }
    }
    stmt.total_local_alloc_size = total_local_size;
    
    // Reset the pointer for the next function.
    m_current_function_locals = nullptr;
}

void Analyzer::visit(BlockStmt& stmt) {
    beginScope();
    int currentLocalOffset = 0; // Block-scoped offset tracking

    // Find the current frame offset to continue from.
    if (m_scopes.size() > 1) { // If not the top-level block of a function
        auto& parentScope = m_scopes[m_scopes.size() - 2];
        int minOffset = 0;
        for (const auto& pair : parentScope) {
            const auto& symbol = m_current_function_locals->at(pair.second);
            if (symbol.stackOffset < minOffset) {
                minOffset = symbol.stackOffset;
            }
        }
        currentLocalOffset = minOffset;
    }

    for (const auto& s : stmt.statements) {
        if (auto* varDecl = dynamic_cast<VarDeclStmt*>(s.get())) {
            // This is a bit of a hack. The visit method doesn't take extra params. 
            // We set the offset before visiting. Seems is the best to do...
            varDecl->base_stack_offset = currentLocalOffset;
            varDecl->accept(*this);
            // After visiting, the VarDeclStmt's symbol has the final offset. Update our tracker.
            currentLocalOffset = m_current_function_locals->at(varDecl->symbol_id).stackOffset;
        } else {
            s->accept(*this);
        }
    }
    endScope();
}

void Analyzer::visit(VarDeclStmt& stmt) {
    if (m_scopes.back().count(stmt.token.lexeme)) {
        throw CompilerError("Symbol '" + stmt.token.lexeme + "' already declared in this scope.", stmt.token.line_number, stmt.token.col_number);
    }
    if (m_data_manager.hasSymbol(stmt.token.lexeme) || m_function_symbols.count(stmt.token.lexeme)) {
        throw CompilerError("Symbol '" + stmt.token.lexeme + "' already declared globally.", stmt.token.line_number, stmt.token.col_number);
    }

    if (stmt.initializer) {
        analyzeExpr(*stmt.initializer, &stmt.type);
        if (stmt.type.sizeInBytes < stmt.initializer->result_type.sizeInBytes && !dynamic_cast<CastExpr*>(stmt.initializer.get())) {
             throw CompilerError("Implicit conversion from '" + to_string(stmt.initializer->result_type) + "' to '" + to_string(stmt.type) + "' requires an explicit cast.", stmt.initializer->token.line_number, stmt.initializer->token.col_number);
        }
    }

    // Determine the size of this variable (with alignment)
    if (stmt.type.base == BaseType::STRUCT) {
        if (!m_struct_symbols.count(stmt.type.structName)) {
             throw CompilerError("Variable '" + stmt.token.lexeme + "' has unknown type 'struct " + stmt.type.structName + "'.", stmt.token.line_number, stmt.token.col_number);
        }
        stmt.type.sizeInBytes = m_struct_symbols.at(stmt.type.structName).totalSize;
    } else if (stmt.type.pointer_level > 0) {
        stmt.type.sizeInBytes = 2;
    } else if (stmt.type.base == BaseType::BYTE) {
        stmt.type.sizeInBytes = 1;
    } else {
        stmt.type.sizeInBytes = 2;
    }
    
    int size_to_allocate = ((stmt.type.sizeInBytes + 1) / 2) * 2;
    if (stmt.type.array_size > 0) {
        int element_size = stmt.type.sizeInBytes;
        if (stmt.type.array_size > std::numeric_limits<int>::max() / element_size) {
            throw CompilerError("Array allocation exceeds the supported size limit.",
                                stmt.token.line_number, stmt.token.col_number);
        }
        size_to_allocate = ((stmt.type.array_size * element_size + 1) / 2) * 2;
    }

    // Calculate the new negative offset from the FP.
    int final_offset = stmt.base_stack_offset - size_to_allocate;

    if (!m_current_function_locals) throw std::runtime_error("Internal Analyzer Error: Not in a function context.");
    const auto symbol_id = createSymbolId();
    Symbol symbol{stmt.type, final_offset, "", symbol_id};
    m_current_function_locals->emplace(symbol_id, symbol);
    stmt.symbol_id = symbol_id;
    m_scopes.back()[stmt.token.lexeme] = symbol_id;
}

void Analyzer::visit(StructDefStmt&) {}

void Analyzer::visit(ConstDataStmt&) {}

void Analyzer::visit(ReturnStmt& stmt) {
    if (stmt.value) {
        if (m_currentFunctionType.base == BaseType::VOID) {
            throw CompilerError("Cannot return a value from a void function.", stmt.token.line_number, stmt.token.col_number);
        }
        analyzeExpr(*stmt.value, &m_currentFunctionType);
    } else {
        if (m_currentFunctionType.base != BaseType::VOID) {
            throw CompilerError("Non-void function must return a value.", stmt.token.line_number, stmt.token.col_number);
        }
    }
}

void Analyzer::visit(IfStmt& stmt) {
    analyzeExpr(*stmt.condition, nullptr);
    stmt.thenBranch->accept(*this);
    if (stmt.elseBranch) {
        stmt.elseBranch->accept(*this);
    }
}

void Analyzer::visit(WhileStmt& stmt) {
    analyzeExpr(*stmt.condition, nullptr);
    m_break_context_stack.push_back(false);
    stmt.body->accept(*this);
    m_break_context_stack.pop_back();
}

void Analyzer::visit(HardwareLoopStmt& stmt) {
    beginScope();
    stmt.body->accept(*this);
    endScope();
}

void Analyzer::visit(ExpressionStmt& stmt) {
    analyzeExpr(*stmt.expression, nullptr);
}

void Analyzer::visit(PlotBeginStmt& stmt) {
    if (m_isInPlottingContext) {
        throw CompilerError("Cannot begin a new plotting context within another.", stmt.token.line_number, stmt.token.col_number);
    }
    m_isInPlottingContext = true;
}

void Analyzer::visit(PlotEndStmt& stmt) {
    if (!m_isInPlottingContext) {
        throw CompilerError("Cannot end a plotting context that has not been started.", stmt.token.line_number, stmt.token.col_number);
    }
    m_isInPlottingContext = false;
}

void Analyzer::visit(PlotStmt& stmt) {
    if (!m_isInPlottingContext) {
        throw CompilerError("'plot' can only be used inside a plot_begin/plot_end block.", stmt.token.line_number, stmt.token.col_number);
    }
    analyzeExpr(*stmt.x, nullptr);
    analyzeExpr(*stmt.y, nullptr);
}

void Analyzer::visit(SetColorStmt& stmt) {
    analyzeExpr(*stmt.color_value, nullptr);
}

void Analyzer::visit(CmodeStmt& stmt) {
    analyzeExpr(*stmt.options_value, nullptr);
}

void Analyzer::visit(RpixStmt&) {}

void Analyzer::visit(CallExpr& expr, const Type*) {
    auto* callee_var = dynamic_cast<VariableExpr*>(expr.callee.get());
    if (!callee_var) {
        throw CompilerError("Can only call named functions.", expr.callee->token.line_number, expr.callee->token.col_number);
    }
    if (!m_function_symbols.count(callee_var->token.lexeme)) {
        throw CompilerError("Call to undeclared function '" + callee_var->token.lexeme + "'.", callee_var->token.line_number, callee_var->token.col_number);
    }
    const FunctionSymbol& func = m_function_symbols.at(callee_var->token.lexeme);
    if (expr.arguments.size() != func.paramTypes.size()) {
        throw CompilerError("Function '" + callee_var->token.lexeme + "' expects " +
                                 std::to_string(func.paramTypes.size()) + " arguments but got " +
                                 std::to_string(expr.arguments.size()) + ".", expr.paren.line_number, expr.paren.col_number);
    }
    for (size_t i = 0; i < expr.arguments.size(); ++i) {
        analyzeExpr(*expr.arguments[i], &func.paramTypes[i]);
    }
    expr.result_type = func.returnType;
}

void Analyzer::visit(AssignExpr& expr, const Type*) {
    analyzeExpr(*expr.name, nullptr);
    const Type& targetType = expr.name->result_type;

    if (targetType.space == AddressSpace::ROM) {
        throw CompilerError("Cannot assign to a 'rom' qualified type. ROM is read-only.", expr.token.line_number, expr.token.col_number);
    }
    if (auto* var = dynamic_cast<VariableExpr*>(expr.name.get())) {
        if (var->token.lexeme == "plot_x" || var->token.lexeme == "plot_y") {
            if (!m_isInPlottingContext) {
                throw CompilerError("Special variables 'plot_x' and 'plot_y' can only be used inside a plot_begin/plot_end block.", var->token.line_number, var->token.col_number);
            }
            analyzeExpr(*expr.value, nullptr);
            expr.result_type = {BaseType::WORD, "", 2, false};
            return;
        }
    }

    if (expr.value) {
        analyzeExpr(*expr.value, &targetType);
        if (targetType.sizeInBytes < expr.value->result_type.sizeInBytes && !dynamic_cast<CastExpr*>(expr.value.get())) {
             throw CompilerError("Implicit conversion from '" + to_string(expr.value->result_type) + "' to '" + to_string(targetType) + "' requires an explicit cast.", expr.value->token.line_number, expr.value->token.col_number);
        }
    }
    expr.result_type = targetType;
}

void Analyzer::visit(BinaryExpr& expr, const Type*) {
    analyzeExpr(*expr.left, nullptr);
    analyzeExpr(*expr.right, nullptr);
    // Implicitly promote byte to word for binary operations
    if (expr.left->result_type.sizeInBytes > expr.right->result_type.sizeInBytes) {
        expr.right->result_type = expr.left->result_type;
    } else if (expr.right->result_type.sizeInBytes > expr.left->result_type.sizeInBytes) {
        expr.left->result_type = expr.right->result_type;
    }
    expr.result_type = {BaseType::WORD, "", 2, false};
}

void Analyzer::visit(UnaryExpr& expr, const Type* context) {
    analyzeExpr(*expr.right, context);
    expr.result_type = expr.right->result_type;
}

void Analyzer::visit(MemberAccessExpr& expr, const Type*) {
    analyzeExpr(*expr.object, nullptr);
    if (expr.object->result_type.base != BaseType::STRUCT) {
        throw CompilerError("Request for member '" + expr.token.lexeme + "' in something that is not a struct.", expr.token.line_number, expr.token.col_number);
    }
    const StructSymbol& struct_def = m_struct_symbols.at(expr.object->result_type.structName);
    if (!struct_def.members.count(expr.token.lexeme)) {
        throw CompilerError("Struct '" + struct_def.name + "' has no member named '" + expr.token.lexeme + "'.", expr.token.line_number, expr.token.col_number);
    }
    const StructMemberSymbol& member_def = struct_def.members.at(expr.token.lexeme);
    expr.result_type = member_def.type;
    expr.member_offset = member_def.offset;
}

void Analyzer::visit(AddressOfExpr& expr, const Type*) {
    analyzeExpr(*expr.right, nullptr);
    Type rightType = expr.right->result_type;
    if (dynamic_cast<VariableExpr*>(expr.right.get()) || dynamic_cast<SubscriptExpr*>(expr.right.get()) || dynamic_cast<MemberAccessExpr*>(expr.right.get())) {
        Type resultType = rightType;
        resultType.pointer_level++; 
        expr.result_type = resultType;
    } else {
        throw CompilerError("Address-of operator '&' can only be applied to an l-value (e.g., a variable or array element).", expr.token.line_number, expr.token.col_number);
    }
}

void Analyzer::visit(DereferenceExpr& expr, const Type*) {
    analyzeExpr(*expr.right, nullptr);
    if (expr.right->result_type.pointer_level == 0) {
        throw CompilerError("Cannot dereference a non-pointer type.", expr.token.line_number, expr.token.col_number);
    }
    Type resultType = expr.right->result_type;
    resultType.pointer_level--;
    expr.result_type = resultType;
}

void Analyzer::visit(SubscriptExpr& expr, const Type*) {
    analyzeExpr(*expr.array, nullptr);
    Type arrayType = expr.array->result_type;
    analyzeExpr(*expr.index, nullptr);
    Type indexType = expr.index->result_type;
    if (indexType.base != BaseType::BYTE && indexType.base != BaseType::WORD) {
        throw CompilerError("Array index must be an integer type.", expr.index->token.line_number, expr.index->token.col_number);
    }
    if (arrayType.array_size == 0 && arrayType.pointer_level == 0) {
        throw CompilerError("Subscript operator '[]' can only be used on arrays or pointers.", expr.token.line_number, expr.token.col_number);
    }
    Type resultType = arrayType;
    if (resultType.array_size > 0) {
        resultType.array_size = 0;
    } else {
        resultType.pointer_level--;
    }
    expr.result_type = resultType;
}

void Analyzer::visit(VariableExpr& expr, const Type*) {
    if (expr.token.lexeme == "plot_x" || expr.token.lexeme == "plot_y") {
        if (!m_isInPlottingContext) {
            throw CompilerError("Special variables 'plot_x' and 'plot_y' can only be used inside a plot_begin/plot_end block.", expr.token.line_number, expr.token.col_number);
        }
        expr.result_type = {BaseType::WORD, "", 2, false};
        return;
    }
    for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
        const auto& scope = *it;
        if (scope.count(expr.token.lexeme)) {
            expr.symbol_id = scope.at(expr.token.lexeme);
            expr.result_type = m_current_function_locals->at(expr.symbol_id).type;
            return;
        }
    }
    if (m_data_manager.hasSymbol(expr.token.lexeme)) {
        expr.result_type = m_data_manager.getSymbolType(expr.token.lexeme);
        if (expr.result_type.array_size > 0) {
            expr.result_type.pointer_level++;
            expr.result_type.array_size = 0;
        }
    } else {
        if (m_function_symbols.count(expr.token.lexeme)) {
            expr.result_type = {BaseType::VOID, "", 2, false, 1};
        } else {
            throw CompilerError("Use of undeclared symbol '" + expr.token.lexeme + "'.", expr.token.line_number, expr.token.col_number);
        }
    }
}

void Analyzer::visit(LiteralExpr& expr, const Type* context) {
    const auto value = parseIntegerLiteral(expr.token, "integer literal");
    if (context) {
        if (context->base == BaseType::BYTE) {
            if (context->is_unsigned) {
                if (value < 0 || value > 255) throw CompilerError("Value '" + std::to_string(value) + "' out of range for unsigned byte.", expr.token.line_number, expr.token.col_number);
            } else {
                if (value < -128 || value > 127) throw CompilerError("Value '" + std::to_string(value) + "' out of range for signed byte.", expr.token.line_number, expr.token.col_number);
            }
        } else if (context->base == BaseType::WORD) {
            if (context->is_unsigned) {
                if (value < 0 || value > 65535) throw CompilerError("Value '" + std::to_string(value) + "' out of range for unsigned word.", expr.token.line_number, expr.token.col_number);
            } else {
                if (value < -32768 || value > 32767) throw CompilerError("Value '" + std::to_string(value) + "' out of range for signed word.", expr.token.line_number, expr.token.col_number);
            }
        }
        expr.result_type = *context;
    } else {
        if (value >= -128 && value <= 127) {
            expr.result_type = {BaseType::BYTE, "", 1, false};
        }
        else if (value >= 0 && value <= 255) {
            expr.result_type = {BaseType::BYTE, "", 1, true};
        }
        else if (value >= -32768 && value <= 32767) {
            expr.result_type = {BaseType::WORD, "", 2, false};
        }
        else if (value >= 0 && value <= 65535) {
            expr.result_type = {BaseType::WORD, "", 2, true};
        }
        else throw CompilerError("Literal '" + std::to_string(value) + "' is too large for a 16-bit word.", expr.token.line_number, expr.token.col_number);
    }
}

void Analyzer::visit(CastExpr& expr, const Type*) {
    analyzeExpr(*expr.expression, nullptr);

    // Correctly populate the size of the cast's target type
    if (expr.cast_to_type.base == BaseType::WORD) {
        expr.cast_to_type.sizeInBytes = 2;
    } else if (expr.cast_to_type.base == BaseType::BYTE) {
        expr.cast_to_type.sizeInBytes = 1;
    } else if (expr.cast_to_type.pointer_level > 0) {
        expr.cast_to_type.sizeInBytes = 2; // Pointers are 2 bytes
    }

    const Type& sourceType = expr.expression->result_type;
    const Type& targetType = expr.cast_to_type;

    // Basic validation
    if (sourceType.base == BaseType::STRUCT || targetType.base == BaseType::STRUCT) {
        if (sourceType.structName != targetType.structName) {
            throw CompilerError("Cannot cast between different struct types.", expr.token.line_number, expr.token.col_number);
        }
    }

    if (sourceType.base == BaseType::VOID || targetType.base == BaseType::VOID) {
         throw CompilerError("Cannot cast to or from a void type.", expr.token.line_number, expr.token.col_number);
    }

    // The result type of the cast expression is the type it was cast to.
    expr.result_type = targetType;
}

void Analyzer::visit(SwitchStmt& stmt) {
    m_break_context_stack.push_back(true);
    m_case_values_stack.emplace_back();

    analyzeExpr(*stmt.condition, nullptr);
    if (stmt.condition->result_type.base != BaseType::WORD && stmt.condition->result_type.base != BaseType::BYTE) {
        throw CompilerError("Switch condition must be of an integer type.", stmt.condition->token.line_number, stmt.condition->token.col_number);
    }
    
    bool has_default = false;
    for (const auto& s : stmt.body->statements) {
        if (auto* case_stmt = dynamic_cast<CaseStmt*>(s.get())) {
            case_stmt->accept(*this);
        } else if (auto* default_stmt = dynamic_cast<DefaultStmt*>(s.get())) {
            if (has_default) {
                throw CompilerError("Multiple 'default' labels in one switch statement.", default_stmt->token.line_number, default_stmt->token.col_number);
            }
            has_default = true;
            default_stmt->accept(*this);
        } else {
            s->accept(*this);
        }
    }

    m_break_context_stack.pop_back();
    m_case_values_stack.pop_back();
}

void Analyzer::visit(CaseStmt& stmt) {
    if (m_break_context_stack.empty() || !m_break_context_stack.back()) {
        throw CompilerError("'case' label not within a switch statement.",
                            stmt.token.line_number, stmt.token.col_number);
    }
    auto* literal = dynamic_cast<LiteralExpr*>(stmt.value.get());
    if (!literal) {
        throw CompilerError("Case label must be a constant integer literal.", stmt.value->token.line_number, stmt.value->token.col_number);
    }
    const auto value = parseIntegerLiteral(literal->token, "case label");
    if (m_case_values_stack.back().count(value)) {
        throw CompilerError("Duplicate case value '" + std::to_string(value) + "'.", literal->token.line_number, literal->token.col_number);
    }
    m_case_values_stack.back().insert(value);
}

void Analyzer::visit(DefaultStmt& stmt) {
    if (m_break_context_stack.empty() || !m_break_context_stack.back()) {
        throw CompilerError("'default' label not within a switch statement.", stmt.token.line_number, stmt.token.col_number);
    }
}

void Analyzer::visit(BreakStmt& stmt) {
    if (m_break_context_stack.empty()) {
        throw CompilerError("'break' statement not within a loop or switch.",
                            stmt.token.line_number, stmt.token.col_number);
    }
}

bool Analyzer::blockCanFallThrough(const std::vector<std::unique_ptr<Stmt>>& statements) const {
    for (const auto& statement : statements) {
        if (!canFallThrough(*statement)) {
            return false;
        }
    }
    return true;
}

bool Analyzer::canFallThrough(const Stmt& stmt) const {
    if (dynamic_cast<const ReturnStmt*>(&stmt)) {
        return false;
    }

    if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
        return blockCanFallThrough(block->statements);
    }

    if (const auto* conditional = dynamic_cast<const IfStmt*>(&stmt)) {
        // Without an else branch, the condition can always choose the path
        // that reaches the end of the if statement.
        if (!conditional->elseBranch) {
            return true;
        }
        return canFallThrough(*conditional->thenBranch) ||
               canFallThrough(*conditional->elseBranch);
    }

    // A loop is conservatively considered fall-through.  Proving that a
    // source-controlled loop is infinite requires constant evaluation and
    // handling of break/control-flow edges; rejecting a missing return is
    // safer than accepting an invalid function ABI on an incomplete proof.
    return true;
}
