#include "IntegerLiteral.hpp"
#include "Analyzer.hpp"
#include "ABI.hpp"
#include "GsuPointer.hpp"
#include "ConstantEvaluator.hpp"
#include "TypeAliases.hpp"
#include <cstdint>
#include <stdexcept>
#include <set>
#include <algorithm>
#include <limits>
#include "CompilerError.hpp"

namespace {
bool constantIndex(const Expr& expression, std::int64_t& value);
bool constantPointer(const Expr& expression, std::uint32_t& address);

std::int64_t parseIntegerLiteral(const Token& token, const std::string& context) {
    try {
        std::size_t parsed = 0;
        const auto value = DiscoNumeric::parse(token.lexeme, &parsed, 0);
        if (parsed != token.lexeme.size()) throw std::invalid_argument("trailing characters");
        return value;
    } catch (const std::exception&) {
        throw CompilerError("Invalid integer literal for " + context + ".",
                            token);
    }
}

} // namespace

Analyzer::Analyzer(DataSegmentManager& dataManager, TargetKind target) : m_target(target), m_data_manager(dataManager) {}

void Analyzer::requireCapability(TargetCapability capability, const Token& source) const {
    if (!supportsCapability(m_target, capability))
        throw CompilerError(std::string("Target '") + targetName(m_target) + "' lacks capability '" + capabilityName(capability) + "'.", source);
}

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
    if (m_type_alias_names.count(stmt.token.lexeme)) throw CompilerError("Value name conflicts with a type alias.", stmt.token);
    if (m_pending_constants.count(stmt.token.lexeme) || m_named_constants.count(stmt.token.lexeme))
        throw CompilerError("Function name conflicts with a compile-time constant.", stmt.token);
    stmt.link_name = stmt.linkage == Linkage::Internal ? std::string(1, '\x01') + stmt.token.lexeme : stmt.token.lexeme;
    if (stmt.linkage == Linkage::Internal && stmt.token.lexeme == "main")
        throw CompilerError("The main entry must have external linkage.", stmt.token);
    normalizeType(stmt.returnType, stmt.token);
    if (stmt.returnType.base == BaseType::STRUCT && stmt.returnType.pointer_level == 0)
        throw CompilerError("Struct return by value requires an aggregate ABI; use a pointer parameter.", stmt.token);
    FunctionSymbol symbol;
    symbol.returnType = stmt.returnType;
    symbol.link_name = stmt.link_name;
    symbol.linkage = stmt.linkage;
    for (auto& param : stmt.params) {
        if (m_type_alias_names.count(param.name.lexeme)) throw CompilerError("Parameter name conflicts with a type alias.", param.name);
        normalizeType(param.type, param.name);
        if (param.type.base == BaseType::STRUCT && param.type.pointer_level == 0)
            throw CompilerError("Struct parameter by value requires an aggregate ABI; pass a pointer.", param.name);
        symbol.paramTypes.push_back(param.type);
    }

    const auto existing = m_function_symbols.find(stmt.token.lexeme);
    if (existing == m_function_symbols.end()) {
        symbol.has_definition = !stmt.is_prototype;
        m_function_symbols.emplace(stmt.token.lexeme, std::move(symbol));
        return;
    }

    if (existing->second.linkage != symbol.linkage || !sameType(existing->second.returnType, symbol.returnType) ||
        existing->second.paramTypes.size() != symbol.paramTypes.size()) {
        throw CompilerError("Conflicting declaration of function '" + stmt.token.lexeme + "'.",
                            stmt.token);
    }
    for (std::size_t i = 0; i < symbol.paramTypes.size(); ++i) {
        if (!sameType(existing->second.paramTypes[i], symbol.paramTypes[i])) {
            throw CompilerError("Conflicting parameter type in declaration of function '" +
                                    stmt.token.lexeme + "'.",
                                stmt.params[i].name.line_number, stmt.params[i].name.col_number);
        }
    }
    if (!stmt.is_prototype && existing->second.has_definition) {
        throw CompilerError("Function '" + stmt.token.lexeme + "' already has a definition.",
                            stmt.token);
    }
    existing->second.has_definition = existing->second.has_definition || !stmt.is_prototype;
}

bool containsReturnStatement(const Stmt& statement) {
    if (dynamic_cast<const ReturnStmt*>(&statement)) return true;
    if (const auto* block = dynamic_cast<const BlockStmt*>(&statement)) {
        for (const auto& child : block->statements) {
            if (containsReturnStatement(*child)) return true;
        }
    } else if (const auto* conditional = dynamic_cast<const IfStmt*>(&statement)) {
        return containsReturnStatement(*conditional->thenBranch) ||
               (conditional->elseBranch && containsReturnStatement(*conditional->elseBranch));
    } else if (const auto* loop = dynamic_cast<const WhileStmt*>(&statement)) {
        return containsReturnStatement(*loop->body);
    } else if (const auto* loop = dynamic_cast<const ForStmt*>(&statement)) {
        return containsReturnStatement(*loop->body);
    } else if (const auto* switch_stmt = dynamic_cast<const SwitchStmt*>(&statement)) {
        return containsReturnStatement(*switch_stmt->body);
    } else if (const auto* plot = dynamic_cast<const PlotBlockStmt*>(&statement)) {
        return containsReturnStatement(*plot->body);
    }
    return false;
}

bool containsPlotBoundary(const Stmt& statement) {
    if (dynamic_cast<const PlotBeginStmt*>(&statement) ||
        dynamic_cast<const PlotEndStmt*>(&statement)) return true;
    if (const auto* block = dynamic_cast<const BlockStmt*>(&statement)) {
        for (const auto& child : block->statements) {
            if (containsPlotBoundary(*child)) return true;
        }
    } else if (const auto* conditional = dynamic_cast<const IfStmt*>(&statement)) {
        return containsPlotBoundary(*conditional->thenBranch) ||
               (conditional->elseBranch && containsPlotBoundary(*conditional->elseBranch));
    } else if (const auto* loop = dynamic_cast<const WhileStmt*>(&statement)) {
        return containsPlotBoundary(*loop->body);
    } else if (const auto* loop = dynamic_cast<const ForStmt*>(&statement)) {
        return containsPlotBoundary(*loop->body);
    } else if (const auto* switch_stmt = dynamic_cast<const SwitchStmt*>(&statement)) {
        return containsPlotBoundary(*switch_stmt->body);
    }
    return false;
}

void Analyzer::registerStructSymbol(StructDefStmt& stmt) {
    if (m_struct_symbols.count(stmt.token.lexeme)) {
        throw CompilerError("Struct '" + stmt.token.lexeme + "' already defined.", stmt.token);
    }

    StructSymbol struct_symbol;
    struct_symbol.name = stmt.token.lexeme;
    int current_offset = 0;
    bool packed = false;
    int explicit_alignment = 0;
    for (const auto& attribute : stmt.attributes) {
        if (attribute.name.lexeme == "packed") packed = true;
        if (attribute.name.lexeme == "align") {
            analyzeExpr(*attribute.arguments.front(), nullptr);
            std::int64_t alignment = 0;
            if (!ConstantEvaluator::evaluate(*attribute.arguments.front(), alignment) ||
                alignment < 1 || alignment > 128 || (alignment & (alignment - 1)) != 0)
                throw CompilerError("@align requires a power of two in 1..128.", attribute.name);
            explicit_alignment = static_cast<int>(alignment);
        }
    }
    struct_symbol.alignment = explicit_alignment ? explicit_alignment : packed ? 1 : 2;

    for (auto& member : stmt.members) {
        if (struct_symbol.members.count(member.name.lexeme)) {
            throw CompilerError("Duplicate member '" + member.name.lexeme + "' in struct '" + stmt.token.lexeme + "'.", member.name);
        }
        
        resolveExtent(member.type, member.array_extent);
        normalizeType(member.type, member.name);
        const int alignment = packed ? 1 : std::max(2, typeAlignment(member.type));
        current_offset = ((current_offset + alignment - 1) / alignment) * alignment;
        if (member.type.sizeInBytes < 0 ||
            current_offset > std::numeric_limits<int>::max() - member.type.sizeInBytes) {
            throw CompilerError("Struct layout exceeds the supported size limit.",
                                member.name);
        }
        
        struct_symbol.member_order.push_back(member.name.lexeme);
        struct_symbol.members[member.name.lexeme] = {member.type, current_offset};
        const auto elements = member.type.array_size > 0 ? member.type.array_size : 1;
        if (elements > 65536 / std::max(1, member.type.sizeInBytes))
            throw CompilerError("Struct member array exceeds one bank.", member.name);
        current_offset += member.type.sizeInBytes * elements;
        if (current_offset > 65536) throw CompilerError("Struct layout exceeds one bank.", member.name);
    }
    
    struct_symbol.totalSize = ((current_offset + struct_symbol.alignment - 1) / struct_symbol.alignment) * struct_symbol.alignment;
    m_struct_symbols[stmt.token.lexeme] = struct_symbol;
}

void Analyzer::registerRomSymbol(ConstDataStmt& stmt) {
    if (m_named_constants.count(stmt.token.lexeme) || m_pending_constants.count(stmt.token.lexeme))
        throw CompilerError("ROM storage conflicts with a compile-time constant.", stmt.token);
    if (m_data_manager.hasSymbol(stmt.token.lexeme) || m_function_symbols.count(stmt.token.lexeme)) {
         throw CompilerError("ROM symbol '" + stmt.token.lexeme + "' already declared.", stmt.token);
    }
    if (stmt.type.pointer_level > 0)
        throw CompilerError("ROM pointer constants require a typed pointer initializer and are not supported yet.", stmt.token);
    normalizeType(stmt.type, stmt.token);
    resolveExtent(stmt.type, stmt.array_extent);
    if (stmt.is_extern) { m_data_manager.add(stmt); return; }
    if (stmt.type.array_size > 0) {
        if (stmt.initializers.size() > static_cast<std::size_t>(stmt.type.array_size)) throw CompilerError("ROM initializer count exceeds extent.", stmt.token);
        while (stmt.initializers.size() < static_cast<std::size_t>(stmt.type.array_size))
            stmt.initializers.push_back(std::make_unique<LiteralExpr>(Token(TokenType::LITERAL_INTEGER, "0", stmt.token)));
    }
    auto scalar_type = stmt.type; scalar_type.array_size = 0;
    for (auto& initializer : stmt.initializers) {
        analyzeExpr(*initializer, &scalar_type);
        coerceExpr(initializer, scalar_type);
        std::int64_t value = 0;
        if (!ConstantEvaluator::evaluate(*initializer, value))
            throw CompilerError("ROM data requires a constant expression.", initializer->token);
        auto folded = std::make_unique<LiteralExpr>(Token(TokenType::LITERAL_INTEGER, std::to_string(value), initializer->token));
        folded->result_type = scalar_type;
        initializer = std::move(folded);
    }
    m_data_manager.add(stmt);
}

