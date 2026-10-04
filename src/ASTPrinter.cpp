#include "ASTPrinter.hpp"
#include "Types.hpp"

std::string ASTPrinter::print(Stmt& stmt) {
    stmt.accept(*this);
    return m_result;
}

std::string ASTPrinter::printExpr(Expr& expr) {
    expr.accept(*this, nullptr);
    return m_result;
}

	void ASTPrinter::visit(CallExpr& expr, const Type*) {
    std::string temp = "(" + printExpr(*expr.callee);
    for (const auto& arg : expr.arguments) {
        temp += " " + printExpr(*arg);
    }
    temp += ")";
    m_result = temp;
}

void ASTPrinter::visit(CastExpr& expr, const Type*) {
    m_result = "(cast " + to_string(expr.cast_to_type) + " " + printExpr(*expr.expression) + ")";
}

void ASTPrinter::visit(MemberAccessExpr& expr, const Type*) {
    m_result = "(. " + printExpr(*expr.object) + " " + expr.token.lexeme + ")";
}

void ASTPrinter::visit(InitializerListExpr&, const Type*) { m_result = "(initializer-list)"; }
void ASTPrinter::visit(StringExpr&, const Type*) { m_result = "(string)"; }
void ASTPrinter::visit(NullExpr&, const Type*) { m_result = "null"; }
void ASTPrinter::visit(UpdateExpr& expr, const Type*) { m_result = "(" + expr.token.lexeme + " " + printExpr(*expr.target) + " " + printExpr(*expr.value) + ")"; }
void ASTPrinter::visit(ForStmt& stmt) {
    const auto initializer = stmt.initializer ? print(*stmt.initializer) : "()";
    const auto condition = printExpr(*stmt.condition);
    const auto increment = stmt.increment ? printExpr(*stmt.increment) : "()";
    m_result = "(for " + initializer + " " + condition + " " + increment + " " + print(*stmt.body) + ")";
}
void ASTPrinter::visit(ContinueStmt&) { m_result = "(continue)"; }
void ASTPrinter::visit(FallthroughStmt&) { m_result = "(fallthrough)"; }
void ASTPrinter::visit(EnumDeclStmt& stmt) { m_result = "(enum " + stmt.token.lexeme + ")"; }
void ASTPrinter::visit(StaticAssertStmt& stmt) { m_result = "(static_assert " + printExpr(*stmt.condition) + ")"; }
void ASTPrinter::visit(TypeAliasDeclStmt& stmt) { m_result = "(type " + stmt.token.lexeme + " " + to_string(stmt.resolved_type) + ")"; }
void ASTPrinter::visit(LayoutQueryExpr& expr, const Type*) { m_result = "(" + expr.token.lexeme + " " + std::to_string(expr.constant_value) + ")"; }
void ASTPrinter::visit(LiteralExpr& expr, const Type*) { m_result = expr.token.lexeme; }
void ASTPrinter::visit(VariableExpr& expr, const Type*) { m_result = expr.token.lexeme; }
void ASTPrinter::visit(PlotCoordinateExpr& expr, const Type*) { m_result = expr.is_y ? "cursor.y" : "cursor.x"; }
void ASTPrinter::visit(ReadPixelExpr& expr, const Type*) {
    m_result = expr.x ? "(read_pixel at " + printExpr(*expr.x) + " " + printExpr(*expr.y) + ")" : "(read_pixel)";
}
void ASTPrinter::visit(BitmapDeclStmt& stmt) { m_result = "(bitmap " + stmt.token.lexeme + ")"; }
void ASTPrinter::visit(UseBitmapStmt& stmt) { m_result = "(use bitmap " + stmt.token.lexeme + ")"; }
void ASTPrinter::visit(UnaryExpr& expr, const Type*) { m_result = "(" + expr.token.lexeme + " " + printExpr(*expr.right) + ")"; }
void ASTPrinter::visit(BinaryExpr& expr, const Type*) { m_result = "(" + expr.token.lexeme + " " + printExpr(*expr.left) + " " + printExpr(*expr.right) + ")"; }
void ASTPrinter::visit(AssignExpr& expr, const Type*) {
    m_result = "(= " + printExpr(*expr.name) + " " + printExpr(*expr.value) + ")";
}

void ASTPrinter::visit(FunctionDeclStmt& stmt) {
    std::string temp_str = "(";
    if (stmt.is_cached) temp_str += "cache ";
    temp_str += "func " + to_string(stmt.returnType) + " " + stmt.token.lexeme + " (";

    // Print parameters
    for (size_t i = 0; i < stmt.params.size(); ++i) {
        temp_str += to_string(stmt.params[i].type) + " " + stmt.params[i].name.lexeme;
        if (i < stmt.params.size() - 1) {
            temp_str += ", ";
        }
    }
    temp_str += ")";

    if (stmt.is_prototype) {
        m_result = temp_str + ";)";
        return;
    }

    // Let's create a temporary BlockStmt to print the body consistently
    auto bodyBlock = std::make_unique<BlockStmt>(std::move(stmt.body));
    temp_str += " " + print(*bodyBlock);
    // Restore the body to the original node since it was moved
    stmt.body = std::move(bodyBlock->statements);
    temp_str += ")";
    m_result = temp_str;
}

