#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Token.hpp"
#include "AST.hpp"
#include "TargetConfig.hpp"
#include "TypeAliases.hpp"
#include "Attributes.hpp"


class Parser {
public:
    Parser(const std::vector<Token>& tokens);
    std::vector<std::unique_ptr<Stmt>> parseProgram();
    void parsePreamble();
    void addImportedAlias(const TypeAliasBinding& binding);
    const CompilerConfig& getConfig() const;
    const std::vector<Token>& getImports() const { return m_imports; }
    const std::string& getModuleName() const { return m_module_name; }
    CompilerConfig& getConfigForUpdate();

private:
    const std::vector<Token>& m_tokens;
    std::size_t m_current = 0;
    std::size_t m_expression_depth = 0;
    std::size_t m_statement_depth = 0;
    CompilerConfig m_config;
    std::vector<Token> m_imports;
    std::string m_module_name;
    bool m_preamble_parsed = false;
    TypeAliasTable m_type_aliases = builtinTypeAliases();

    std::vector<Attribute> parseAttributes();
    bool preambleImportAhead() const;
    bool configurationEnabled(const std::vector<Attribute>& attributes) const;
    std::unique_ptr<Stmt> applyAttributes(std::unique_ptr<Stmt> statement, std::vector<Attribute> attributes,
                                        AttributeSite site = AttributeSite::Statement);
    std::unique_ptr<Stmt> typeAliasDeclaration(bool enabled);
    void validateValueName(const Token& name) const;
    void parseDirective();
    Type parseType();
    std::unique_ptr<Stmt> declaration();
    std::unique_ptr<Stmt> globalDeclaration();
    std::unique_ptr<Stmt> functionDeclaration(bool is_cached, Type baseType, Token name);
    std::unique_ptr<Stmt> structDeclaration();
    std::unique_ptr<Stmt> enumDeclaration();
    std::unique_ptr<Stmt> staticAssertion();
    std::unique_ptr<Stmt> varDeclaration(Type pre_parsed_type, Token name);
	std::unique_ptr<Stmt> romConstDeclaration(Type type, Token name);
    std::unique_ptr<Stmt> statement();
    std::unique_ptr<Stmt> ifStatement();
    std::unique_ptr<Stmt> whileStatement(bool is_cached);
    std::unique_ptr<Stmt> forStatement(bool is_cached);
    std::unique_ptr<Stmt> switchStatement();
    std::unique_ptr<Stmt> plotStatement();
    std::unique_ptr<Stmt> bitmapDeclaration();
    std::unique_ptr<Stmt> atStatement();
    std::unique_ptr<Stmt> optionsStatement();
    std::vector<std::unique_ptr<Stmt>> cursorWrites(std::unique_ptr<Expr> x, std::unique_ptr<Expr> y, const Token& source);
    std::unique_ptr<Stmt> drawStatement();
    std::unique_ptr<Stmt> returnStatement();
    std::unique_ptr<Stmt> blockStatement();
    std::unique_ptr<Expr> expression();
    std::unique_ptr<Expr> assignment();
    std::unique_ptr<Expr> logicalOr();
    std::unique_ptr<Expr> logicalAnd();
    std::unique_ptr<Expr> bitwiseOr();
    std::unique_ptr<Expr> bitwiseXor();
    std::unique_ptr<Expr> bitwiseAnd();
    std::unique_ptr<Expr> shift();
    std::unique_ptr<Expr> binaryLeft(std::unique_ptr<Expr> (Parser::*operand)(), const std::vector<TokenType>& operators);
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
