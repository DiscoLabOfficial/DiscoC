#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "AST.hpp"
#include "Analyzer.hpp"
#include "CompilerError.hpp"
#include "Types.hpp"

// IDs are stable handles into the owning IR function. They do not borrow
// addresses of vector elements, so adding instructions or blocks cannot
// invalidate references kept by a pass.
struct IRValueId {
    static constexpr std::uint32_t Invalid = 0;
    std::uint32_t value = Invalid;

    bool isValid() const { return value != Invalid; }
};

struct IRBlockId {
    static constexpr std::uint32_t Invalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t value = Invalid;

    bool isValid() const { return value != Invalid; }
};

enum class IROpcode {
    Constant,
    Address,
    Load,
    LoadIndirect,
    Store,
    StoreIndirect,
    Binary,
    Unary,
    Cast,
    Call,
    PlotBegin,
    PlotEnd,
    Plot,
    SetColor,
    CMode,
    Rpix,
    HardwareLoop,
    HardwareLoopEnd,
    Branch,
    CondBranch,
    Switch,
    Return,
    ReturnVoid,
    Unreachable,
};

struct IRInstruction {
    IROpcode opcode = IROpcode::Constant;
    Type type;
    IRValueId result;
    std::vector<IRValueId> operands;
    std::vector<IRBlockId> targets;
    std::vector<std::int64_t> case_values;
    bool has_default_target = false;
    std::int64_t immediate = 0;
    std::string operation;
    std::string symbol;
    SymbolId symbol_id;
    Token source = {TokenType::UNKNOWN, "", 0, 0};

    bool isTerminator() const;
    bool producesValue() const;
};

struct IRBasicBlock {
    IRBlockId id;
    std::string label;
    std::vector<IRInstruction> instructions;
};

struct IRFunction {
    std::string name;
    Type return_type;
    std::vector<Parameter> parameters;
    int total_local_alloc_size = 0;
    bool is_cached = false;
    bool needs_implicit_return = false;
    std::vector<IRBasicBlock> blocks;
    IRBlockId entry;
    std::uint32_t value_count = 0;
};

struct IRModule {
    std::vector<IRFunction> functions;
};

class IRVerifier {
public:
    static void verify(const IRModule& module);

private:
    static void verifyFunction(const IRFunction& function);
    static void fail(const std::string& message, const Token& source);
};

// Lowers the analyzed AST into a control-flow aware, typed IR. The IR is
// verified before the target-specific backend consumes it.
class IRLowerer : public Visitor {
public:
    IRModule lower(const std::vector<std::unique_ptr<Stmt>>& program);

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
    IRFunction& currentFunction();
    const IRBasicBlock& currentBlock() const;
    IRBasicBlock& currentBlock();
    IRBlockId createBlock(const std::string& label);
    IRValueId createValue();
    IRValueId lowerExpression(Expr& expr);
    IRValueId lowerAddress(Expr& expr);
    IRValueId emitValue(IROpcode opcode, const Type& type, const Token& source,
                        const std::vector<IRValueId>& operands = {},
                        const std::string& operation = {},
                        const std::string& symbol = {},
                        std::int64_t immediate = 0,
                        SymbolId symbol_id = {});
    void emitInstruction(IRInstruction instruction);
    void emitBranch(IRBlockId target, const Token& source);
    void emitConditionalBranch(IRValueId condition, IRBlockId true_target,
                               IRBlockId false_target, const Token& source);
    bool isTerminated() const;
    void lowerStatementList(const std::vector<std::unique_ptr<Stmt>>& statements);
    void lowerSwitchBody(SwitchStmt& stmt, IRValueId condition, IRBlockId end_block);
    void requireFunction(const Token& source) const;

    IRModule m_module;
    std::size_t m_current_function = 0;
    IRBlockId m_current_block;
    IRValueId m_last_value;
    std::vector<IRBlockId> m_break_targets;
};

std::string dumpIR(const IRModule& module);