void ASTPrinter::visit(ReturnStmt& stmt) {
    if (stmt.value) {
        m_result = "(return " + printExpr(*stmt.value) + ")";
    } else {
        m_result = "(return)";
    }
}

void ASTPrinter::visit(VarDeclStmt& stmt) {
    std::string init = stmt.initializer ? " = " + printExpr(*stmt.initializer) : "";
    for (const auto& scalar : stmt.aggregate_initializers)
        init += " @" + std::to_string(scalar.offset) + "=" + printExpr(*scalar.value);
    m_result = "(var_decl " + to_string(stmt.type) + " " + stmt.token.lexeme + init + ")";
}

void ASTPrinter::visit(BlockStmt& stmt) {
    std::string temp_str = "{";
    for (const auto& s : stmt.statements) {
        temp_str += " " + print(*s);
    }
    temp_str += " }";
    m_result = temp_str;
}

void ASTPrinter::visit(IfStmt& stmt) {
    std::string temp_str = "(if " + printExpr(*stmt.condition);
    temp_str += " " + print(*stmt.thenBranch);
    if (stmt.elseBranch) {
        temp_str += " else " + print(*stmt.elseBranch);
    }
    temp_str += ")";
    m_result = temp_str;
}

void ASTPrinter::visit(WhileStmt& stmt) {
    std::string temp_str = "(";
    if (stmt.is_cached) temp_str += "cache ";
    temp_str += "while " + printExpr(*stmt.condition) + " " + print(*stmt.body) + ")";
    m_result = temp_str;
}

void ASTPrinter::visit(ExpressionStmt& stmt) {
    m_result = printExpr(*stmt.expression);
}

void ASTPrinter::visit(PlotStmt&) { m_result = "(pixel)"; }

void ASTPrinter::visit(PlotBeginStmt&) { m_result = "(plot_begin)"; }
void ASTPrinter::visit(PlotBlockStmt& stmt) { m_result = "(plot " + print(*stmt.body) + ")"; }
void ASTPrinter::visit(PlotEndStmt&) { m_result = "(plot_end)"; }
void ASTPrinter::visit(SetColorStmt& stmt) { m_result = "(color " + printExpr(*stmt.color_value) + ")"; }
void ASTPrinter::visit(CmodeStmt& stmt) { m_result = "(options " + printExpr(*stmt.options_value) + ")"; }
void ASTPrinter::visit(RpixStmt&) { m_result = "(flush)"; }
void ASTPrinter::visit(AddressOfExpr& expr, const Type*) { m_result = "(& " + printExpr(*expr.right) + ")"; }
void ASTPrinter::visit(DereferenceExpr& expr, const Type*) { m_result = "(* " + printExpr(*expr.right) + ")"; }
void ASTPrinter::visit(SubscriptExpr& expr, const Type*) { m_result = printExpr(*expr.array) + "[" + printExpr(*expr.index) + "]"; }

void ASTPrinter::visit(ConstDataStmt& stmt) {
    std::string temp = "(rom_const " + to_string(stmt.type) + " " + stmt.token.lexeme + " = {";
    for (const auto& expr : stmt.initializers) {
        temp += " " + printExpr(*expr);
    }
    temp += " })";
    m_result = temp;
}

void ASTPrinter::visit(StructDefStmt& stmt) {
    std::string temp = "(struct_def " + stmt.token.lexeme + " {";
    for (const auto& member : stmt.members) {
        temp += " (" + to_string(member.type) + " " + member.name.lexeme + ";)";
    }
    temp += " })";
    m_result = temp;
}

void ASTPrinter::visit(HardwareLoopStmt& stmt) {
    m_result = "(hardware_loop count=" + stmt.count->token.lexeme + " " + print(*stmt.body) + ")";
}

void ASTPrinter::visit(SwitchStmt& stmt) {
    std::string temp = "(switch " + printExpr(*stmt.condition) + " " + print(*stmt.body) + ")";
    m_result = temp;
}

void ASTPrinter::visit(CaseStmt& stmt) {
    m_result = "(case " + printExpr(*stmt.value) + ":)";
}

void ASTPrinter::visit(DefaultStmt&) {
    m_result = "(default:)";
}

void ASTPrinter::visit(BreakStmt&) {
    m_result = "(break)";
}