void Analyzer::registerGlobalSymbol(VarDeclStmt& stmt) {
    if (m_function_symbols.count(stmt.token.lexeme) || (m_named_constants.count(stmt.token.lexeme) && !m_pending_constants.count(stmt.token.lexeme)))
        throw CompilerError("Global name conflicts with a function or enumerator.", stmt.token);

    if (stmt.inferred_extent && !stmt.initializer) throw CompilerError("Inferred array extent requires an initializer.", stmt.token);
    stmt.link_name = stmt.linkage == Linkage::Internal ? std::string(1, '\x01') + stmt.token.lexeme : stmt.token.lexeme;
    if (stmt.is_extern && stmt.initializer && !stmt.is_imported)
        throw CompilerError("An extern global cannot have an initializer.", stmt.token);
    resolveExtent(stmt.type, stmt.array_extent);
    normalizeType(stmt.type, stmt.token);
    if (stmt.is_constexpr) {
        if (stmt.is_extern || !stmt.initializer || stmt.type.pointer_level > 0 || stmt.type.array_size > 0 ||
            stmt.type.is_volatile || (stmt.type.base != BaseType::BOOL && stmt.type.base != BaseType::BYTE && stmt.type.base != BaseType::WORD))
            throw CompilerError("constexpr requires a scalar integer initializer and no storage qualifiers.", stmt.token);
        resolveConstant(stmt.token.lexeme);
        return;
    }
    const auto existing = m_data_manager.getEntries().find(stmt.token.lexeme);
    if (m_function_symbols.count(stmt.token.lexeme))
        throw CompilerError("Duplicate global symbol '" + stmt.token.lexeme + "'.", stmt.token);
    if (existing != m_data_manager.getEntries().end()) {
        const auto& declaration = existing->second;
        if (declaration.storage != AddressSpace::RAM ||
            (!declaration.is_extern && !stmt.is_extern))
            throw CompilerError("Duplicate global symbol '" + stmt.token.lexeme + "'.", stmt.token);
        if (!sameType(declaration.type, stmt.type) || declaration.type.is_const != stmt.type.is_const ||
            declaration.type.is_volatile != stmt.type.is_volatile || declaration.link_name != stmt.link_name)
            throw CompilerError("Conflicting global declaration '" + stmt.token.lexeme + "'.", stmt.token);
    }
    if (stmt.type.is_const && !stmt.initializer && !stmt.is_extern)
        throw CompilerError("A const global requires an initializer.", stmt.token);
    if (stmt.initializer && (stmt.type.array_size > 0 || stmt.inferred_extent || (stmt.type.base == BaseType::STRUCT && stmt.type.pointer_level == 0)))
        prepareInitializer(stmt);
    if (stmt.initializer) {
        if (stmt.type.array_size > 0 || (stmt.type.base == BaseType::STRUCT && stmt.type.pointer_level == 0))
            throw CompilerError("Aggregate global initializers are not supported; zero initialization is automatic.", stmt.token);
        analyzeExpr(*stmt.initializer, &stmt.type);
        coerceExpr(stmt.initializer, stmt.type);
    }
    stmt.symbol_id = existing == m_data_manager.getEntries().end() ? createSymbolId() : existing->second.id;
    m_data_manager.add(stmt);
}

void Analyzer::beginScope() {
    m_scopes.emplace_back();
}

void Analyzer::endScope() {
    m_scopes.pop_back();
}

void Analyzer::analyze(const std::vector<std::unique_ptr<Stmt>>& program) {
    m_bitmaps.clear(); m_registered_bitmaps.clear(); m_selected_bitmap = BitmapConfig{};
    for (const auto& statement : program)
        if (auto* bitmap = dynamic_cast<BitmapDeclStmt*>(statement.get())) visit(*bitmap);
    m_type_alias_names.clear();
    for (const auto& binding : builtinTypeAliases()) m_type_alias_names.insert(binding.first);
    for (const auto& statement : program)
        if (const auto* alias = dynamic_cast<const TypeAliasDeclStmt*>(statement.get()))
            if (!m_type_alias_names.insert(alias->token.lexeme).second)
                throw CompilerError("Type alias already declared or reserved.", alias->token);
    for (const auto& statement : program)
        if ((dynamic_cast<const FunctionDeclStmt*>(statement.get()) ||
             dynamic_cast<const VarDeclStmt*>(statement.get()) ||
             dynamic_cast<const ConstDataStmt*>(statement.get())) &&
            m_type_alias_names.count(statement->token.lexeme))
            throw CompilerError("Value name conflicts with a type alias.", statement->token);
    m_scopes.clear();
    m_function_symbols.clear();
    m_struct_symbols.clear();
    m_all_local_symbols.clear();
    m_next_symbol_id = 1;
    m_break_context_stack.clear();
    m_case_values_stack.clear();
    m_pending_constants.clear();
    m_named_constants.clear();
    m_local_constants.clear();
    m_constexpr_ids.clear();
    m_loop_depth = 0;
    m_evaluating_constants.clear();
    m_enum_types.clear();
    m_registered_enums.clear();
    for (const auto& statement : program) {
        auto* declaration = dynamic_cast<VarDeclStmt*>(statement.get());
        if (declaration && declaration->type.is_const && !declaration->type.is_volatile &&
            (declaration->type.base == BaseType::BYTE || declaration->type.base == BaseType::WORD || declaration->type.base == BaseType::BOOL) && declaration->type.pointer_level == 0 && declaration->type.array_size == 0 && !declaration->inferred_extent && !declaration->array_extent && declaration->initializer)
            if (!m_pending_constants.emplace(declaration->token.lexeme, declaration).second)
                throw CompilerError("Duplicate compile-time constant.", declaration->token);
    }
    m_isInPlottingContext = false;
    m_next_local_stack_offset = 0;

    for (const auto& stmt : program)
        if (auto* s = dynamic_cast<StructDefStmt*>(stmt.get())) registerStructSymbol(*s);
        else if (auto* enumeration = dynamic_cast<EnumDeclStmt*>(stmt.get())) visit(*enumeration);
    for (const auto& stmt : program)
        if (auto* func = dynamic_cast<FunctionDeclStmt*>(stmt.get())) registerFunctionSymbol(*func);
    for (const auto& stmt : program)
        if (auto* data = dynamic_cast<ConstDataStmt*>(stmt.get())) registerRomSymbol(*data);
    for (const auto& stmt : program)
        if (auto* global = dynamic_cast<VarDeclStmt*>(stmt.get())) registerGlobalSymbol(*global);
    for (const auto& stmt : program) {
        stmt->accept(*this);
    }
    for (const auto& function : m_function_symbols)
        if (function.second.linkage == Linkage::Internal && !function.second.has_definition)
            throw CompilerError("Internal function must be defined in this compilation unit: " + function.first, 1, 1);
}

void Analyzer::analyzeExpr(Expr& expr, const Type* context) {
    expr.is_constant = false;
    expr.accept(*this, context);
    std::int64_t value = 0;
    if (!expr.result_type.is_volatile && ConstantEvaluator::evaluate(expr, value)) {
        expr.is_constant = true;
        expr.constant_value = value;
    }
}

int Analyzer::typeAlignment(const Type& type) const {
    if (type.pointer_level > 0) return 2;
    if (type.base == BaseType::STRUCT) return m_struct_symbols.at(type.structName).alignment;
    return type.sizeInBytes == 1 ? 1 : 2;
}

void Analyzer::resolveExtent(Type& type, std::unique_ptr<Expr>& expression) {
    if (!expression) return;
    analyzeExpr(*expression, nullptr);
    std::int64_t size = 0;
    if (expression->result_type.pointer_level != 0 || !ConstantEvaluator::evaluate(*expression, size))
        throw CompilerError("Array size must be an integer constant expression.", expression->token);
    if (size < 1 || size > 1000000)
        throw CompilerError("Array size must be between 1 and 1000000.", expression->token);
    type.array_size = static_cast<int>(size);
}

void Analyzer::resolveConstant(const std::string& name) {
    if (m_named_constants.count(name)) return;
    const auto found = m_pending_constants.find(name);
    if (found == m_pending_constants.end()) return;
    auto& declaration = *found->second;
    if (m_evaluating_constants.size() >= 128 || !m_evaluating_constants.insert(name).second)
        throw CompilerError("Cyclic constant expression or dependency limit exceeded.", declaration.token);
    normalizeType(declaration.type, declaration.token);
    analyzeExpr(*declaration.initializer, &declaration.type);
    coerceExpr(declaration.initializer, declaration.type);
    std::int64_t value = 0;
    if (!ConstantEvaluator::evaluate(*declaration.initializer, value))
        throw CompilerError("Compile-time constant requires a constant expression.", declaration.token);
    m_named_constants.emplace(name, ConstantSymbol{valueType(declaration.type), value});
    m_evaluating_constants.erase(name);
}

