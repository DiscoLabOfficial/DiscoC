#pragma once
#include "Token.hpp"
#include "Types.hpp"
#include "CompilerError.hpp"
#include <algorithm>
#include <vector>
#include <memory>

struct PlotCoordinateExpr; struct LayoutQueryExpr; struct EnumDeclStmt; struct StaticAssertStmt;
struct TypeAliasDeclStmt;
struct InitializerListExpr; struct StringExpr;
struct NullExpr; struct UpdateExpr; struct ForStmt; struct ContinueStmt; struct FallthroughStmt;
struct LiteralExpr; struct VariableExpr; struct BinaryExpr; struct AssignExpr;
struct UnaryExpr; struct MemberAccessExpr; struct ReturnStmt; struct VarDeclStmt; struct FunctionDeclStmt;
struct IfStmt; struct BlockStmt; struct WhileStmt; struct ExpressionStmt; struct PlotStmt;
struct PlotBlockStmt; struct PlotBeginStmt; struct PlotEndStmt; struct SetColorStmt; struct CmodeStmt; struct RpixStmt;
struct AddressOfExpr; struct DereferenceExpr; struct SubscriptExpr; struct ConstDataStmt; struct HardwareLoopStmt; struct CastExpr;
struct CallExpr; struct StructDefStmt; struct SwitchStmt; struct CaseStmt; struct DefaultStmt; struct BreakStmt;

struct Visitor {
    virtual ~Visitor() = default;
    virtual void visit(LiteralExpr& expr, const Type* context) = 0;
    virtual void visit(VariableExpr& expr, const Type* context) = 0;
    virtual void visit(PlotCoordinateExpr& expr, const Type* context) = 0;
    virtual void visit(LayoutQueryExpr& expr, const Type* context) = 0;
    virtual void visit(NullExpr& expr, const Type* context) = 0;
    virtual void visit(InitializerListExpr& expr, const Type* context) = 0;
    virtual void visit(StringExpr& expr, const Type* context) = 0;
    virtual void visit(UpdateExpr& expr, const Type* context) = 0;
    virtual void visit(ForStmt& stmt) = 0;
    virtual void visit(ContinueStmt& stmt) = 0;
    virtual void visit(FallthroughStmt& stmt) = 0;
    virtual void visit(EnumDeclStmt& stmt) = 0;
    virtual void visit(StaticAssertStmt& stmt) = 0;
    virtual void visit(TypeAliasDeclStmt& stmt) = 0;
    virtual void visit(BinaryExpr& expr, const Type* context) = 0;
    virtual void visit(AssignExpr& expr, const Type* context) = 0;
    virtual void visit(UnaryExpr& expr, const Type* context) = 0;
    virtual void visit(AddressOfExpr& expr, const Type* context) = 0;
    virtual void visit(DereferenceExpr& expr, const Type* context) = 0;
    virtual void visit(SubscriptExpr& expr, const Type* context) = 0;
    virtual void visit(MemberAccessExpr& expr, const Type* context) = 0;
    virtual void visit(CallExpr& expr, const Type* context) = 0;
    virtual void visit(CastExpr& expr, const Type* context) = 0;
    virtual void visit(SwitchStmt& stmt) = 0;
    virtual void visit(CaseStmt& stmt) = 0;
    virtual void visit(DefaultStmt& stmt) = 0;
    virtual void visit(BreakStmt& stmt) = 0;
    virtual void visit(ReturnStmt& stmt) = 0; virtual void visit(VarDeclStmt& stmt) = 0;
    virtual void visit(FunctionDeclStmt& stmt) = 0; virtual void visit(IfStmt& stmt) = 0;
    virtual void visit(BlockStmt& stmt) = 0; virtual void visit(WhileStmt& stmt) = 0;
    virtual void visit(ExpressionStmt& stmt) = 0;
    virtual void visit(PlotStmt& stmt) = 0;
    virtual void visit(PlotBlockStmt& stmt) = 0;
    virtual void visit(PlotBeginStmt& stmt) = 0;
    virtual void visit(PlotEndStmt& stmt) = 0;
    virtual void visit(SetColorStmt& stmt) = 0;
    virtual void visit(CmodeStmt& stmt) = 0;
    virtual void visit(RpixStmt& stmt) = 0;
    virtual void visit(HardwareLoopStmt& stmt) = 0;
    virtual void visit(StructDefStmt& stmt) = 0;
    virtual void visit(ConstDataStmt& stmt) = 0;
};

