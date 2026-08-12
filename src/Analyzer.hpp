#pragma once
#include "AST.hpp"
#include "DataSegment.hpp"
#include <map>
#include <vector>
#include <set>

struct FunctionSymbol {
    Type returnType;
    std::vector<Type> paramTypes;
};
struct StructMemberSymbol {
    Type type;
    int offset;
};
struct StructSymbol {
    std::string name;
    int totalSize = 0;
    std::map<std::string, StructMemberSymbol> members;
};

class Analyzer : public Visitor {
public:
    using LocalSymbolTable = std::map<SymbolId, Symbol>;

    Analyzer(DataSegmentManager& dataManager);
    void analyze(const std::vector<std::unique_ptr<Stmt>>& program);

    const std::map<std::string, FunctionSymbol>& getFunctionSymbols() const;
    const std::map<std::string, LocalSymbolTable>& getAllLocalSymbols() const;

    void registerFunctionSymbol(FunctionDeclStmt& stmt);
    void registerStructSymbol(StructDefStmt& stmt);
    void registerRomSymbol(ConstDataStmt& stmt);

    void visit(LiteralExpr& expr, const Type* context) override;
    void visit(VariableExpr& expr, const Type* context) override;
    void visit(BinaryExpr& expr, const Type* context) override;
    void visit(AssignExpr& expr, const Type* context) override;
    void visit(UnaryExpr& expr, const Type* context) override;
    void visit(AddressOfExpr& expr, const Type* context) override;
    void visit(DereferenceExpr& expr, const Type* context) override;
    void visit(SubscriptExpr& expr, const Type* context) override;
    void visit(MemberAccessExpr& expr, const Type* context) override;
    void visit(CallExpr& expr, const Type* context) override;
	void visit(CastExpr& expr, const Type* context) override;

    void visit(ReturnStmt& stmt) override;
    void visit(VarDeclStmt& stmt) override;
    void visit(FunctionDeclStmt& stmt) override;
    void visit(IfStmt& stmt) override;
    void visit(BlockStmt& stmt) override;
    void visit(WhileStmt& stmt) override;
    void visit(ExpressionStmt& stmt) override;
	void visit(PlotStmt& stmt) override;
	void visit(PlotBeginStmt& stmt) override;
	void visit(PlotEndStmt& stmt) override;
	void visit(SetColorStmt& stmt) override;
	void visit(CmodeStmt& stmt) override;
	void visit(RpixStmt& stmt) override;
	void visit(HardwareLoopStmt& stmt) override;
    void visit(StructDefStmt& stmt) override;
    void visit(ConstDataStmt& stmt) override;
    
    void visit(SwitchStmt& stmt) override;
    void visit(CaseStmt& stmt) override;
    void visit(DefaultStmt& stmt) override;
    void visit(BreakStmt& stmt) override;


private:
    void analyzeExpr(Expr& expr, const Type* context);
    void beginScope();
    void endScope();
    bool canFallThrough(const Stmt& stmt) const;
    bool blockCanFallThrough(const std::vector<std::unique_ptr<Stmt>>& statements) const;

    SymbolId createSymbolId();

    std::map<std::string, LocalSymbolTable> m_all_local_symbols;
    LocalSymbolTable* m_current_function_locals = nullptr;
    std::vector<std::map<std::string, SymbolId>> m_scopes;
    std::uint32_t m_next_symbol_id = 1;

    std::vector<bool> m_switch_context_stack;
    std::vector<std::set<long>> m_case_values_stack;

    DataSegmentManager& m_data_manager;
    Type m_currentFunctionType;
    bool m_isInPlottingContext = false;
    std::map<std::string, FunctionSymbol> m_function_symbols;
    std::map<std::string, StructSymbol> m_struct_symbols;
};