void Analyzer::visit(EnumDeclStmt& stmt) {
    if (m_registered_enums.count(&stmt)) return;
    if (m_enum_types.count(stmt.token.lexeme)) throw CompilerError("Enum already defined.", stmt.token);
    m_registered_enums.insert(&stmt);
    normalizeType(stmt.underlying_type, stmt.token);
    m_enum_types.emplace(stmt.token.lexeme, stmt.underlying_type);
    std::int64_t next = 0;
    for (auto& entry : stmt.entries) {
        if (m_type_alias_names.count(entry.name.lexeme))
            throw CompilerError("Enumerator name conflicts with a type alias.", entry.name);
        if (m_named_constants.count(entry.name.lexeme) || m_pending_constants.count(entry.name.lexeme))
            throw CompilerError("Duplicate compile-time constant '" + entry.name.lexeme + "'.", entry.name);
        auto value = next;
        if (entry.value) {
            analyzeExpr(*entry.value, &stmt.underlying_type);
            coerceExpr(entry.value, stmt.underlying_type);
            if (!ConstantEvaluator::evaluate(*entry.value, value))
                throw CompilerError("Enumerator requires a constant expression.", entry.name);
        }
        const auto maximum = stmt.underlying_type.base == BaseType::BYTE
            ? (stmt.underlying_type.is_unsigned ? 255 : 127) : (stmt.underlying_type.is_unsigned ? 65535 : 32767);
        const auto minimum = stmt.underlying_type.is_unsigned ? 0 : stmt.underlying_type.base == BaseType::BYTE ? -128 : -32768;
        if (value < minimum || value > maximum)
            throw CompilerError("Enumerator value exceeds its underlying type.", entry.name);
        m_named_constants.emplace(entry.name.lexeme, ConstantSymbol{stmt.underlying_type, value});
        next = value + 1;
    }
}

void Analyzer::visit(TypeAliasDeclStmt& stmt) { normalizeType(stmt.resolved_type, stmt.token); }

void Analyzer::visit(StaticAssertStmt& stmt) {
    analyzeExpr(*stmt.condition, nullptr);
    std::int64_t value = 0;
    if (!ConstantEvaluator::evaluate(*stmt.condition, value))
        throw CompilerError("static_assert requires a constant expression.", stmt.token);
    if (value == 0) throw CompilerError("Static assertion failed.", stmt.token);
}

void Analyzer::visit(LayoutQueryExpr& expr, const Type*) {
    Type type = expr.queried_type;
    if (expr.queried_expression) {
        // Unevaluated operand: type-check only. Lowering emits no hardware
        // loads, stores or calls for sizeof/alignof.
        analyzeExpr(*expr.queried_expression, nullptr);
        type = expr.queried_expression->result_type;
        if (const auto* variable = dynamic_cast<const VariableExpr*>(expr.queried_expression.get()))
            if (variable->is_array_decay && m_data_manager.hasSymbol(variable->token.lexeme))
                type = m_data_manager.getSymbolType(variable->token.lexeme);
    }
    normalizeType(type, expr.token);
    std::int64_t value = 0;
    if (expr.token.type == TokenType::KEYWORD_OFFSETOF) {
        if (type.pointer_level != 0 || type.base != BaseType::STRUCT)
            throw CompilerError("offsetof requires a struct type.", expr.token);
        const auto& structure = m_struct_symbols.at(type.structName);
        const auto member = structure.members.find(expr.member.lexeme);
        if (member == structure.members.end()) throw CompilerError("Unknown offsetof member.", expr.member);
        value = member->second.offset;
    } else if (expr.token.type == TokenType::KEYWORD_ALIGNOF) value = typeAlignment(type);
    else value = static_cast<std::int64_t>(type.sizeInBytes) * (type.array_size > 0 ? type.array_size : 1);
    if (value <= 0 && expr.token.type != TokenType::KEYWORD_OFFSETOF)
        throw CompilerError("Layout query requires a complete non-void type.", expr.token);
    if (value > 65535) throw CompilerError("Layout query exceeds an unsigned word.", expr.token);
    expr.result_type = {BaseType::WORD, "", 2, true};
    expr.is_constant = true;
    expr.constant_value = value;
}

void Analyzer::normalizeType(Type& type, const Token& source) {
    if (!type.enum_name.empty()) {
        const auto found = m_enum_types.find(type.enum_name);
        if (found == m_enum_types.end()) throw CompilerError("Unknown enum type.", source);
        type.base = found->second.base;
        type.is_unsigned = found->second.is_unsigned;
        type.enum_name.clear();
    }
    if (type.is_unsigned && type.base != BaseType::BYTE && type.base != BaseType::WORD)
        throw CompilerError("Unsigned is only valid with byte or word types.", source);
    if (type.pointer_level < 0) {
        throw CompilerError("Invalid negative pointer depth.", source);
    }
    if (type.pointer_level > 0) {
        normalizePointerLayers(type);
        if (std::find(type.pointer_reach.begin(), type.pointer_reach.end(), true) != type.pointer_reach.end())
            requireCapability(TargetCapability::FarData, source);
        if (type.base == BaseType::STRUCT) {
            const auto structure = m_struct_symbols.find(type.structName);
            if (structure != m_struct_symbols.end()) {
                type.alignment = structure->second.alignment;
                type.aggregate_size = structure->second.totalSize;
            }
        }
        return;
    }
    switch (type.base) {
        case BaseType::BYTE:
        case BaseType::BOOL:
            type.sizeInBytes = 1;
            break;
        case BaseType::WORD:
            type.sizeInBytes = 2;
            break;
        case BaseType::VOID:
            type.sizeInBytes = 0;
            break;
        case BaseType::STRUCT: {
            const auto found = m_struct_symbols.find(type.structName);
            if (found == m_struct_symbols.end()) {
                throw CompilerError("Unknown struct type 'struct " + type.structName + "'.",
                                    source);
            }
            type.sizeInBytes = found->second.totalSize;
            type.aggregate_size = found->second.totalSize;
            type.alignment = found->second.alignment;
            break;
        }
        default:
            throw CompilerError("Invalid incomplete type.", source);
    }
}

bool Analyzer::sameType(const Type& left, const Type& right) const {
    return left.base == right.base &&
           left.structName == right.structName &&
           left.is_unsigned == right.is_unsigned &&
           left.pointer_level == right.pointer_level &&
           samePointerLayers(left, right) &&
           left.array_size == right.array_size &&
           (left.pointer_level == 0 || left.space == right.space);
}

bool Analyzer::canImplicitlyConvert(const Type& source, const Type& target) const {
    if (sameType(source, target)) return true;
    if (source.pointer_level > 0 && source.pointer_level == target.pointer_level) {
        Type qualified = source;
        Type destination = target;
        normalizePointerLayers(qualified);
        normalizePointerLayers(destination);
        const auto from = qualified.pointee_qualifiers.back();
        const auto to = destination.pointee_qualifiers.back();
        if ((!from.is_const || to.is_const) && (!from.is_volatile || to.is_volatile)) {
            qualified.pointee_qualifiers.back() = to;
            normalizePointerLayers(qualified);
            if (sameType(qualified, destination)) return true;
        }
    }
    if (source.pointer_level != 0 || target.pointer_level != 0 ||
        source.structName != target.structName ||
        source.is_far != target.is_far ||
        (source.pointer_level != 0 && source.space != target.space) ||
        source.array_size != target.array_size) {
        return false;
    }
    if ((target.base == BaseType::BOOL && (source.base == BaseType::BYTE || source.base == BaseType::WORD)) ||
        (source.base == BaseType::BOOL && (target.base == BaseType::BYTE || target.base == BaseType::WORD))) return true;
    return source.is_unsigned == target.is_unsigned &&
           source.base == BaseType::BYTE && target.base == BaseType::WORD;
}

void Analyzer::coerceExpr(std::unique_ptr<Expr>& expression, const Type& target) {
    if (dynamic_cast<NullExpr*>(expression.get()) && target.pointer_level > 0) { analyzeExpr(*expression, &target); return; }
    const Type source = expression->result_type;
    if (sameType(source, target)) return;
    if (!canImplicitlyConvert(source, target)) {
        throw CompilerError("Cannot implicitly convert '" + to_string(source) +
                                "' to '" + to_string(target) + "'.",
                            expression->token);
    }
    auto converted = std::make_unique<CastExpr>(expression->token, target, std::move(expression));
    converted->implicit_conversion = true;
    converted->result_type = target;
    expression = std::move(converted);
}

bool Analyzer::isAssignableLValue(const Expr& expression) const {
    return dynamic_cast<const PlotCoordinateExpr*>(&expression) != nullptr ||
           dynamic_cast<const VariableExpr*>(&expression) != nullptr ||
           dynamic_cast<const DereferenceExpr*>(&expression) != nullptr ||
           dynamic_cast<const SubscriptExpr*>(&expression) != nullptr ||
           dynamic_cast<const MemberAccessExpr*>(&expression) != nullptr;
}

