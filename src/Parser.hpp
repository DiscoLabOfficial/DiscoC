#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Token.hpp"
#include "AST.hpp"
#include "TargetConfig.hpp"


class Parser {
public:
    Parser(const std::vector<Token>& tokens);
    std::vector<std::unique_ptr<Stmt>> parseProgram();
    const CompilerConfig& getConfig() const;
    CompilerConfig& getConfigForUpdate();

private:
    const std::vector<Token>& m_tokens;
    std::size_t m_current = 0;
    CompilerConfig m_config;

    void parseDirective();
    Type parseType();
    std::unique_ptr<Stmt> declaration();
    std::unique_ptr<Stmt> globalDeclaration();
    std::unique_ptr<Stmt> functionDeclaration(bool is_cached, Type baseType, Token name);
    std::unique_ptr<Stmt> structDeclaration();
    std::unique_ptr<Stmt> varDeclaration(Type pre_parsed_type, Token name);
	std::unique_ptr<Stmt> romConstDeclaration(Type type, Token name);
    std::unique_ptr<Stmt> statement();
    std::unique_ptr<Stmt> ifStatement();
    std::unique_ptr<Stmt> whileStatement(bool is_cached);
    std::unique_ptr<Stmt> forStatement(bool is_cached);
    std::unique_ptr<Stmt> switchStatement();
    std::unique_ptr<Stmt> plotStatement();
	std::unique_ptr<Stmt> plotBeginStatement();
	std::unique_ptr<Stmt> plotEndStatement();
	std::unique_ptr<Stmt> setColorStatement();
	std::unique_ptr<Stmt> setPlotOptionsStatement();
    std::unique_ptr<Stmt> flushPixelsStatement();
    std::unique_ptr<Stmt> drawStatement();
    std::unique_ptr<Stmt> returnStatement();
    std::unique_ptr<Stmt> blockStatement();
    std::unique_ptr<Expr> expression();
    std::unique_ptr<Expr> assignment();
    std::unique_ptr<Expr> equality();
    std::unique_ptr<Expr> comparison();
    std::unique_ptr<Expr> term();
    std::unique_ptr<Expr> factor();
    std::unique_ptr<Expr> postfix();
    std::unique_ptr<Expr> unary();
    std::unique_ptr<Expr> primary();
    bool isAtEnd(); Token peek(); Token peekNext(); Token previous(); Token advance();
    bool isAtStartOfDeclaration();
    bool check(TokenType type);
    bool match(const std::vector<TokenType>& types);
    Token consume(TokenType type, const std::string& message);
    std::int64_t parseIntegerLiteral(const Token& token, const std::string& context);
    void validateValueType(const Type& type, const Token& token, const std::string& context);
};
