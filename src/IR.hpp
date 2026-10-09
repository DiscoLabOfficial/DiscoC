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
    // A storage-free sentinel also works when a C++14 container binds it by
    // reference; static constexpr data would require an out-of-line definition.
    enum : std::uint32_t { Invalid = std::numeric_limits<std::uint32_t>::max() };
    std::uint32_t value = Invalid;

    bool isValid() const { return value != Invalid; }
};

enum class IROpcode {
    // Parallel edge selection, not an eagerly evaluated expression. Operands
    // and targets are paired incoming values and predecessor block IDs.
    Phi,
    Constant,
    Address,
    PointerOffset,
    PointerCompare,
    Load,
    LoadIndirect,
    Store,
    StoreIndirect,
    // Ordered constant initialization of a proven, contiguous local RAM array.
    // Not memcpy: no source memory read, alias substitution, or volatile access.
    MemoryInitialize,
    Binary,
    // O2 internal pair: result selects operation ("/" or "%"), while
    // DivModResult selects its other component. Both components have type
    // word; the pair is owned by this function, not source-addressable memory.
    DivMod,
    DivModResult,
    // Backend-local fusion: unsigned extraction of one raw word bit (0/1),
    // independent of the source word's signedness. Not a source operator.
    BitExtract,
    Unary,
    Cast,
    Call,
    PlotCoordinateRead,
    PlotCoordinateWrite,
    PlotBegin,
    PlotEnd,
    Plot,
    SetColor,
    CMode,
    Rpix,
    Cache,
    HardwareLoop,
    HardwareLoopEnd,
    HardwareLoopLeave,
    Branch,
    CondBranch,
    Switch,
    Return,
    ReturnVoid,
    Unreachable,
};

struct IRHardwareEffects {
    std::uint16_t reads_registers = 0, writes_registers = 0;
    bool reads_color = false, writes_color = false, reads_por = false, writes_por = false;
    bool reads_framebuffer = false, writes_framebuffer = false, pixel_cache = false, rom_buffer = false;
    bool observable() const {
        return writes_registers || writes_color || writes_por || reads_framebuffer || writes_framebuffer || pixel_cache || rom_buffer;
    }
};

struct IRInstruction {
    IROpcode opcode = IROpcode::Constant;
    Type type;
    IRValueId result;
    std::vector<IRValueId> operands;
    std::vector<IRBlockId> targets;
    // Explicit O2 hardware-loop setup records the LOOP destination separately
    // from its initial successor. This is not another setup CFG edge.
    IRBlockId loop_target;
    std::uint32_t loop_id = 0;
    std::vector<std::int64_t> case_values;
    bool has_default_target = false;
    std::int64_t immediate = 0;
    std::string operation;
    std::string symbol;
    SymbolId symbol_id;
    bool memory_volatile = false;
    bool in_plot_context = false;
    // A representation-identical O2 copy at a loop preheader. Keeping a
    // distinct SSA lifetime permits a hot register and a cold spill location.
    bool is_live_range_split = false;
    std::vector<std::uint16_t> initialization_values;
    bool compiler_generated_loop = false;
    Token source = {TokenType::UNKNOWN, "", 0, 0};

    bool isTerminator() const;
    bool producesValue() const;
    IRHardwareEffects hardwareEffects() const {
        IRHardwareEffects e;
        switch (opcode) {
            case IROpcode::PlotCoordinateRead: e.reads_registers = static_cast<std::uint16_t>(1u << (immediate ? 2 : 1)); break;
            case IROpcode::PlotCoordinateWrite: e.writes_registers = static_cast<std::uint16_t>(1u << (immediate ? 2 : 1)); break;
            case IROpcode::Plot:
                e.reads_registers = 6; e.writes_registers = 2; e.reads_color = e.reads_por = true;
                e.writes_framebuffer = e.pixel_cache = true; break;
            case IROpcode::Rpix:
                e.reads_registers = 6; e.reads_por = true;
                e.reads_framebuffer = e.writes_framebuffer = e.pixel_cache = true; break;
            case IROpcode::SetColor:
                e.reads_color = e.reads_por = e.writes_color = true;
                if (operation == "rom.byte") { e.rom_buffer = true; e.writes_registers = 1u << 14; }
                break;
            case IROpcode::CMode: e.writes_por = true; break;
            case IROpcode::HardwareLoop:
            case IROpcode::HardwareLoopEnd:
            case IROpcode::HardwareLoopLeave:
                e.reads_registers = e.writes_registers = (1u << 12) | (1u << 13); break;
            default: break;
        }
        return e;
    }
};

struct IRBasicBlock {
    IRBlockId id;
    std::string label;
    std::vector<IRInstruction> instructions;
};

struct IRFunction {
    std::string name;
    std::string link_name;
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
    TargetKind target = TargetKind::GSU;
    BitmapConfig bitmap{};
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
    explicit IRLowerer(TargetKind target = TargetKind::GSU) : m_target(target) {}
    IRModule lower(const std::vector<std::unique_ptr<Stmt>>& program);

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
    void visit(PlotBlockStmt& stmt) override;
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
    IRValueId lowerLogical(BinaryExpr& expr);
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

    TargetKind m_target;
    IRModule m_module;
    std::uint32_t m_next_hardware_loop = 0;
    std::size_t m_current_function = 0;
    IRBlockId m_current_block;
    IRValueId m_last_value;
    std::vector<IRBlockId> m_break_targets;
    std::vector<IRBlockId> m_continue_targets;
    bool m_plot_context = false;
};

std::string dumpIR(const IRModule& module);