struct Expr {
    virtual ~Expr() = default;
    virtual void accept(Visitor& visitor, const Type* context) = 0;
    Token token = {TokenType::UNKNOWN, "", 0, 0};
    bool is_constant = false;
    std::int64_t constant_value = 0;
    Type result_type;
    Type address_type; // Resolved l-value storage address, independent of the stored value's pointee.
    static constexpr std::size_t MaxDepth = 256;
    std::size_t depth = 1;
protected:
    void includeChild(const Expr& child) {
        depth = std::max(depth, child.depth + 1);
        if (depth > MaxDepth)
            throw CompilerError("Expression depth exceeds the supported limit of 256.", token.line_number, token.col_number);
    }
};
struct Attribute {
    Token name;
    std::vector<std::unique_ptr<Expr>> arguments;
};
struct Stmt {
    virtual ~Stmt() = default;
    virtual void accept(Visitor& visitor) = 0;
    Token token = {TokenType::UNKNOWN, "", 0, 0};
    std::vector<Attribute> attributes;
    Linkage linkage = Linkage::External;
    std::string link_name;
};

struct TypeAliasDeclStmt : public Stmt {
    Type resolved_type;
    TypeAliasDeclStmt(Token name, Type type) : resolved_type(std::move(type)) {
        token = std::move(name);
    }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};

struct Parameter {
    Type type;
    Token name;
    SymbolId symbol_id;
};

// Represents a function call, e.g., `add(5, 10)`
struct CallExpr : public Expr {
    std::string resolved_symbol;
    std::unique_ptr<Expr> callee; // The expression that evaluates to a function (e.g., a VariableExpr 'add')
    Token paren; // The closing parenthesis, to report parity errors
    std::vector<std::unique_ptr<Expr>> arguments;
    CallExpr(std::unique_ptr<Expr> c, Token p, std::vector<std::unique_ptr<Expr>> args)
        : callee(std::move(c)), paren(std::move(p)), arguments(std::move(args)) {
        token = paren;
        includeChild(*callee);
        for (const auto& argument : arguments) includeChild(*argument);
    }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};

struct Member {
    Type type;
    Token name;
    std::unique_ptr<Expr> array_extent;
};

// Represents a struct definition, e.g., `struct Vec2 { word x; word y; };`
struct StructDefStmt : public Stmt {
    StructDefStmt(Token n, std::vector<Member> m) { token = n; members = std::move(m); }
    std::vector<Member> members;
    void accept(Visitor& v) override { v.visit(*this); }
};

