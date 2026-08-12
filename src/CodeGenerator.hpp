#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <map>
#include "AST.hpp"
#include "Opcodes.hpp"
#include "DataSegment.hpp"
#include "ObjectFile.hpp"
#include "Parser.hpp"
#include "Analyzer.hpp"

class CodeGenerator : public Visitor {
public:
    CodeGenerator(
        const std::map<std::string, std::map<std::string, Symbol>>& all_local_symbols,
        const std::map<std::string, FunctionSymbol>& global_function_symbols,
        const DataSegmentManager& data_manager,
		const CompilerConfig& config
    );

    ObjectFile generate(const std::vector<std::unique_ptr<Stmt>>& program);

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
	void dereferenceIfNeeded(Expr& expr);
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
    void emitByte(uint8_t byte);
    void emitWord(uint16_t word);
    void emitLiteral(long value, const Type& type);
	void emitMove(uint8_t destination, uint8_t source);
	void emitPush(uint8_t reg);
	void emitPop(uint8_t reg);
	void emitStore(uint8_t address_reg, uint8_t value_reg, bool byte);
	void emitImmediateArithmetic(uint8_t op_base, long value);
	void emitAdjustStack(std::size_t bytes, bool add);
	void emitFunctionEpilogue();
	void patchRelativeBranch(std::size_t patch_offset, std::size_t target_address, const Token& source);
	std::vector<uint8_t> visitAndMeasure(Stmt& stmt);
    std::string newLabel();
    int m_label_counter = 0;
    
    bool m_isInPlottingContext = false;

    const std::map<std::string, std::map<std::string, Symbol>>& m_all_local_symbols;
    const std::map<std::string, FunctionSymbol>& m_global_function_symbols;
    const DataSegmentManager& m_data_manager;
	const CompilerConfig& m_config;
    
    ObjectFile m_object_file;

    std::string m_currentFunctionName;
    std::map<std::string, Symbol> m_symbolTable;
    int m_stackOffset = 0;

    std::vector<std::vector<size_t>*> m_break_labels;
};