void Analyzer::visit(FunctionDeclStmt& stmt) {
    if (stmt.is_cached) requireCapability(TargetCapability::InstructionCache, stmt.token);
    if (stmt.is_prototype) return;
    m_isInPlottingContext = false;
    m_lexical_plot_context = false;

    // Create a new local symbol table for this function in our main map.
    m_current_function_locals = &m_all_local_symbols[stmt.token.lexeme];
    m_current_function_locals->clear();
    
    m_scopes.clear();
    m_currentFunctionType = stmt.returnType;
    normalizeType(m_currentFunctionType, stmt.token);
    m_next_local_stack_offset = 0;
    beginScope();

    // STEP 1: Process parameters. They have POSITIVE offsets from the frame pointer.
    // Empty descending stack: [FP+2]=Old_FP, [FP+4]=ReturnAddr, [FP+6]=Param1.
    int paramOffset = GSUAbi::FirstParameterOffset;
    for (auto& param : stmt.params) {
        if (m_scopes.back().count(param.name.lexeme)) {
            throw CompilerError("Duplicate parameter name '" + param.name.lexeme + "'.", param.name);
        }

        normalizeType(param.type, param.name);

        const auto symbol_id = createSymbolId();
        Symbol symbol{param.type, paramOffset, "", symbol_id};
        m_current_function_locals->emplace(symbol_id, symbol);
        param.symbol_id = symbol_id;
        m_scopes.back()[param.name.lexeme] = symbol_id;
        paramOffset += isFarPointer(param.type) ? 4 : static_cast<int>(GSUAbi::ParameterSlotSize);
        if (paramOffset > 65528) throw CompilerError("Function parameter area exceeds one bank.",
                                                    param.name);
    }

    // STEP 2: Process the function body to find local variables and their sizes.
    // We will calculate their NEGATIVE offsets from the frame pointer.
    auto bodyBlock = std::make_unique<BlockStmt>(std::move(stmt.body));
    bodyBlock->accept(*this);
    stmt.body = std::move(bodyBlock->statements);
    endScope();
    if (m_isInPlottingContext)
        throw CompilerError("Unclosed plotting context; use plot { ... }.", stmt.token);

    stmt.needs_implicit_return = blockCanFallThrough(stmt.body);
    if (!isVoidValue(stmt.returnType) && stmt.needs_implicit_return) {
        throw CompilerError(
            "Non-void function may reach the end without returning a value.",
            stmt.token);
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
    // The next PUSH writes at SP before decrementing it. Keep an empty word
    // below the lowest local so expression temporaries cannot overwrite it.
    constexpr int max_local_storage = 65528;
    if (total_local_size > max_local_storage) {
        throw CompilerError("Local stack frame exceeds the 16-bit GSU address space.",
                            stmt.token);
    }
    total_local_size = -m_next_local_stack_offset;
    if (total_local_size > max_local_storage) throw CompilerError("Local stack frame exceeds the 16-bit GSU address space.", stmt.token);
    stmt.total_local_alloc_size = total_local_size > 0
        ? total_local_size + static_cast<int>(GSUAbi::ParameterSlotSize) : 0;
    
    // Reset the pointer for the next function.
    m_current_function_locals = nullptr;
}

void Analyzer::visit(BlockStmt& stmt) {
    beginScope();

    for (const auto& s : stmt.statements) {
        if (auto* varDecl = dynamic_cast<VarDeclStmt*>(s.get())) {
            // One monotonically decreasing cursor is shared by all lexical
            // scopes.  Recomputing an offset from the parent scope would make
            // sibling blocks reuse the same stack slots.
            varDecl->base_stack_offset = m_next_local_stack_offset;
            varDecl->accept(*this);
            m_next_local_stack_offset = m_current_function_locals->at(varDecl->symbol_id).stackOffset;
        } else {
            s->accept(*this);
        }
    }
    endScope();
}

void Analyzer::visit(VarDeclStmt& stmt) {
    if (m_type_alias_names.count(stmt.token.lexeme)) throw CompilerError("Value name conflicts with a type alias.", stmt.token);
    if (stmt.inferred_extent && !stmt.initializer) throw CompilerError("Inferred array extent requires an initializer.", stmt.token);
    if (stmt.is_global) return;
    if (m_scopes.back().count(stmt.token.lexeme)) {
        throw CompilerError("Symbol '" + stmt.token.lexeme + "' already declared in this scope.", stmt.token);
    }
    if (m_data_manager.hasSymbol(stmt.token.lexeme) || m_function_symbols.count(stmt.token.lexeme)) {
        throw CompilerError("Symbol '" + stmt.token.lexeme + "' already declared globally.", stmt.token);
    }

    resolveExtent(stmt.type, stmt.array_extent);
    normalizeType(stmt.type, stmt.token);

    if (stmt.type.is_const && !stmt.initializer)
        throw CompilerError("A const local requires an initializer.", stmt.token);
    if (stmt.type.pointer_level == 0 && stmt.type.space == AddressSpace::ROM)
        throw CompilerError("Local storage resides in RAM; declare ROM data globally.", stmt.token);

    if (stmt.initializer && (stmt.type.array_size > 0 || stmt.inferred_extent || (stmt.type.base == BaseType::STRUCT && stmt.type.pointer_level == 0)))
        prepareInitializer(stmt);
    if (stmt.initializer) {
        if (stmt.type.base == BaseType::STRUCT && stmt.type.pointer_level == 0)
            throw CompilerError("Struct initialization by value is not supported; initialize members.", stmt.token);
        analyzeExpr(*stmt.initializer, &stmt.type);
        coerceExpr(stmt.initializer, stmt.type);
    }
    
    int size_to_allocate = ((stmt.type.sizeInBytes + 1) / 2) * 2;
    if (stmt.type.array_size > 0) {
        int element_size = stmt.type.sizeInBytes;
        if (stmt.type.array_size > 65536 / element_size) {
            throw CompilerError("Array allocation exceeds the supported size limit.",
                                stmt.token);
        }
        size_to_allocate = ((stmt.type.array_size * element_size + 1) / 2) * 2;
    }

    // Calculate the new negative offset from the FP.
    if (stmt.is_constexpr && (!stmt.initializer || stmt.type.pointer_level > 0 || stmt.type.array_size > 0 || stmt.type.is_volatile ||
        (stmt.type.base != BaseType::BOOL && stmt.type.base != BaseType::BYTE && stmt.type.base != BaseType::WORD)))
        throw CompilerError("constexpr requires a scalar integer initializer.", stmt.token);
    if (stmt.is_constexpr) size_to_allocate = 0;
    const int extra_alignment = stmt.is_constexpr ? 0 : std::max(2, typeAlignment(stmt.type)) - 2;
    size_to_allocate += extra_alignment;
    if (size_to_allocate > 65528 || m_next_local_stack_offset < -65528 + size_to_allocate)
        throw CompilerError("Local stack frame exceeds the 16-bit GSU address space.", stmt.token);
    stmt.base_stack_offset = m_next_local_stack_offset;
    int final_offset = stmt.base_stack_offset - size_to_allocate;
    m_next_local_stack_offset = final_offset;

    if (!m_current_function_locals) throw std::runtime_error("Internal Analyzer Error: Not in a function context.");
    const auto symbol_id = createSymbolId();
    Symbol symbol{stmt.type, final_offset, "", symbol_id};
    m_current_function_locals->emplace(symbol_id, symbol);
    stmt.symbol_id = symbol_id;
    if (stmt.is_constexpr) m_constexpr_ids.insert(symbol_id);
    m_scopes.back()[stmt.token.lexeme] = symbol_id;
    if (stmt.type.is_const && !stmt.type.is_volatile && stmt.initializer && stmt.type.pointer_level == 0) {
        std::int64_t value = 0;
        if (ConstantEvaluator::evaluate(*stmt.initializer, value)) m_local_constants.emplace(symbol_id, value);
        else if (stmt.is_constexpr) throw CompilerError("constexpr requires a constant expression.", stmt.token);
    }
}

void Analyzer::visit(StructDefStmt&) {}

void Analyzer::visit(ConstDataStmt&) {}

void Analyzer::visit(ReturnStmt& stmt) {
    if (stmt.value) {
        if (isVoidValue(m_currentFunctionType)) {
            throw CompilerError("Cannot return a value from a void function.", stmt.token);
        }
        analyzeExpr(*stmt.value, &m_currentFunctionType);
        coerceExpr(stmt.value, m_currentFunctionType);
    } else {
        if (!isVoidValue(m_currentFunctionType)) {
            throw CompilerError("Non-void function must return a value.", stmt.token);
        }
    }
}

void Analyzer::visit(IfStmt& stmt) {
    analyzeExpr(*stmt.condition, nullptr);
    coerceExpr(stmt.condition, Type{BaseType::BOOL, "", 1, false});
    const bool incoming_plot_context = m_isInPlottingContext;
    stmt.thenBranch->accept(*this);
    const bool then_plot_context = m_isInPlottingContext;
    m_isInPlottingContext = incoming_plot_context;
    if (stmt.elseBranch) {
        stmt.elseBranch->accept(*this);
        if (m_isInPlottingContext != then_plot_context) {
            throw CompilerError("Plotting context must be balanced consistently across both branches.",
                                stmt.token);
        }
    } else if (then_plot_context != incoming_plot_context) {
        throw CompilerError("A plotting context cannot begin or end on only one branch.",
                            stmt.token);
    }
    m_isInPlottingContext = stmt.elseBranch ? then_plot_context : incoming_plot_context;
}

void Analyzer::visit(WhileStmt& stmt) {
    if (stmt.is_cached) requireCapability(TargetCapability::InstructionCache, stmt.token);
    analyzeExpr(*stmt.condition, nullptr);
    coerceExpr(stmt.condition, Type{BaseType::BOOL, "", 1, false});
    const bool incoming_plot_context = m_isInPlottingContext;
    m_break_context_stack.push_back(false);
    ++m_loop_depth;
    stmt.body->accept(*this);
    m_break_context_stack.pop_back();
    --m_loop_depth;
    if (m_isInPlottingContext != incoming_plot_context) {
        throw CompilerError("A plotting context cannot cross a loop boundary.",
                            stmt.token);
    }
    m_isInPlottingContext = incoming_plot_context;
}

void Analyzer::visit(ForStmt& stmt) {
    if (stmt.is_cached) requireCapability(TargetCapability::InstructionCache, stmt.token);
    beginScope();
    if (stmt.initializer) stmt.initializer->accept(*this);
    analyzeExpr(*stmt.condition, nullptr);
    coerceExpr(stmt.condition, Type{BaseType::BOOL, "", 1, false});
    const bool plotting = m_isInPlottingContext;
    m_break_context_stack.push_back(false);
    ++m_loop_depth;
    stmt.body->accept(*this);
    if (stmt.increment) analyzeExpr(*stmt.increment, nullptr);
    --m_loop_depth;
    m_break_context_stack.pop_back();
    if (m_isInPlottingContext != plotting)
        throw CompilerError("A plotting context cannot cross a loop boundary.", stmt.token);
    endScope();
}
void Analyzer::visit(ContinueStmt& stmt) {
    if (m_loop_depth == 0)
        throw CompilerError("'continue' statement not within a loop.", stmt.token);
}
void Analyzer::visit(FallthroughStmt& stmt) {
    if (!m_fallthrough_marker_allowed)
        throw CompilerError("fallthrough must directly precede a switch label.", stmt.token);
}

void Analyzer::visit(HardwareLoopStmt& stmt) {
    requireCapability(TargetCapability::HardwareLoops, stmt.token);
    if (containsReturnStatement(*stmt.body)) {
        throw CompilerError("A hardware loop cannot contain a return statement.",
                            stmt.token);
    }
    const bool incoming_plot_context = m_isInPlottingContext;
    beginScope();
    stmt.body->accept(*this);
    endScope();
    if (m_isInPlottingContext != incoming_plot_context) {
        throw CompilerError("A plotting context cannot cross a hardware-loop boundary.",
                            stmt.token);
    }
    m_isInPlottingContext = incoming_plot_context;
}

void Analyzer::visit(ExpressionStmt& stmt) {
    analyzeExpr(*stmt.expression, nullptr);
    if (stmt.expression->result_type.base == BaseType::STRUCT && stmt.expression->result_type.pointer_level == 0)
        throw CompilerError("Struct values reside in memory; access members or take their address.", stmt.token);
}

void Analyzer::visit(PlotBlockStmt& stmt) {
    requireCapability(TargetCapability::Graphics, stmt.token);
    if (m_isInPlottingContext)
        throw CompilerError("Cannot nest plotting contexts.", stmt.token);
    m_isInPlottingContext = true;
    m_lexical_plot_context = true;
    try {
        stmt.body->accept(*this);
    } catch (...) {
        m_isInPlottingContext = false;
        m_lexical_plot_context = false;
        throw;
    }
    m_isInPlottingContext = false;
    m_lexical_plot_context = false;
}

void Analyzer::visit(PlotBeginStmt& stmt) {
    requireCapability(TargetCapability::Graphics, stmt.token);
    if (m_isInPlottingContext) {
        throw CompilerError("Cannot begin a new plotting context within another.", stmt.token);
    }
    m_isInPlottingContext = true;
}

void Analyzer::visit(PlotEndStmt& stmt) {
    if (m_lexical_plot_context)
        throw CompilerError("plot_end cannot close a lexical plot block.", stmt.token);
    if (!m_isInPlottingContext) {
        throw CompilerError("Cannot end a plotting context that has not been started.", stmt.token);
    }
    m_isInPlottingContext = false;
}

void Analyzer::visit(PlotStmt& stmt) {
    requirePlotContext(stmt.token);
}

void Analyzer::visit(SetColorStmt& stmt) {
    requirePlotContext(stmt.token);
    analyzeExpr(*stmt.color_value, nullptr);
    if (stmt.color_value->result_type.pointer_level != 0 || stmt.color_value->result_type.array_size != 0 ||
        (stmt.color_value->result_type.base != BaseType::BYTE && stmt.color_value->result_type.base != BaseType::WORD && stmt.color_value->result_type.base != BaseType::BOOL))
        throw CompilerError("Color requires a scalar integer value.", stmt.token);
}

void Analyzer::visit(CmodeStmt& stmt) {
    requirePlotContext(stmt.token);
    analyzeExpr(*stmt.options_value, nullptr);
    if (!stmt.options_value->is_constant || stmt.options_value->constant_value < 0 || stmt.options_value->constant_value > 31)
        throw CompilerError("Plot options must be a constant POR mask.", stmt.token);
}

void Analyzer::visit(RpixStmt& stmt) { requireCapability(TargetCapability::Graphics, stmt.token); }

void Analyzer::requirePlotContext(const Token& source) const {
    requireCapability(TargetCapability::Graphics, source);
    if (!m_isInPlottingContext) throw CompilerError("Graphics operation requires a plotting context (plot { ... }).", source);
}

void Analyzer::visit(ReadPixelExpr& expr, const Type*) {
    requirePlotContext(expr.token);
    if (expr.x) {
        const Type coordinate{BaseType::WORD, "", 2, false};
        analyzeExpr(*expr.x, &coordinate); coerceExpr(expr.x, coordinate);
        analyzeExpr(*expr.y, &coordinate); coerceExpr(expr.y, coordinate);
    }
    expr.result_type = Type{BaseType::BYTE, "", 1, false};
}

void Analyzer::visit(BitmapDeclStmt& stmt) {
    if (!m_registered_bitmaps.insert(&stmt).second) return;
    requireCapability(TargetCapability::Graphics, stmt.token);
    try { stmt.config.validate(); } catch (const std::exception& error) { throw CompilerError(error.what(), stmt.token); }
    if (m_bitmaps.size() >= 128) throw CompilerError("Bitmap declaration count exceeds 128.", stmt.token);
    const auto found = m_bitmaps.find(stmt.token.lexeme);
    if (found != m_bitmaps.end()) throw CompilerError("Duplicate bitmap declaration.", stmt.token);
    m_bitmaps.emplace(stmt.token.lexeme, stmt.config);
}

void Analyzer::visit(UseBitmapStmt& stmt) {
    requireCapability(TargetCapability::Graphics, stmt.token);
    const auto found = m_bitmaps.find(stmt.token.lexeme);
    if (found == m_bitmaps.end()) throw CompilerError("Unknown bitmap: " + stmt.token.lexeme, stmt.token);
    stmt.config = found->second;
    if (m_selected_bitmap.enabled && m_selected_bitmap != stmt.config)
        throw CompilerError("A GSU payload can select only one host bitmap configuration; runtime switching requires SNES-side configuration.", stmt.token);
    m_selected_bitmap = stmt.config;
}

void Analyzer::visit(CallExpr& expr, const Type*) {
    auto* callee_var = dynamic_cast<VariableExpr*>(expr.callee.get());
    if (!callee_var) {
        throw CompilerError("Can only call named functions.", expr.callee->token);
    }
    if (!m_function_symbols.count(callee_var->token.lexeme)) {
        throw CompilerError("Call to undeclared function '" + callee_var->token.lexeme + "'.", callee_var->token);
    }
    const FunctionSymbol& func = m_function_symbols.at(callee_var->token.lexeme);
    expr.resolved_symbol = func.link_name;
    if (expr.arguments.size() != func.paramTypes.size()) {
        throw CompilerError("Function '" + callee_var->token.lexeme + "' expects " +
                                 std::to_string(func.paramTypes.size()) + " arguments but got " +
                                 std::to_string(expr.arguments.size()) + ".", expr.paren);
    }
    for (size_t i = 0; i < expr.arguments.size(); ++i) {
        analyzeExpr(*expr.arguments[i], &func.paramTypes[i]);
        coerceExpr(expr.arguments[i], func.paramTypes[i]);
    }
    expr.result_type = func.returnType;
}

void Analyzer::visit(AssignExpr& expr, const Type*) {
    analyzeExpr(*expr.name, nullptr);
    if (!isAssignableLValue(*expr.name)) {
        throw CompilerError("Left-hand side of assignment must be an l-value.",
                            expr.name->token);
    }
    const Type& targetType = expr.name->result_type;
    if (targetType.array_size > 0)
        throw CompilerError("Whole-array assignment is not supported; assign elements instead.", expr.token);
    if (targetType.base == BaseType::STRUCT && targetType.pointer_level == 0)
        throw CompilerError("Struct assignment by value is not supported; assign members.", expr.token);

    // A local pointer to ROM is itself a writable RAM object. Read-only
    // applies to the addressed storage, not to the value's pointee qualifier.
    const auto* local = dynamic_cast<VariableExpr*>(expr.name.get());
    const bool local_pointer = local && local->symbol_id.isValid() && targetType.pointer_level > 0;
    if ((expr.name->address_type.pointer_level > 0 && expr.name->address_type.space == AddressSpace::ROM) ||
        (expr.name->address_type.pointer_level == 0 && targetType.space == AddressSpace::ROM && !local_pointer)) {
        throw CompilerError("Cannot assign to a 'rom' qualified type. ROM is read-only.", expr.token);
    }
    if (targetType.is_const)
        throw CompilerError("Cannot assign through a const-qualified access.", expr.token);
    if (dynamic_cast<PlotCoordinateExpr*>(expr.name.get())) {
        analyzeExpr(*expr.value, &targetType);
        coerceExpr(expr.value, targetType);
        expr.result_type = targetType;
        return;
    }

    if (expr.value) {
        analyzeExpr(*expr.value, &targetType);
        coerceExpr(expr.value, targetType);
    }
    expr.result_type = targetType;
}

void Analyzer::visit(InitializerListExpr& expr, const Type*) {
    throw CompilerError("Initializer list requires aggregate storage.", expr.token);
}
void Analyzer::visit(StringExpr& expr, const Type*) {
    throw CompilerError("String literal requires byte-array storage.", expr.token);
}
void Analyzer::prepareInitializer(VarDeclStmt& declaration) {
    if (auto* string = dynamic_cast<StringExpr*>(declaration.initializer.get())) {
        if (declaration.type.base != BaseType::BYTE || declaration.type.pointer_level != 0)
            throw CompilerError("String storage requires a byte array.", string->token);
        std::vector<std::unique_ptr<Expr>> bytes;
        for (const unsigned char byte : string->token.lexeme) {
            const int value = declaration.type.is_unsigned || byte < 128 ? byte : static_cast<int>(byte) - 256;
            bytes.push_back(std::make_unique<LiteralExpr>(Token(TokenType::LITERAL_INTEGER, std::to_string(value), string->token)));
        }
        bytes.push_back(std::make_unique<LiteralExpr>(Token(TokenType::LITERAL_INTEGER, "0", string->token)));
        declaration.initializer = std::make_unique<InitializerListExpr>(string->token, std::move(bytes));
    }
    if (declaration.inferred_extent) {
        const auto* list = dynamic_cast<const InitializerListExpr*>(declaration.initializer.get());
        if (!list || list->elements.empty())
            throw CompilerError("Inferred array extent requires a nonempty initializer list.", declaration.token);
        declaration.type.array_size = static_cast<int>(list->elements.size());
        declaration.inferred_extent = false;
    }
    if (static_cast<std::int64_t>(declaration.type.sizeInBytes) * std::max(1, declaration.type.array_size) > 65536)
        throw CompilerError("Aggregate initialization exceeds one bank.", declaration.token);
    flattenInitializer(declaration.type, std::move(declaration.initializer), 0, declaration.aggregate_initializers, declaration.token);
}
void Analyzer::flattenInitializer(Type type, std::unique_ptr<Expr> initializer, int offset,
                                 std::vector<AggregateInitializer>& output, const Token& source) {
    auto* list = dynamic_cast<InitializerListExpr*>(initializer.get());
    if (type.array_size > 0) {
        const auto count = type.array_size;
        type.array_size = 0;
        if (initializer && (!list || list->elements.size() > static_cast<std::size_t>(count)))
            throw CompilerError("Array initializer count exceeds its extent or requires braces.", source);
        for (int index = 0; index < count; ++index) {
            std::unique_ptr<Expr> element;
            if (list && static_cast<std::size_t>(index) < list->elements.size()) element = std::move(list->elements[static_cast<std::size_t>(index)]);
            flattenInitializer(type, std::move(element), offset + index * type.sizeInBytes, output, source);
        }
        return;
    }
    if (type.base == BaseType::STRUCT && type.pointer_level == 0) {
        const auto& structure = m_struct_symbols.at(type.structName);
        if (initializer && (!list || list->elements.size() > structure.member_order.size()))
            throw CompilerError("Struct initializer requires members in declaration order.", source);
        for (std::size_t index = 0; index < structure.member_order.size(); ++index) {
            const auto& member = structure.members.at(structure.member_order[index]);
            Type member_type = member.type;
            member_type.is_const = member_type.is_const || type.is_const;
            member_type.is_volatile = member_type.is_volatile || type.is_volatile;
            if (member.type.sizeInBytes > 1 && (member.offset & 1))
                throw CompilerError("Cannot initialize a misaligned packed word member.", source);
            std::unique_ptr<Expr> element;
            if (list && index < list->elements.size()) element = std::move(list->elements[index]);
            flattenInitializer(member_type, std::move(element), offset + member.offset, output, source);
        }
        return;
    }
    if (list) {
        if (list->elements.size() != 1) throw CompilerError("Scalar initializer requires one value.", source);
        auto scalar = std::move(list->elements.front()); initializer = std::move(scalar);
    }
    if (!initializer) {
        if (type.pointer_level > 0) initializer = std::make_unique<NullExpr>(Token(TokenType::KEYWORD_NULL, "null", source));
        else initializer = std::make_unique<LiteralExpr>(Token(TokenType::LITERAL_INTEGER, "0", source));
    }
    analyzeExpr(*initializer, &type); coerceExpr(initializer, type);
    if (output.size() >= 65536) throw CompilerError("Too many aggregate scalar elements.", source);
    output.push_back({offset, type, std::move(initializer)});
}

void Analyzer::visit(NullExpr& expr, const Type* context) {
    expr.result_type = context && context->pointer_level > 0 ? valueType(*context) :
        pointerTo(Type{BaseType::VOID, "", 0, false}, AddressSpace::RAM);
    expr.is_constant = true; expr.constant_value = 0;
}
void Analyzer::visit(UpdateExpr& expr, const Type*) {
    analyzeExpr(*expr.target, nullptr);
    const Type target = expr.target->result_type;
    if (!isAssignableLValue(*expr.target) || target.is_const ||
        (expr.target->address_type.pointer_level > 0 && expr.target->address_type.space == AddressSpace::ROM))
        throw CompilerError("Update requires a mutable l-value.", expr.token);
    if (target.array_size > 0 || (target.pointer_level == 0 && target.base != BaseType::WORD && target.base != BaseType::BYTE))
        throw CompilerError("Update requires an integer or pointer object.", expr.token);
    auto operation = expr.operation;
    if (operation.type == TokenType::PLUS_PLUS || operation.type == TokenType::PLUS_EQUAL) { operation.type = TokenType::PLUS; operation.lexeme = "+"; }
    else if (operation.type == TokenType::MINUS_MINUS || operation.type == TokenType::MINUS_EQUAL) { operation.type = TokenType::MINUS; operation.lexeme = "-"; }
    else {
        operation.lexeme.pop_back();
        switch (operation.type) {
            case TokenType::STAR_EQUAL: operation.type = TokenType::STAR; break;
            case TokenType::SLASH_EQUAL: operation.type = TokenType::SLASH; break;
            case TokenType::PERCENT_EQUAL: operation.type = TokenType::PERCENT; break;
            case TokenType::AND_EQUAL: operation.type = TokenType::AMPERSAND; break;
            case TokenType::OR_EQUAL: operation.type = TokenType::PIPE; break;
            case TokenType::XOR_EQUAL: operation.type = TokenType::CARET; break;
            case TokenType::SHIFT_LEFT_EQUAL: operation.type = TokenType::SHIFT_LEFT; break;
            case TokenType::SHIFT_RIGHT_EQUAL: operation.type = TokenType::SHIFT_RIGHT; break;
            default: throw CompilerError("Unsupported update operator.", expr.token);
        }
    }
    BinaryExpr binary(std::move(expr.target), operation, std::move(expr.value));
    visit(binary, nullptr);
    expr.operation = operation;
    expr.operation_type = binary.result_type;
    expr.pointer_stride = binary.pointer_stride;
    expr.target = std::move(binary.left);
    while (auto* cast = dynamic_cast<CastExpr*>(expr.target.get())) {
        if (!cast->implicit_conversion) break;
        auto target_expression = std::move(cast->expression);
        expr.target = std::move(target_expression);
    }
    expr.value = std::move(binary.right);
    expr.result_type = valueType(target);
}

void Analyzer::visit(BinaryExpr& expr, const Type*) {
    analyzeExpr(*expr.left, nullptr);
    analyzeExpr(*expr.right, nullptr);
    if ((expr.token.type == TokenType::EQUAL_EQUAL || expr.token.type == TokenType::BANG_EQUAL) &&
        (expr.left->result_type.pointer_level > 0 || expr.right->result_type.pointer_level > 0)) {
        if (dynamic_cast<NullExpr*>(expr.left.get())) analyzeExpr(*expr.left, &expr.right->result_type);
        if (dynamic_cast<NullExpr*>(expr.right.get())) analyzeExpr(*expr.right, &expr.left->result_type);
        Type left = expr.left->result_type, right = expr.right->result_type;
        left.pointee_qualifiers.clear(); right.pointee_qualifiers.clear();
        if (left.pointer_level == 0 || right.pointer_level == 0 || !sameType(left, right))
            throw CompilerError("Pointer equality requires compatible pointer representations and address spaces.", expr.token);
        expr.result_type = {BaseType::BOOL, "", 1, false};
        return;
    }
    if (expr.token.type == TokenType::AND_AND || expr.token.type == TokenType::OR_OR) {
        const Type boolean{BaseType::BOOL, "", 1, false};
        coerceExpr(expr.left, boolean);
        coerceExpr(expr.right, boolean);
        expr.result_type = boolean;
        return;
    }
    if (expr.left->result_type.pointer_level > 0 &&
        expr.right->result_type.pointer_level == 0 &&
        (expr.right->result_type.base == BaseType::BYTE || expr.right->result_type.base == BaseType::WORD) &&
        (expr.token.type == TokenType::PLUS || expr.token.type == TokenType::MINUS)) {
        expr.pointer_stride = pointeeSize(expr.left->result_type, expr.token);
        expr.result_type = expr.left->result_type;
        validateConstantAddress(expr, expr.result_type, expr.pointer_stride);
        return;
    }
    if (expr.token.type == TokenType::SLASH || expr.token.type == TokenType::PERCENT) {
        std::int64_t divisor = 0;
        if (constantIndex(*expr.right, divisor) && divisor == 0)
            throw CompilerError("Division by zero.", expr.token);
    }
    if ((expr.left->result_type.base != BaseType::BYTE && expr.left->result_type.base != BaseType::WORD && expr.left->result_type.base != BaseType::BOOL) ||
        (expr.right->result_type.base != BaseType::BYTE && expr.right->result_type.base != BaseType::WORD && expr.right->result_type.base != BaseType::BOOL) ||
        expr.left->result_type.pointer_level != 0 ||
        expr.right->result_type.pointer_level != 0) {
        throw CompilerError("Binary operators require integer operands.",
                            expr.token);
    }
    Type promoted{BaseType::WORD, "", 2, false};
    promoted.space = AddressSpace::RAM;
    if (expr.token.type == TokenType::SHIFT_LEFT || expr.token.type == TokenType::SHIFT_RIGHT) {
        std::int64_t count = 0;
        if (constantIndex(*expr.right, count) && (count < 0 || count > 15))
            throw CompilerError("Shift count must be in 0..15 after integer promotion.", expr.token);
        promoted.is_unsigned = expr.left->result_type.is_unsigned;
        coerceExpr(expr.left, promoted);
        Type countType = promoted;
        countType.is_unsigned = expr.right->result_type.is_unsigned;
        coerceExpr(expr.right, countType);
        expr.result_type = promoted;
        return;
    }
    // Contextual literals do not permit implicit signedness changes for
    // already-typed variables or subexpressions.
    if (dynamic_cast<LiteralExpr*>(expr.right.get()) && !dynamic_cast<LiteralExpr*>(expr.left.get())) {
        promoted.is_unsigned = expr.left->result_type.is_unsigned;
        analyzeExpr(*expr.right, &promoted);
    } else if (dynamic_cast<LiteralExpr*>(expr.left.get()) && !dynamic_cast<LiteralExpr*>(expr.right.get())) {
        promoted.is_unsigned = expr.right->result_type.is_unsigned;
        analyzeExpr(*expr.left, &promoted);
    } else promoted.is_unsigned = expr.left->result_type.is_unsigned;
    if (expr.left->result_type.is_unsigned != expr.right->result_type.is_unsigned &&
        expr.left->result_type.base != BaseType::BOOL && expr.right->result_type.base != BaseType::BOOL)
        throw CompilerError("Mixed signed/unsigned operands require an explicit cast.", expr.token);
    coerceExpr(expr.left, promoted);
    coerceExpr(expr.right, promoted);
    const auto operation = expr.token.type;
    expr.result_type = operation == TokenType::EQUAL_EQUAL || operation == TokenType::BANG_EQUAL ||
        operation == TokenType::LESS || operation == TokenType::LESS_EQUAL ||
        operation == TokenType::GREATER || operation == TokenType::GREATER_EQUAL
        ? Type{BaseType::BOOL, "", 1, false} : promoted;
}

void Analyzer::visit(UnaryExpr& expr, const Type* context) {
    if (expr.token.type == TokenType::BANG) {
        analyzeExpr(*expr.right, nullptr);
        coerceExpr(expr.right, Type{BaseType::BOOL, "", 1, false});
        expr.result_type = {BaseType::BOOL, "", 1, false};
        return;
    }
    if (expr.token.type == TokenType::MINUS && dynamic_cast<LiteralExpr*>(expr.right.get()) &&
        expr.right->token.type == TokenType::LITERAL_INTEGER) {
        Token negative = expr.right->token;
        negative.lexeme = "-" + negative.lexeme;
        LiteralExpr folded(negative);
        visit(folded, context);
        analyzeExpr(*expr.right, nullptr);
        expr.result_type = folded.result_type;
        return;
    }
    analyzeExpr(*expr.right, context);
    if (expr.right->result_type.pointer_level > 0 ||
        (expr.right->result_type.base != BaseType::BYTE && expr.right->result_type.base != BaseType::WORD))
        throw CompilerError("Unary arithmetic requires an integer operand, not a pointer.", expr.token);
    expr.result_type = expr.right->result_type;
}

void Analyzer::visit(MemberAccessExpr& expr, const Type*) {
    if (expr.through_pointer) {
        const auto token = expr.object->token;
        expr.object = std::make_unique<DereferenceExpr>(token, std::move(expr.object));
        expr.through_pointer = false;
    }
    analyzeExpr(*expr.object, nullptr);
    if (expr.object->result_type.pointer_level != 0 ||
        expr.object->result_type.base != BaseType::STRUCT) {
        throw CompilerError("Request for member '" + expr.token.lexeme + "' in something that is not a struct.", expr.token);
    }
    const StructSymbol& struct_def = m_struct_symbols.at(expr.object->result_type.structName);
    if (!struct_def.members.count(expr.token.lexeme)) {
        throw CompilerError("Struct '" + struct_def.name + "' has no member named '" + expr.token.lexeme + "'.", expr.token);
    }
    const StructMemberSymbol& member_def = struct_def.members.at(expr.token.lexeme);
    expr.result_type = member_def.type;
    expr.result_type.is_const = expr.result_type.is_const || expr.object->result_type.is_const;
    expr.result_type.is_volatile = expr.result_type.is_volatile || expr.object->result_type.is_volatile;
    expr.member_offset = member_def.offset;
    if (expr.result_type.sizeInBytes > 1 && (expr.member_offset & 1))
        throw CompilerError("Packed member requires a misaligned word access; use byte fields instead.", expr.token);
    expr.address_type = pointerTo(expr.result_type, expr.object->address_type.space,
                                  isFarPointer(expr.object->address_type));
}

void Analyzer::visit(AddressOfExpr& expr, const Type*) {
    if (dynamic_cast<const PlotCoordinateExpr*>(expr.right.get()))
        throw CompilerError("Plot coordinate registers do not have memory addresses.", expr.token);
    analyzeExpr(*expr.right, nullptr);
    if (expr.right->address_type.pointer_level > MaxPointerDepth)
        throw CompilerError("Pointer depth exceeds the supported limit of 32.", expr.token);
    if (expr.right->address_type.pointer_level > 0) {
        expr.result_type = expr.right->address_type;
    } else {
        throw CompilerError("Address-of operator '&' can only be applied to an l-value (e.g., a variable or array element).", expr.token);
    }
}

void Analyzer::visit(DereferenceExpr& expr, const Type*) {
    analyzeExpr(*expr.right, nullptr);
    if (expr.right->result_type.pointer_level == 0) {
        throw CompilerError("Cannot dereference a non-pointer type.", expr.token);
    }
    Type resultType = pointeeType(expr.right->result_type);
    normalizeType(resultType, expr.token);
    if (isVoidValue(resultType))
        throw CompilerError("Cannot dereference a void pointer; cast to a complete object pointer first.", expr.token);
    validateConstantAddress(*expr.right, expr.right->result_type, resultType.sizeInBytes);
    expr.result_type = resultType;
    expr.address_type = expr.right->result_type;
}

void Analyzer::visit(SubscriptExpr& expr, const Type*) {
    analyzeExpr(*expr.array, nullptr);
    Type arrayType = expr.array->result_type;
    analyzeExpr(*expr.index, nullptr);
    Type indexType = expr.index->result_type;
    if (indexType.pointer_level > 0 || (indexType.base != BaseType::BYTE && indexType.base != BaseType::WORD)) {
        throw CompilerError("Array index must be an integer type.", expr.index->token);
    }
    if (arrayType.array_size == 0 && arrayType.pointer_level == 0) {
        throw CompilerError("Subscript operator '[]' can only be used on arrays or pointers.", expr.token);
    }
    if (arrayType.array_size > 0) {
        expr.element_size = arrayType.sizeInBytes;
    } else {
        expr.element_size = pointeeSize(arrayType, expr.token);
    }
    Type resultType = arrayType;
    if (resultType.array_size > 0) {
        resultType.array_size = 0;
    } else {
        resultType = pointeeType(resultType);
        normalizeType(resultType, expr.token);
    }
    expr.result_type = resultType;
    expr.address_type = pointerTo(resultType, arrayType.array_size > 0 ? expr.array->address_type.space : arrayType.space,
                                  arrayType.array_size == 0 && isFarPointer(arrayType));
    if (arrayType.array_size == 0) {
        try {
            std::uint32_t address = 0;
            std::int64_t index = 0;
            if (constantPointer(*expr.array, address) && constantIndex(*expr.index, index))
                (void)GsuPointer::add(address, index, expr.element_size, arrayType.space, isFarPointer(arrayType), storageAlignment(resultType));
        } catch (const std::exception& error) {
            throw CompilerError(error.what(), expr.token);
        }
    }
}

void Analyzer::visit(PlotCoordinateExpr& expr, const Type*) {
    requireCapability(TargetCapability::Graphics, expr.token);
    if (!m_isInPlottingContext)
        throw CompilerError("Plot coordinates require a plotting context (plot { ... }).", expr.token);
    expr.result_type = {BaseType::WORD, "", 2, false};
}

void Analyzer::visit(VariableExpr& expr, const Type*) {
    for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
        const auto& scope = *it;
        if (scope.count(expr.token.lexeme)) {
            expr.symbol_id = scope.at(expr.token.lexeme);
            expr.result_type = m_current_function_locals->at(expr.symbol_id).type;
            if (!m_constexpr_ids.count(expr.symbol_id)) expr.address_type = pointerTo(expr.result_type, AddressSpace::RAM);
            const auto constant = m_local_constants.find(expr.symbol_id);
            if (constant != m_local_constants.end()) { expr.is_constant = true; expr.constant_value = constant->second; }
            return;
        }
    }
    resolveConstant(expr.token.lexeme);
    const auto constant = m_named_constants.find(expr.token.lexeme);
    if (constant != m_named_constants.end()) {
        expr.is_constant = true;
        expr.constant_value = constant->second.value;
        expr.result_type = constant->second.type;
        expr.result_type.is_const = true;
        if (m_data_manager.hasSymbol(expr.token.lexeme)) {
            expr.result_type = m_data_manager.getSymbolType(expr.token.lexeme);
            expr.address_type = pointerTo(expr.result_type, AddressSpace::RAM);
        }
        return;
    }
    if (m_data_manager.hasSymbol(expr.token.lexeme)) {
        expr.result_type = m_data_manager.getSymbolType(expr.token.lexeme);
        const auto& entry = m_data_manager.getEntries().at(expr.token.lexeme);
        expr.symbol_id = entry.id;
        expr.address_type = pointerTo(expr.result_type, entry.storage);
        if (expr.result_type.array_size > 0) {
            expr.is_array_decay = true;
            expr.result_type = pointerTo(expr.result_type, entry.storage);
        }
    } else {
        if (m_function_symbols.count(expr.token.lexeme)) {
            throw CompilerError("Function pointers and inter-bank code calls are not supported yet.", expr.token);
        } else {
            throw CompilerError("Use of undeclared symbol '" + expr.token.lexeme + "'.", expr.token);
        }
    }
}