// Represents member access, e.g., `my_vec.x`
struct LayoutQueryExpr : public Expr {
    Type queried_type;
    std::unique_ptr<Expr> queried_expression;
    Token member = {TokenType::UNKNOWN, "", 1, 1};
    LayoutQueryExpr(Token operation, Type type, std::unique_ptr<Expr> expression = {})
        : queried_type(std::move(type)), queried_expression(std::move(expression)) {
        token = std::move(operation);
        if (queried_expression) includeChild(*queried_expression);
    }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct EnumEntry { Token name; std::unique_ptr<Expr> value; };
struct EnumDeclStmt : public Stmt {
    Type underlying_type;
    std::vector<EnumEntry> entries;
    EnumDeclStmt(Token name, Type underlying, std::vector<EnumEntry> values)
        : underlying_type(std::move(underlying)), entries(std::move(values)) { token = std::move(name); }
    void accept(Visitor& v) override { v.visit(*this); }
};
struct StaticAssertStmt : public Stmt {
    std::unique_ptr<Expr> condition;
    StaticAssertStmt(Token keyword, std::unique_ptr<Expr> expression) : condition(std::move(expression)) { token = std::move(keyword); }
    void accept(Visitor& v) override { v.visit(*this); }
};
struct MemberAccessExpr : public Expr {
    std::unique_ptr<Expr> object;
    bool through_pointer = false;
    int member_offset = 0; // Filled in by the Analyzer
    MemberAccessExpr(std::unique_ptr<Expr> obj, Token m) : object(std::move(obj)) { token = m; includeChild(*object); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};

// All derived node classes now initialize their 'token' member.
struct LiteralExpr : public Expr {
    LiteralExpr(Token v) { token = std::move(v); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct VariableExpr : public Expr {
    SymbolId symbol_id;
    bool is_array_decay = false;
    VariableExpr(Token n) { token = std::move(n); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
// Explicit register l-value: not a lexical variable or a memory address.
struct PlotCoordinateExpr : public Expr {
    bool is_y;
    PlotCoordinateExpr(Token member, bool y) : is_y(y) { token = std::move(member); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct InitializerListExpr : public Expr {
    std::vector<std::unique_ptr<Expr>> elements;
    InitializerListExpr(Token brace, std::vector<std::unique_ptr<Expr>> values) : elements(std::move(values)) {
        token = std::move(brace); for (const auto& element : elements) includeChild(*element);
    }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct StringExpr : public Expr {
    explicit StringExpr(Token literal) { token = std::move(literal); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct NullExpr : public Expr {
    explicit NullExpr(Token keyword) { token = std::move(keyword); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
// Address is evaluated once; analysis resolves the underlying binary operator.
struct UpdateExpr : public Expr {
    std::unique_ptr<Expr> target, value;
    Token operation;
    Type operation_type;
    int pointer_stride = 0;
    bool postfix = false;
    UpdateExpr(Token op, std::unique_ptr<Expr> object, std::unique_ptr<Expr> rhs, bool post = false)
        : target(std::move(object)), value(std::move(rhs)), operation(op), postfix(post) {
        token = std::move(op); includeChild(*target); includeChild(*value);
    }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct BinaryExpr : public Expr {
    std::unique_ptr<Expr> left; std::unique_ptr<Expr> right;
    int pointer_stride = 0; // Validated by semantic analysis, never guessed by the backend.
    BinaryExpr(std::unique_ptr<Expr> l, Token o, std::unique_ptr<Expr> r) : left(std::move(l)), right(std::move(r)) { token = std::move(o); includeChild(*left); includeChild(*right); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct AssignExpr : public Expr {
    std::unique_ptr<Expr> name;
    std::unique_ptr<Expr> value;
    AssignExpr(std::unique_ptr<Expr> n, std::unique_ptr<Expr> v)
        : name(std::move(n)), value(std::move(v)) { token = name->token; includeChild(*name); includeChild(*value); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct UnaryExpr : public Expr {
    std::unique_ptr<Expr> right;
    UnaryExpr(Token o, std::unique_ptr<Expr> r) : right(std::move(r)) { token = std::move(o); includeChild(*right); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct BlockStmt : public Stmt {
    std::vector<std::unique_ptr<Stmt>> statements;
    BlockStmt(std::vector<std::unique_ptr<Stmt>> stmts) : statements(std::move(stmts)) { if (!this->statements.empty()) token = this->statements[0]->token; }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct IfStmt : public Stmt {
    std::unique_ptr<Expr> condition; std::unique_ptr<Stmt> thenBranch; std::unique_ptr<Stmt> elseBranch;
    IfStmt(std::unique_ptr<Expr> c, std::unique_ptr<Stmt> th, std::unique_ptr<Stmt> e)
        : condition(std::move(c)), thenBranch(std::move(th)), elseBranch(std::move(e)) { if(condition) token = condition->token; }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct HardwareLoopStmt : public Stmt {
    std::unique_ptr<LiteralExpr> count;
    std::unique_ptr<Stmt> body;
    HardwareLoopStmt(std::unique_ptr<LiteralExpr> c, std::unique_ptr<Stmt> b)
        : count(std::move(c)), body(std::move(b)) { if (this->body) token = this->body->token; }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct WhileStmt : public Stmt {
    bool is_cached = false;
    std::unique_ptr<Expr> condition;
    std::unique_ptr<Stmt> body;
    WhileStmt(bool cached, std::unique_ptr<Expr> c, std::unique_ptr<Stmt> b)
        : is_cached(cached), condition(std::move(c)), body(std::move(b)) { if (condition) token = condition->token; }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct ForStmt : public Stmt {
    bool is_cached = false;
    std::unique_ptr<Stmt> initializer;
    std::unique_ptr<Expr> condition, increment;
    std::unique_ptr<Stmt> body;
    ForStmt(Token keyword, bool cached, std::unique_ptr<Stmt> init, std::unique_ptr<Expr> cond,
            std::unique_ptr<Expr> step, std::unique_ptr<Stmt> block)
        : is_cached(cached), initializer(std::move(init)), condition(std::move(cond)), increment(std::move(step)), body(std::move(block)) { token = std::move(keyword); }
    void accept(Visitor& v) override { v.visit(*this); }
};
struct ContinueStmt : public Stmt {
    explicit ContinueStmt(Token keyword) { token = std::move(keyword); }
    void accept(Visitor& v) override { v.visit(*this); }
};
struct FallthroughStmt : public Stmt {
    explicit FallthroughStmt(Token keyword) { token = std::move(keyword); }
    void accept(Visitor& v) override { v.visit(*this); }
};
struct ReturnStmt : public Stmt {
    std::unique_ptr<Expr> value;
    ReturnStmt(std::unique_ptr<Expr> val) : value(std::move(val)) { if(value) token = value->token; }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct AggregateInitializer { int offset; Type type; std::unique_ptr<Expr> value; };
struct VarDeclStmt : public Stmt {
    bool is_global = false;
    bool is_extern = false;
    Type type; std::unique_ptr<Expr> initializer;
    SymbolId symbol_id;
    std::unique_ptr<Expr> array_extent;
    std::vector<AggregateInitializer> aggregate_initializers;
    bool inferred_extent = false;
    bool is_constexpr = false;
    int base_stack_offset = 0;
    VarDeclStmt(Type t, Token n, std::unique_ptr<Expr> i)
        : type(std::move(t)), initializer(std::move(i)) { token = std::move(n); }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct FunctionDeclStmt : public Stmt {
    bool is_cached = false;
    bool is_prototype = false;
    Type returnType;
    std::vector<Parameter> params;
    std::vector<std::unique_ptr<Stmt>> body;
    int total_local_alloc_size = 0;
    // Set by semantic analysis when control flow can reach the closing brace.
    // Code generation uses this to emit the ABI epilogue for implicit void
    // returns without adding dead epilogues after explicit returns.
    bool needs_implicit_return = false;
    FunctionDeclStmt(Token n, bool cached, Type rt, std::vector<Parameter> p,
                     std::vector<std::unique_ptr<Stmt>> b, bool prototype = false)
        : is_cached(cached), is_prototype(prototype), returnType(std::move(rt)),
          params(std::move(p)), body(std::move(b)) { token = std::move(n); }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct ExpressionStmt : public Stmt {
    std::unique_ptr<Expr> expression;
    ExpressionStmt(std::unique_ptr<Expr> expr) : expression(std::move(expr)) { if (expression) token = expression->token; }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct PlotStmt : public Stmt {
    std::unique_ptr<Expr> x;
    std::unique_ptr<Expr> y;
    PlotStmt(std::unique_ptr<Expr> x_coord, std::unique_ptr<Expr> y_coord)
        : x(std::move(x_coord)), y(std::move(y_coord)) { if(x) token = x->token; }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct PlotBlockStmt : public Stmt {
    std::unique_ptr<Stmt> body;
    PlotBlockStmt(Token keyword, std::unique_ptr<Stmt> block) : body(std::move(block)) { token = std::move(keyword); }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct PlotBeginStmt : public Stmt {
    PlotBeginStmt() = default;
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct PlotEndStmt : public Stmt {
    PlotEndStmt() = default;
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct SetColorStmt : public Stmt {
    std::unique_ptr<Expr> color_value;
    SetColorStmt(std::unique_ptr<Expr> value)
        : color_value(std::move(value)) { if(color_value) token = color_value->token; }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct CmodeStmt : public Stmt {
    std::unique_ptr<Expr> options_value;
    CmodeStmt(std::unique_ptr<Expr> value) : options_value(std::move(value)) { if(options_value) token = options_value->token; }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct RpixStmt : public Stmt {
    RpixStmt() = default;
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct AddressOfExpr : public Expr {
    std::unique_ptr<Expr> right;
    AddressOfExpr(Token t, std::unique_ptr<Expr> r) : right(std::move(r)) { token = std::move(t); includeChild(*right); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct DereferenceExpr : public Expr {
    std::unique_ptr<Expr> right;
    DereferenceExpr(Token t, std::unique_ptr<Expr> r) : right(std::move(r)) { token = std::move(t); includeChild(*right); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct SubscriptExpr : public Expr {
    std::unique_ptr<Expr> array;
    std::unique_ptr<Expr> index;
    // Filled by semantic analysis.  This is the size of one indexed element,
    // including pointees such as struct values; the pointer itself is always
    // two bytes and therefore cannot be used as the stride.
    int element_size = 0;
    SubscriptExpr(std::unique_ptr<Expr> a, Token bracket, std::unique_ptr<Expr> i)
        : array(std::move(a)), index(std::move(i)) { token = bracket; includeChild(*array); includeChild(*index); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct ConstDataStmt : public Stmt {
    Type type;
    bool is_array = false;
    std::unique_ptr<Expr> array_extent;
    std::vector<std::unique_ptr<Expr>> initializers;
    ConstDataStmt(Token n, Type t, std::vector<std::unique_ptr<Expr>> i, bool array = false)
        : type(std::move(t)), is_array(array), initializers(std::move(i)) { token = std::move(n); }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};

// Represents a 'break;' statement.
struct BreakStmt : public Stmt {
    BreakStmt(Token t) { token = std::move(t); }
    void accept(Visitor& v) override { v.visit(*this); }
};

// Represents a 'case <constant>:' label. It doesn't own statements itself.
struct CaseStmt : public Stmt {
    std::unique_ptr<Expr> value;
    // Filled by the textual assembly backend when it emits a switch.
    std::string label_name; 
    CaseStmt(Token t, std::unique_ptr<Expr> v) : value(std::move(v)) { token = std::move(t); }
    void accept(Visitor& v) override { v.visit(*this); }
};

// Represents a 'default:' label.
struct DefaultStmt : public Stmt {
    // Filled by the textual assembly backend when it emits a switch.
    std::string label_name; 
    DefaultStmt(Token t) { token = std::move(t); }
    void accept(Visitor& v) override { v.visit(*this); }
};

// Represents the entire 'switch (expr) { ... }' block.
// The body is a BlockStmt containing a mix of Case, Default, Break, and regular statements.
struct SwitchStmt : public Stmt {
    std::unique_ptr<Expr> condition;
    std::unique_ptr<BlockStmt> body;
    SwitchStmt(std::unique_ptr<Expr> c, std::unique_ptr<BlockStmt> b) 
        : condition(std::move(c)), body(std::move(b)) { if (condition) token = condition->token; }
    void accept(Visitor& v) override { v.visit(*this); }
};

struct CastExpr : public Expr {
    Type cast_to_type;
    std::unique_ptr<Expr> expression;
    bool implicit_conversion = false;
    CastExpr(Token t, Type type, std::unique_ptr<Expr> expr)
        : cast_to_type(std::move(type)), expression(std::move(expr)) { token = std::move(t); includeChild(*expression); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
