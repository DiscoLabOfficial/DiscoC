#pragma once
#include <string>
#include "AST.hpp"

class ASTPrinter : public Visitor {
public:
    ASTPrinter() = default;
    std::string print(Stmt& stmt);
    std::string printExpr(Expr& expr);

    void visit(LiteralExpr& expr, const Type* context) override;
    void visit(VariableExpr& expr, const Type* context) override;
    void visit(PlotCoordinateExpr& expr, const Type* context) override;
    void visit(ReadPixelExpr& expr, const Type* context) override;
    void visit(BitmapDeclStmt& stmt) override;
    void visit(UseBitmapStmt& stmt) override;
    void visit(LayoutQueryExpr& expr, const Type* context) override;
    void visit(NullExpr& expr, const Type* context) override;
    void visit(InitializerListExpr& expr, const Type* context) override;
    void visit(StringExpr& expr, const Type* context) override;
    void visit(UpdateExpr& expr, const Type* context) override;
    void visit(ForStmt& stmt) override;
    void visit(ContinueStmt& stmt) override;
    void visit(FallthroughStmt& stmt) override;
    void visit(EnumDeclStmt& stmt) override;
    void visit(StaticAssertStmt& stmt) override;
    void visit(TypeAliasDeclStmt& stmt) override;
    void visit(BinaryExpr& expr, const Type* context) override;
    void visit(AssignExpr& expr, const Type* context) override;
    void visit(SubscriptExpr& expr, const Type* context) override;
    void visit(UnaryExpr& expr, const Type* context) override;
    void visit(AddressOfExpr& expr, const Type* context) override;
	void visit(MemberAccessExpr& expr, const Type* context) override;
    void visit(DereferenceExpr& expr, const Type* context) override;
    void visit(CallExpr& expr, const Type* context) override;
	void visit(CastExpr& expr, const Type* context) override;
    void visit(SwitchStmt& stmt) override;
    void visit(CaseStmt& stmt) override;
    void visit(DefaultStmt& stmt) override;
    void visit(BreakStmt& stmt) override;
    void visit(ReturnStmt& stmt) override;
    void visit(VarDeclStmt& stmt) override;
    void visit(FunctionDeclStmt& stmt) override;
    void visit(IfStmt& stmt) override;
    void visit(BlockStmt& stmt) override;
    void visit(WhileStmt& stmt) override;
    void visit(ExpressionStmt& stmt) override;
	void visit(PlotStmt& stmt) override;
    void visit(PlotBlockStmt& stmt) override;
	void visit(PlotBeginStmt& stmt) override;
	void visit(PlotEndStmt& stmt) override;
    void visit(SetColorStmt& stmt) override;
	void visit(CmodeStmt& stmt) override;
	void visit(RpixStmt& stmt) override;
	void visit(HardwareLoopStmt& stmt) override;
	void visit(StructDefStmt& stmt) override;
    void visit(ConstDataStmt& stmt) override;
private:
    std::string m_result;
};