void Analyzer::visit(LiteralExpr& expr, const Type* context) {
    if (expr.token.type == TokenType::KEYWORD_TRUE || expr.token.type == TokenType::KEYWORD_FALSE) {
        expr.result_type = {BaseType::BOOL, "", 1, false};
        return;
    }
    if (context && context->pointer_level > 0)
        throw CompilerError("Pointer address construction requires an explicit cast.", expr.token);
    const auto value = parseIntegerLiteral(expr.token, "integer literal");
    if (context) {
        if (context->base == BaseType::BYTE) {
            if (context->is_unsigned) {
                if (value < 0 || value > 255) throw CompilerError("Value '" + std::to_string(value) + "' out of range for unsigned byte.", expr.token);
            } else {
                if (value < -128 || value > 127) throw CompilerError("Value '" + std::to_string(value) + "' out of range for signed byte.", expr.token);
            }
        } else if (context->base == BaseType::WORD) {
            if (context->is_unsigned) {
                if (value < 0 || value > 65535) throw CompilerError("Value '" + std::to_string(value) + "' out of range for unsigned word.", expr.token);
            } else {
                if (value < -32768 || value > 32767) throw CompilerError("Value '" + std::to_string(value) + "' out of range for signed word.", expr.token);
            }
        }
        expr.result_type = context->base == BaseType::BOOL ? Type{BaseType::WORD, "", 2, false} : valueType(*context);
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
        else throw CompilerError("Literal '" + std::to_string(value) + "' is too large for a 16-bit word.", expr.token);
    }
}

