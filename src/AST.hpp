#pragma once
#include "Token.hpp"
#include "Types.hpp"
#include <vector>
#include <memory>

struct LiteralExpr; struct VariableExpr; struct BinaryExpr; struct AssignExpr;
struct UnaryExpr; struct MemberAccessExpr; struct ReturnStmt; struct VarDeclStmt; struct FunctionDeclStmt;
struct IfStmt; struct BlockStmt; struct WhileStmt; struct ExpressionStmt; struct PlotStmt;
struct PlotBeginStmt; struct PlotEndStmt; struct SetColorStmt; struct CmodeStmt; struct RpixStmt;
struct AddressOfExpr; struct DereferenceExpr; struct SubscriptExpr; struct ConstDataStmt; struct HardwareLoopStmt; struct CastExpr;
struct CallExpr; struct StructDefStmt; struct SwitchStmt; struct CaseStmt; struct DefaultStmt; struct BreakStmt;

struct Visitor {
    virtual ~Visitor() = default;
    virtual void visit(LiteralExpr& expr, const Type* context) = 0;
    virtual void visit(VariableExpr& expr, const Type* context) = 0;
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
    Type result_type;
};
struct Stmt {
    virtual ~Stmt() = default;
    virtual void accept(Visitor& visitor) = 0;
    Token token = {TokenType::UNKNOWN, "", 0, 0};
};

struct Parameter {
    Type type;
    Token name;
    SymbolId symbol_id;
};

// Represents a function call, e.g., `add(5, 10)`
struct CallExpr : public Expr {
    std::unique_ptr<Expr> callee; // The expression that evaluates to a function (e.g., a VariableExpr 'add')
    Token paren; // The closing parenthesis, to report parity errors
    std::vector<std::unique_ptr<Expr>> arguments;
    CallExpr(std::unique_ptr<Expr> c, Token p, std::vector<std::unique_ptr<Expr>> args)
        : callee(std::move(c)), paren(std::move(p)), arguments(std::move(args)) { token = paren; }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};

struct Member {
    Type type;
    Token name;
};

// Represents a struct definition, e.g., `struct Vec2 { word x; word y; };`
struct StructDefStmt : public Stmt {
    StructDefStmt(Token n, std::vector<Member> m) { token = n; members = std::move(m); }
    std::vector<Member> members;
    void accept(Visitor& v) override { v.visit(*this); }
};

// Represents member access, e.g., `my_vec.x`
struct MemberAccessExpr : public Expr {
    std::unique_ptr<Expr> object;
    int member_offset = 0; // Filled in by the Analyzer
    MemberAccessExpr(std::unique_ptr<Expr> obj, Token m) : object(std::move(obj)) { token = m; }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};

// All derived node classes now initialize their 'token' member.
struct LiteralExpr : public Expr {
    LiteralExpr(Token v) { token = std::move(v); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct VariableExpr : public Expr {
    SymbolId symbol_id;
    VariableExpr(Token n) { token = std::move(n); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct BinaryExpr : public Expr {
    std::unique_ptr<Expr> left; std::unique_ptr<Expr> right;
    BinaryExpr(std::unique_ptr<Expr> l, Token o, std::unique_ptr<Expr> r) : left(std::move(l)), right(std::move(r)) { token = std::move(o); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct AssignExpr : public Expr {
    std::unique_ptr<Expr> name;
    std::unique_ptr<Expr> value;
    AssignExpr(std::unique_ptr<Expr> n, std::unique_ptr<Expr> v)
        : name(std::move(n)), value(std::move(v)) { token = name->token; }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct UnaryExpr : public Expr {
    std::unique_ptr<Expr> right;
    UnaryExpr(Token o, std::unique_ptr<Expr> r) : right(std::move(r)) { token = std::move(o); }
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
struct ReturnStmt : public Stmt {
    std::unique_ptr<Expr> value;
    ReturnStmt(std::unique_ptr<Expr> val) : value(std::move(val)) { if(value) token = value->token; }
    void accept(Visitor& visitor) override { visitor.visit(*this); }
};
struct VarDeclStmt : public Stmt {
    Type type; std::unique_ptr<Expr> initializer;
    SymbolId symbol_id;
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
    AddressOfExpr(Token t, std::unique_ptr<Expr> r) : right(std::move(r)) { token = std::move(t); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct DereferenceExpr : public Expr {
    std::unique_ptr<Expr> right;
    DereferenceExpr(Token t, std::unique_ptr<Expr> r) : right(std::move(r)) { token = std::move(t); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct SubscriptExpr : public Expr {
    std::unique_ptr<Expr> array;
    std::unique_ptr<Expr> index;
    SubscriptExpr(std::unique_ptr<Expr> a, Token bracket, std::unique_ptr<Expr> i)
        : array(std::move(a)), index(std::move(i)) { token = bracket; }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
struct ConstDataStmt : public Stmt {
    Type type;
    std::vector<std::unique_ptr<Expr>> initializers;
    ConstDataStmt(Token n, Type t, std::vector<std::unique_ptr<Expr>> i)
        : type(std::move(t)), initializers(std::move(i)) { token = std::move(n); }
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
    CastExpr(Token t, Type type, std::unique_ptr<Expr> expr)
        : cast_to_type(std::move(type)), expression(std::move(expr)) { token = std::move(t); }
    void accept(Visitor& v, const Type* c) override { v.visit(*this, c); }
};
