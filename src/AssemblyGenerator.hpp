#pragma once
#include <string>
#include <sstream>
#include <map>
#include <vector>
#include "AST.hpp"
#include "Analyzer.hpp"
#include "DataSegment.hpp"

class AssemblyGenerator : public Visitor {
public:
    AssemblyGenerator(
        const std::map<std::string, FunctionSymbol>& global_function_symbols,
        const std::map<std::string, Analyzer::LocalSymbolTable>& all_local_symbols,
        const DataSegmentManager& data_manager
    );

    std::string generate(const std::vector<std::unique_ptr<Stmt>>& program);

    // Expression visitors
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
	void dereferenceIfNeeded(Expr& expr, bool is_for_assignment = false);

    // Statement visitors
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
	void visit(PlotBeginStmt& stmt) override;
	void visit(PlotEndStmt& stmt) override;
	void visit(SetColorStmt& stmt) override;
	void visit(CmodeStmt& stmt) override;
	void visit(RpixStmt& stmt) override;
	void visit(HardwareLoopStmt& stmt) override;
	void visit(StructDefStmt& stmt) override;
    void visit(ConstDataStmt& stmt) override;

private:
    void emit(const std::string& s, const std::string& comment = "");
    void indent();
    std::string newLabel();
    void analyzeAndSetLocals(FunctionDeclStmt& stmt);

    const std::map<std::string, FunctionSymbol>& m_global_function_symbols;
    const std::map<std::string, Analyzer::LocalSymbolTable>& m_all_local_symbols;
    const DataSegmentManager& m_data_manager;
    std::stringstream m_out;
    int m_indent_level = 0;
    int m_label_counter = 0;
    
    // State for current function
    std::string m_currentFunctionName;
    std::vector<std::string> m_break_labels; 
    Analyzer::LocalSymbolTable m_symbolTable;
    int m_stackOffset = 0;
    bool m_isInPlottingContext = false;
	bool m_functionHasExplicitReturn = false;
};