void Analyzer::visit(CastExpr& expr, const Type*) {
    normalizeType(expr.cast_to_type, expr.token);
    if (dynamic_cast<NullExpr*>(expr.expression.get()) && expr.cast_to_type.pointer_level > 0) {
        analyzeExpr(*expr.expression, &expr.cast_to_type);
        expr.result_type = expr.cast_to_type;
        return;
    }
    if (expr.cast_to_type.pointer_level > 0 && dynamic_cast<LiteralExpr*>(expr.expression.get())) {
        const auto value = parseIntegerLiteral(expr.expression->token, "pointer address");
        if (value < 0 || value > (isFarPointer(expr.cast_to_type) ? 0xffffff : 0xffff))
            throw CompilerError("Pointer address exceeds its representation.", expr.token);
        expr.expression->result_type = expr.cast_to_type;
    } else analyzeExpr(*expr.expression, nullptr);

    const Type& sourceType = expr.expression->result_type;
    const Type& targetType = expr.cast_to_type;

    // Basic validation
    if ((sourceType.base == BaseType::STRUCT && sourceType.pointer_level == 0) ||
        (targetType.base == BaseType::STRUCT && targetType.pointer_level == 0)) {
        throw CompilerError("Struct casts by value are not supported; cast a pointer instead.", expr.token);
    }

    if ((sourceType.base == BaseType::VOID && sourceType.pointer_level == 0) ||
        (targetType.base == BaseType::VOID && targetType.pointer_level == 0)) {
         throw CompilerError("Cannot cast to or from a void type.", expr.token);
    }
    if (sourceType.pointer_level > 0 && targetType.pointer_level == 0 &&
        (isFarPointer(sourceType) || targetType.base != BaseType::WORD || !targetType.is_unsigned))
        throw CompilerError("Pointer-to-integer casts require a near pointer and unsigned word; far banks cannot be discarded.", expr.token);
    if (sourceType.pointer_level == 0 && targetType.pointer_level > 0 && !dynamic_cast<LiteralExpr*>(expr.expression.get()) &&
        (sourceType.base != BaseType::WORD || !sourceType.is_unsigned))
        throw CompilerError("Integer-to-pointer casts require an unsigned word or address literal.", expr.token);
    if (sourceType.pointer_level > 0 && targetType.pointer_level > 0) {
        Type qualifiedSource = sourceType;
        Type qualifiedTarget = targetType;
        normalizePointerLayers(qualifiedSource);
        normalizePointerLayers(qualifiedTarget);
        for (std::size_t layer = 0; layer < qualifiedSource.pointee_qualifiers.size(); ++layer) {
            if (layer >= qualifiedTarget.pointee_qualifiers.size() ||
                (qualifiedSource.pointee_qualifiers[layer].is_const && !qualifiedTarget.pointee_qualifiers[layer].is_const) ||
                (qualifiedSource.pointee_qualifiers[layer].is_volatile && !qualifiedTarget.pointee_qualifiers[layer].is_volatile))
                throw CompilerError("Pointer cast cannot discard const or volatile qualifiers.", expr.token);
        }
        if (sourceType.pointer_level != targetType.pointer_level || sourceType.space != targetType.space ||
            !samePointerLayers(pointeeType(sourceType), pointeeType(targetType)))
            throw CompilerError("Pointer cast cannot change address space or nested pointer representation.", expr.token);
    }
    if (isFarPointer(targetType) && sourceType.pointer_level == 0)
        throw CompilerError("Construct a far address with an explicit 24-bit address literal.", expr.token);

    // The result type of the cast expression is the type it was cast to.
    expr.result_type = targetType;
    if (targetType.pointer_level > 0)
        validateConstantAddress(expr, targetType, targetType.base == BaseType::VOID && targetType.pointer_level == 1
                                ? 1 : pointeeSize(targetType, expr.token));
}

