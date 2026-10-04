#include "ModuleInterface.hpp"
#include <stdexcept>

namespace {
std::unique_ptr<Expr> copyExpression(const Expr* value, std::size_t depth = 1) {
    if (!value) return {};
    if (depth > Expr::MaxDepth) throw CompilerError("Module interface expression depth exceeded.", value->token);
    const auto child = [&](const std::unique_ptr<Expr>& expression) { return copyExpression(expression.get(), depth + 1); };
    std::unique_ptr<Expr> result;
    // Resolved public constants/layout must not leak private names into importers.
    if (value->is_constant && value->result_type.pointer_level == 0 &&
        (value->result_type.base == BaseType::BOOL || value->result_type.base == BaseType::BYTE || value->result_type.base == BaseType::WORD)) {
        const bool boolean = value->result_type.base == BaseType::BOOL;
        const auto kind = boolean ? (value->constant_value ? TokenType::KEYWORD_TRUE : TokenType::KEYWORD_FALSE) : TokenType::LITERAL_INTEGER;
        result = std::make_unique<LiteralExpr>(Token(kind, boolean ? (value->constant_value ? "true" : "false") : std::to_string(value->constant_value), value->token));
    } else if (dynamic_cast<const LiteralExpr*>(value)) result = std::make_unique<LiteralExpr>(value->token);
    else if (dynamic_cast<const VariableExpr*>(value)) result = std::make_unique<VariableExpr>(value->token);
    else if (dynamic_cast<const NullExpr*>(value)) result = std::make_unique<NullExpr>(value->token);
    else if (dynamic_cast<const StringExpr*>(value)) result = std::make_unique<StringExpr>(value->token);
    else if (const auto* expression = dynamic_cast<const BinaryExpr*>(value))
        result = std::make_unique<BinaryExpr>(child(expression->left), expression->token, child(expression->right));
    else if (const auto* expression = dynamic_cast<const UnaryExpr*>(value))
        result = std::make_unique<UnaryExpr>(expression->token, child(expression->right));
    else if (const auto* expression = dynamic_cast<const CastExpr*>(value))
        result = std::make_unique<CastExpr>(expression->token, expression->cast_to_type, child(expression->expression));
    else if (const auto* expression = dynamic_cast<const LayoutQueryExpr*>(value)) {
        auto query = std::make_unique<LayoutQueryExpr>(expression->token, expression->queried_type, child(expression->queried_expression));
        query->member = expression->member; result = std::move(query);
    } else if (const auto* expression = dynamic_cast<const CallExpr*>(value)) {
        std::vector<std::unique_ptr<Expr>> arguments;
        for (const auto& argument : expression->arguments) arguments.push_back(child(argument));
        result = std::make_unique<CallExpr>(child(expression->callee), expression->paren, std::move(arguments));
    } else if (const auto* expression = dynamic_cast<const MemberAccessExpr*>(value)) {
        auto member = std::make_unique<MemberAccessExpr>(child(expression->object), expression->token);
        member->through_pointer = expression->through_pointer; result = std::move(member);
    } else if (const auto* expression = dynamic_cast<const AddressOfExpr*>(value))
        result = std::make_unique<AddressOfExpr>(expression->token, child(expression->right));
    else if (const auto* expression = dynamic_cast<const DereferenceExpr*>(value))
        result = std::make_unique<DereferenceExpr>(expression->token, child(expression->right));
    else if (const auto* expression = dynamic_cast<const SubscriptExpr*>(value))
        result = std::make_unique<SubscriptExpr>(child(expression->array), expression->token, child(expression->index));
    else if (const auto* expression = dynamic_cast<const InitializerListExpr*>(value)) {
        std::vector<std::unique_ptr<Expr>> elements;
        for (const auto& element : expression->elements) elements.push_back(child(element));
        result = std::make_unique<InitializerListExpr>(expression->token, std::move(elements));
    } else if (const auto* expression = dynamic_cast<const AssignExpr*>(value))
        result = std::make_unique<AssignExpr>(child(expression->name), child(expression->value));
    else if (const auto* expression = dynamic_cast<const UpdateExpr*>(value))
        result = std::make_unique<UpdateExpr>(expression->operation, child(expression->target), child(expression->value), expression->postfix);
    else throw CompilerError("Unsupported expression in module interface.", value->token);
    result->is_constant = value->is_constant; result->constant_value = value->constant_value;
    result->result_type = value->result_type; result->address_type = value->address_type;
    return result;
}
}