int Analyzer::pointeeSize(const Type& type, const Token& source) {
    Type element = pointeeType(type);
    normalizeType(element, source);
    if (element.sizeInBytes <= 0 || element.sizeInBytes > 65528)
        throw CompilerError("Pointer arithmetic/access requires a complete non-void pointee.",
                            source);
    return element.sizeInBytes;
}

namespace {
bool constantIndex(const Expr& expression, std::int64_t& value) {
    if (ConstantEvaluator::evaluate(expression, value)) return true;
    if (const auto* literal = dynamic_cast<const LiteralExpr*>(&expression)) {
        value = DiscoNumeric::parse(literal->token.lexeme, nullptr, 0);
        return true;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
        if (unary->token.type == TokenType::MINUS && constantIndex(*unary->right, value)) {
            value = -value;
            return true;
        }
    }
    return false;
}

bool constantPointer(const Expr& expression, std::uint32_t& address) {
    if (const auto* cast = dynamic_cast<const CastExpr*>(&expression)) {
        if (const auto* literal = dynamic_cast<const LiteralExpr*>(cast->expression.get())) {
            address = static_cast<std::uint32_t>(DiscoNumeric::parse(literal->token.lexeme, nullptr, 0));
            return true;
        }
        if (cast->expression->result_type.pointer_level > 0 &&
            isFarPointer(cast->expression->result_type) == isFarPointer(cast->result_type))
            return constantPointer(*cast->expression, address);
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
        std::int64_t index = 0;
        if (binary->pointer_stride > 0 && constantPointer(*binary->left, address) &&
            constantIndex(*binary->right, index)) {
            if (binary->token.type == TokenType::MINUS) index = -index;
            address = GsuPointer::add(address, index, binary->pointer_stride,
                                      binary->result_type.space, isFarPointer(binary->result_type), storageAlignment(pointeeType(binary->result_type)));
            return true;
        }
    }
    return false;
}
} // namespace

void Analyzer::validateConstantAddress(const Expr& expression, const Type& pointer, int width) const {
    try {
        std::uint32_t address = 0;
        if (constantPointer(expression, address))
            GsuPointer::validate(address, pointer.space, isFarPointer(pointer), width, storageAlignment(pointeeType(pointer)));
    } catch (const std::exception& error) {
        throw CompilerError(error.what(), expression.token);
    }
}

void Analyzer::visit(SwitchStmt& stmt) {
    if (containsPlotBoundary(*stmt.body)) {
        throw CompilerError("Plotting context boundaries cannot appear inside a switch.",
                            stmt.token);
    }
    m_break_context_stack.push_back(true);
    m_case_values_stack.emplace_back();

    analyzeExpr(*stmt.condition, nullptr);
    if (stmt.condition->result_type.base != BaseType::WORD && stmt.condition->result_type.base != BaseType::BYTE) {
        throw CompilerError("Switch condition must be of an integer type.", stmt.condition->token);
    }
    
    bool has_default = false;
    for (std::size_t index = 0; index < stmt.body->statements.size(); ++index) {
        const auto& s = stmt.body->statements[index];
        if (dynamic_cast<FallthroughStmt*>(s.get())) {
            m_fallthrough_marker_allowed = index + 1 < stmt.body->statements.size() &&
                (dynamic_cast<CaseStmt*>(stmt.body->statements[index + 1].get()) || dynamic_cast<DefaultStmt*>(stmt.body->statements[index + 1].get()));
            s->accept(*this);
            m_fallthrough_marker_allowed = false;
            continue;
        }
        if (auto* case_stmt = dynamic_cast<CaseStmt*>(s.get())) {
            case_stmt->accept(*this);
        } else if (auto* default_stmt = dynamic_cast<DefaultStmt*>(s.get())) {
            if (has_default) {
                throw CompilerError("Multiple 'default' labels in one switch statement.", default_stmt->token);
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
                            stmt.token);
    }
    analyzeExpr(*stmt.value, nullptr);
    std::int64_t value = 0;
    if (stmt.value->result_type.pointer_level != 0 || !ConstantEvaluator::evaluate(*stmt.value, value))
        throw CompilerError("Case label must be an integer constant expression.", stmt.value->token);
    const auto location = stmt.value->token;
    auto folded = std::make_unique<LiteralExpr>(Token(TokenType::LITERAL_INTEGER, std::to_string(value), location));
    folded->result_type = stmt.value->result_type;
    stmt.value = std::move(folded);
    if (m_case_values_stack.back().count(value)) {
        throw CompilerError("Duplicate case value '" + std::to_string(value) + "'.", location);
    }
    m_case_values_stack.back().insert(value);
}

void Analyzer::visit(DefaultStmt& stmt) {
    if (m_break_context_stack.empty() || !m_break_context_stack.back()) {
        throw CompilerError("'default' label not within a switch statement.", stmt.token);
    }
}

void Analyzer::visit(BreakStmt& stmt) {
    if (m_break_context_stack.empty()) {
        throw CompilerError("'break' statement not within a loop or switch.",
                            stmt.token);
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

namespace {

// True when a break inside this statement leaves the enclosing switch.
// Breaks in nested loops or switches target those constructs instead.
bool breaksEnclosingSwitch(const Stmt& stmt) {
    if (dynamic_cast<const BreakStmt*>(&stmt)) return true;
    if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
        for (const auto& child : block->statements) if (breaksEnclosingSwitch(*child)) return true;
        return false;
    }
    if (const auto* plot = dynamic_cast<const PlotBlockStmt*>(&stmt)) return breaksEnclosingSwitch(*plot->body);
    if (const auto* conditional = dynamic_cast<const IfStmt*>(&stmt))
        return breaksEnclosingSwitch(*conditional->thenBranch) ||
               (conditional->elseBranch && breaksEnclosingSwitch(*conditional->elseBranch));
    return false;
}

} // namespace

bool Analyzer::canFallThrough(const Stmt& stmt) const {
    if (dynamic_cast<const ReturnStmt*>(&stmt) || dynamic_cast<const BreakStmt*>(&stmt) || dynamic_cast<const ContinueStmt*>(&stmt)) {
        return false;
    }

    if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
        return blockCanFallThrough(block->statements);
    }
    if (const auto* plot = dynamic_cast<const PlotBlockStmt*>(&stmt))
        return canFallThrough(*plot->body);

    if (const auto* conditional = dynamic_cast<const IfStmt*>(&stmt)) {
        // Without an else branch, the condition can always choose the path
        // that reaches the end of the if statement.
        if (!conditional->elseBranch) {
            return true;
        }
        return canFallThrough(*conditional->thenBranch) ||
               canFallThrough(*conditional->elseBranch);
    }

    if (const auto* selection = dynamic_cast<const SwitchStmt*>(&stmt)) {
        // A switch completes when no label matches, when a break targets it,
        // or when control runs off its final arm. Earlier arms can only
        // return, leave through an enclosing construct, or reach that arm.
        const auto& statements = selection->body->statements;
        bool has_default = false;
        std::size_t final_arm = 0;
        for (std::size_t index = 0; index < statements.size(); ++index) {
            const bool is_default = dynamic_cast<const DefaultStmt*>(statements[index].get()) != nullptr;
            if (is_default || dynamic_cast<const CaseStmt*>(statements[index].get())) final_arm = index + 1;
            has_default = has_default || is_default;
            if (breaksEnclosingSwitch(*statements[index])) return true;
        }
        if (!has_default) return true;
        for (std::size_t index = final_arm; index < statements.size(); ++index) {
            if (!canFallThrough(*statements[index])) return false;
        }
        return true;
    }

    // A loop is conservatively considered fall-through.  Proving that a
    // source-controlled loop is infinite requires constant evaluation and
    // handling of break/control-flow edges; rejecting a missing return is
    // safer than accepting an invalid function ABI on an incomplete proof.
    return true;
}