std::unique_ptr<Stmt> moduleInterface(const Stmt& declaration, bool interface_file) {
    if (declaration.linkage == Linkage::Internal) return {};
    std::unique_ptr<Stmt> result;
    if (const auto* function = dynamic_cast<const FunctionDeclStmt*>(&declaration)) {
        auto parameters = function->params;
        for (auto& parameter : parameters) parameter.symbol_id = SymbolId{};
        result = std::make_unique<FunctionDeclStmt>(function->token, function->is_cached, function->returnType,
            std::move(parameters), std::vector<std::unique_ptr<Stmt>>{}, true);
    } else if (const auto* global = dynamic_cast<const VarDeclStmt*>(&declaration)) {
        const bool constant = global->is_constexpr || (global->type.is_const && !global->type.is_volatile &&
            global->type.pointer_level == 0 && global->type.array_size == 0 && !global->array_extent && !global->inferred_extent);
        auto variable = std::make_unique<VarDeclStmt>(global->type, global->token,
            constant ? copyExpression(global->initializer.get()) : nullptr);
        variable->is_global = true; variable->is_constexpr = global->is_constexpr;
        variable->is_extern = !global->is_constexpr;
        variable->array_extent = copyExpression(global->array_extent.get());
        if (global->inferred_extent && global->type.array_size == 0) {
            if (const auto* list = dynamic_cast<const InitializerListExpr*>(global->initializer.get()))
                variable->type.array_size = static_cast<int>(list->elements.size());
            else if (const auto* text = dynamic_cast<const StringExpr*>(global->initializer.get()))
                variable->type.array_size = static_cast<int>(text->token.lexeme.size() + 1);
        }
        result = std::move(variable);
    } else if (const auto* structure = dynamic_cast<const StructDefStmt*>(&declaration)) {
        std::vector<Member> members;
        for (const auto& member : structure->members)
            members.push_back({member.type, member.name, copyExpression(member.array_extent.get())});
        result = std::make_unique<StructDefStmt>(structure->token, std::move(members));
    } else if (const auto* enumeration = dynamic_cast<const EnumDeclStmt*>(&declaration)) {
        std::vector<EnumEntry> entries;
        for (const auto& entry : enumeration->entries) entries.push_back({entry.name, copyExpression(entry.value.get())});
        result = std::make_unique<EnumDeclStmt>(enumeration->token, enumeration->underlying_type, std::move(entries));
    } else if (const auto* alias = dynamic_cast<const TypeAliasDeclStmt*>(&declaration))
        result = std::make_unique<TypeAliasDeclStmt>(alias->token, alias->resolved_type);
    else if (const auto* data = dynamic_cast<const ConstDataStmt*>(&declaration)) {
        auto external = std::make_unique<ConstDataStmt>(data->token, data->type, std::vector<std::unique_ptr<Expr>>{}, data->is_array);
        external->is_extern = true; external->array_extent = copyExpression(data->array_extent.get());
        if (data->is_array && !data->array_extent && external->type.array_size == 0)
            external->type.array_size = static_cast<int>(data->initializers.size());
        result = std::move(external);
    } else if (const auto* assertion = dynamic_cast<const StaticAssertStmt*>(&declaration)) {
        if (interface_file) result = std::make_unique<StaticAssertStmt>(assertion->token, copyExpression(assertion->condition.get()));
    }
    if (!result) return {};
    result->is_imported = true;
    for (const auto& attribute : declaration.attributes) {
        Attribute copy{attribute.name, {}};
        for (const auto& argument : attribute.arguments) copy.arguments.push_back(copyExpression(argument.get()));
        result->attributes.push_back(std::move(copy));
    }
    return result;
}
