#include "Parser.hpp"
#include <stdexcept>
#include "CompilerError.hpp"

Parser::Parser(const std::vector<Token>& tokens) : m_tokens(tokens) {}

std::vector<std::unique_ptr<Stmt>> Parser::parseProgram() {
    std::vector<std::unique_ptr<Stmt>> statements;
    while (!isAtEnd()) {
        if (peek().type == TokenType::KEYWORD_SET) {
            parseDirective();
        } else {
            statements.push_back(globalDeclaration());
        }
    }
    return statements;
}

const CompilerConfig& Parser::getConfig() const {
    return m_config;
}

CompilerConfig& Parser::getConfigForUpdate() {
    return m_config;
}

void Parser::parseDirective() {
    consume(TokenType::KEYWORD_SET, "Expect 'set'.");
    Token key = consume(TokenType::IDENTIFIER, "Expect configuration key.");
    consume(TokenType::EQUAL, "Expect '=' after key.");
    if (key.lexeme == "memory_mapping") {
        Token value = consume(TokenType::IDENTIFIER, "Expect mapping value (e.g., 'lorom' or 'hirom').");
        if (value.lexeme == "lorom") {
            m_config.mapping = MemoryMapping::LoROM;
            m_config.code_start_address = 0x8000;
        } else if (value.lexeme == "hirom") {
            m_config.mapping = MemoryMapping::HiROM;
            m_config.code_start_address = 0x408000;
        } else {
            throw std::runtime_error("Parse Error: Unsupported memory mapping '" + value.lexeme + "'.");
        }
    } else if (key.lexeme == "code_start_address") {
        Token value = consume(TokenType::LITERAL_INTEGER, "Expect an integer literal for the start address.");
        m_config.code_start_address = std::stol(value.lexeme, nullptr, 0);
    } else {
        throw std::runtime_error("Parse Error: Unknown configuration key '" + key.lexeme + "'.");
    }
    consume(TokenType::SEMICOLON, "Expect ';' after set directive.");
}

bool Parser::isAtStartOfDeclaration() {
    return peek().type == TokenType::KEYWORD_ROM ||
           peek().type == TokenType::KEYWORD_UNSIGNED ||
           peek().type == TokenType::KEYWORD_WORD ||
           peek().type == TokenType::KEYWORD_BYTE ||
           peek().type == TokenType::KEYWORD_VOID ||
		   peek().type == TokenType::KEYWORD_STRUCT ||
           peek().type == TokenType::KEYWORD_FAR;
}

Type Parser::parseType() {
    Type type;

    // Loop as long as we see type-related keywords we haven't processed yet.
    while (true) {
        if (peek().type == TokenType::KEYWORD_ROM && type.space == AddressSpace::NONE) {
            advance();
            type.space = AddressSpace::ROM;
        } else if (peek().type == TokenType::KEYWORD_STRUCT && type.base == BaseType::NONE) {
            advance();
            type.base = BaseType::STRUCT;
            type.structName = consume(TokenType::IDENTIFIER, "Expect struct name after 'struct' keyword.").lexeme;
        } else if (peek().type == TokenType::KEYWORD_UNSIGNED && !type.is_unsigned) {
            advance();
            type.is_unsigned = true;
        } else if (peek().type == TokenType::KEYWORD_FAR && !type.is_far) {
            advance();
            type.is_far = true;
        } else if (peek().type == TokenType::KEYWORD_WORD && type.base == BaseType::NONE) {
            advance();
            type.base = BaseType::WORD;
        } else if (peek().type == TokenType::KEYWORD_BYTE && type.base == BaseType::NONE) {
            advance();
            type.base = BaseType::BYTE;
        } else if (peek().type == TokenType::KEYWORD_VOID && type.base == BaseType::NONE) {
            advance();
            type.base = BaseType::VOID;
        } else {
            // If we don't see any more type keywords we can handle, break the loop.
            break;
        }
    }

    // After parsing keywords, ensure a base type was actually specified.
    if (type.base == BaseType::NONE) {
        throw std::runtime_error("Parse Error: Expected a base type specifier (word, byte, or void).");
    }

    // If 'rom' wasn't specified, the default is RAM.
    if (type.space == AddressSpace::NONE) {
        type.space = AddressSpace::RAM;
    }
    
    // Now, parse any pointer specifiers.
    while (match({TokenType::STAR})) {
        type.pointer_level++;
    }
    
    // Finally, validate the combination.
    if (type.is_far && type.pointer_level == 0) {
        throw std::runtime_error("Parse Error: The 'far' keyword can only be applied to pointer types.");
    }
    
    return type;
}


std::unique_ptr<Stmt> Parser::globalDeclaration() {
    bool is_cached = match({TokenType::KEYWORD_CACHE});
    if (peek().type == TokenType::KEYWORD_ROM && m_tokens[m_current + 1].type == TokenType::KEYWORD_CONST) {
        advance(); advance();
        if(is_cached) throw std::runtime_error("Parse Error: 'cache' cannot be applied to 'rom const' data.");
        Type type = parseType();
        type.space = AddressSpace::ROM;
        Token name = consume(TokenType::IDENTIFIER, "Expect identifier for rom const data.");
        return romConstDeclaration(type, name);
    } else if (peek().type == TokenType::KEYWORD_STRUCT) {
        return structDeclaration();
    }
    Type type = parseType();
    Token name = consume(TokenType::IDENTIFIER, "Expect identifier after type.");
    // It must be a function if we see a '('.
    consume(TokenType::LPAREN, "Expect '(' to begin function declaration.");
    return functionDeclaration(is_cached, type, name);
}

std::unique_ptr<Stmt> Parser::romConstDeclaration(Type type, Token name) {
    std::vector<std::unique_ptr<Expr>> initializers;

    // Check for array vs. single variable syntax
    if (match({TokenType::LBRACKET})) {
        // Array syntax: rom const word my_data[] = { ... };
        consume(TokenType::RBRACKET, "Expect ']' for rom data array.");
        consume(TokenType::EQUAL, "Expect '=' for rom data initialization.");
        consume(TokenType::LBRACE, "Expect '{' for rom data list.");
        if (!check(TokenType::RBRACE)) {
            do { initializers.push_back(expression()); } while (match({TokenType::COMMA}));
        }
        consume(TokenType::RBRACE, "Expect '}' to close rom data list.");
    } else {
        // Single variable syntax: rom const word MAX_VAL = 100;
        consume(TokenType::EQUAL, "Expect '=' for rom const initialization.");
        initializers.push_back(expression());
    }
    consume(TokenType::SEMICOLON, "Expect ';' after rom data declaration.");
    return std::make_unique<ConstDataStmt>(name, type, std::move(initializers));
}

std::unique_ptr<Stmt> Parser::functionDeclaration(bool is_cached, Type returnType, Token name) {
    std::vector<Parameter> params;
    if (!check(TokenType::RPAREN)) {
        do {
            if (!isAtStartOfDeclaration()) {
                throw std::runtime_error("Parse Error: Expect type specifier for parameter.");
            }
            Type paramType = parseType();
            Token paramName = consume(TokenType::IDENTIFIER, "Expect parameter name.");
            params.push_back({paramType, paramName, {}});
        } while (match({TokenType::COMMA}));
    }
    consume(TokenType::RPAREN, "Expect ')' after parameters.");
    auto body = statement();
    if (auto* block = dynamic_cast<BlockStmt*>(body.get())) {
        return std::make_unique<FunctionDeclStmt>(name, is_cached, returnType, std::move(params), std::move(block->statements));
    }
    throw std::runtime_error("Function body must be a block statement { ... }.");
}

std::unique_ptr<Stmt> Parser::structDeclaration() {
    consume(TokenType::KEYWORD_STRUCT, "Expect 'struct' keyword.");
    Token name = consume(TokenType::IDENTIFIER, "Expect struct name.");
    consume(TokenType::LBRACE, "Expect '{' to begin struct body.");

    std::vector<Member> members;
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        Type memberType = parseType();
        Token memberName = consume(TokenType::IDENTIFIER, "Expect member name.");
        consume(TokenType::SEMICOLON, "Expect ';' after struct member.");
        members.push_back({memberType, memberName});
    }

    consume(TokenType::RBRACE, "Expect '}' to close struct body.");
    consume(TokenType::SEMICOLON, "Expect ';' after struct declaration.");
    return std::make_unique<StructDefStmt>(name, std::move(members));
}

std::unique_ptr<Stmt> Parser::statement() {
    bool is_cached = match({TokenType::KEYWORD_CACHE});

    if (match({TokenType::KEYWORD_IF})) return ifStatement();
    if (match({TokenType::KEYWORD_FOR})) return forStatement(is_cached);
    if (match({TokenType::KEYWORD_WHILE})) return whileStatement(is_cached);
    if (match({TokenType::KEYWORD_SWITCH})) return switchStatement();
    if (match({TokenType::KEYWORD_BREAK})) {
        Token keyword = previous();
        consume(TokenType::SEMICOLON, "Expect ';' after 'break'.");
        return std::make_unique<BreakStmt>(keyword);
    }
    if (match({TokenType::LBRACE})) return blockStatement();
    if (match({TokenType::KEYWORD_RETURN})) return returnStatement();
    if (match({TokenType::KEYWORD_PLOT})) return plotStatement();
    if (match({TokenType::KEYWORD_PLOT_BEGIN})) return plotBeginStatement();
    if (match({TokenType::KEYWORD_PLOT_END})) return plotEndStatement();
    if (match({TokenType::KEYWORD_SET_COLOR})) return setColorStatement();
    if (match({TokenType::KEYWORD_SET_PLOT_OPTIONS})) return setPlotOptionsStatement();
    if (match({TokenType::KEYWORD_FLUSH_PIXELS})) return flushPixelsStatement();
    if (match({TokenType::KEYWORD_DRAW})) return drawStatement();

    if (isAtStartOfDeclaration()) {
        if (is_cached) throw std::runtime_error("Parse Error: 'cache' cannot precede a variable declaration.");
        Type type = parseType();
        Token name = consume(TokenType::IDENTIFIER, "Expect identifier for variable declaration.");
        return varDeclaration(type, name);
    }

    if (is_cached) throw std::runtime_error("Parse Error: 'cache' can only be applied to a for or while loop.");

    auto expr = expression();
    consume(TokenType::SEMICOLON, "Expect ';' after expression.");
    return std::make_unique<ExpressionStmt>(std::move(expr));
}

std::unique_ptr<Stmt> Parser::switchStatement() {
    consume(TokenType::LPAREN, "Expect '(' after 'switch'.");
    auto condition = expression();
    consume(TokenType::RPAREN, "Expect ')' after switch condition.");
    consume(TokenType::LBRACE, "Expect '{' to begin switch body.");

    std::vector<std::unique_ptr<Stmt>> body_statements;
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        if (match({TokenType::KEYWORD_CASE})) {
            Token keyword = previous();
            auto value = expression();
            consume(TokenType::COLON, "Expect ':' after case value.");
            body_statements.push_back(std::make_unique<CaseStmt>(keyword, std::move(value)));
        } else if (match({TokenType::KEYWORD_DEFAULT})) {
            Token keyword = previous();
            consume(TokenType::COLON, "Expect ':' after 'default'.");
            body_statements.push_back(std::make_unique<DefaultStmt>(keyword));
        } else {
            body_statements.push_back(statement());
        }
    }

    consume(TokenType::RBRACE, "Expect '}' to close switch body.");
    auto body_block = std::make_unique<BlockStmt>(std::move(body_statements));
    return std::make_unique<SwitchStmt>(std::move(condition), std::move(body_block));
}

std::unique_ptr<Stmt> Parser::varDeclaration(Type type, Token name) {
    std::unique_ptr<Expr> initializer = nullptr;
    if (match({TokenType::LBRACKET})) {
        if (type.pointer_level > 0) throw std::runtime_error("Arrays of pointers not supported yet.");
        Token size_token = consume(TokenType::LITERAL_INTEGER, "Expect array size.");
        type.array_size = std::stoi(size_token.lexeme);
        consume(TokenType::RBRACKET, "Expect ']' after array size.");
    } else if (match({TokenType::EQUAL})) {
        initializer = expression();
    }
    consume(TokenType::SEMICOLON, "Expect ';' after variable declaration.");
    return std::make_unique<VarDeclStmt>(type, name, std::move(initializer));
}


std::unique_ptr<Stmt> Parser::ifStatement() {
    consume(TokenType::LPAREN, "Expect '(' after 'if'.");
    auto condition = expression();
    consume(TokenType::RPAREN, "Expect ')' after if condition.");
    auto thenBranch = statement(); // DISCO TODO: this can be a single statement. what about declarations?
    std::unique_ptr<Stmt> elseBranch = nullptr; // DISCO TODO: dangling else
    if (match({TokenType::KEYWORD_ELSE})) elseBranch = statement();
    return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch));
}

std::unique_ptr<Stmt> Parser::whileStatement(bool is_cached) {
    consume(TokenType::LPAREN, "Expect '(' after 'while'.");
    auto condition = expression();
    consume(TokenType::RPAREN, "Expect ')' after while condition.");
    auto body = statement();
    return std::make_unique<WhileStmt>(is_cached, std::move(condition), std::move(body));
}

std::unique_ptr<Stmt> Parser::forStatement(bool is_cached) {
    consume(TokenType::LPAREN, "Expect '(' after 'for'.");
    std::unique_ptr<Stmt> initializer;
    if (match({TokenType::SEMICOLON})) {
    } else if (isAtStartOfDeclaration()) {
        Type type = parseType();
        Token name = consume(TokenType::IDENTIFIER, "Expect identifier in for-loop initializer.");
        initializer = varDeclaration(type, name);
    } else {
        initializer = std::make_unique<ExpressionStmt>(expression());
        consume(TokenType::SEMICOLON, "Expect ';' after for-loop expression initializer.");
    }
    
    std::unique_ptr<Expr> condition = nullptr;
    if (!check(TokenType::SEMICOLON)) {
        condition = expression();
    }
    consume(TokenType::SEMICOLON, "Expect ';' after for-loop condition.");

    std::unique_ptr<Expr> increment = nullptr;
    if (!check(TokenType::RPAREN)) {
        increment = expression();
    }
    consume(TokenType::RPAREN, "Expect ')' after for-loop clauses.");
    
    std::unique_ptr<Stmt> body = statement();
    
    if (increment) {
        auto statements = std::vector<std::unique_ptr<Stmt>>();
        if(auto* bodyAsBlock = dynamic_cast<BlockStmt*>(body.get())) {
            statements = std::move(bodyAsBlock->statements);
        } else {
            statements.push_back(std::move(body));
        }
        statements.push_back(std::make_unique<ExpressionStmt>(std::move(increment)));
        body = std::make_unique<BlockStmt>(std::move(statements));
    }

    if (!condition) {
        condition = std::make_unique<LiteralExpr>(Token(TokenType::LITERAL_INTEGER, "1", 0, 0));
    }
    
    auto loop = std::make_unique<WhileStmt>(is_cached, std::move(condition), std::move(body));
    
    if (initializer) {
        std::vector<std::unique_ptr<Stmt>> outerStmts;
        outerStmts.push_back(std::move(initializer));
        outerStmts.push_back(std::move(loop));
        return std::make_unique<BlockStmt>(std::move(outerStmts));
    }
    
    return loop;
}

std::unique_ptr<Stmt> Parser::returnStatement() {
    std::unique_ptr<Expr> value = nullptr;
    if (!check(TokenType::SEMICOLON)) { value = expression(); }
    consume(TokenType::SEMICOLON, "Expect ';' after return value.");
    return std::make_unique<ReturnStmt>(std::move(value));
}
std::unique_ptr<Stmt> Parser::blockStatement() {
    std::vector<std::unique_ptr<Stmt>> statements;
    while (!check(TokenType::RBRACE) && !isAtEnd()) { // FIXME: statement() should be declaration() or statement()
        statements.push_back(statement());
    }
    consume(TokenType::RBRACE, "Expect '}' after block.");
    return std::make_unique<BlockStmt>(std::move(statements));
}
std::unique_ptr<Expr> Parser::expression() { return assignment(); }
std::unique_ptr<Expr> Parser::assignment() {
    auto expr = equality();
    if (match({TokenType::EQUAL})) {
        auto value = assignment();
        return std::make_unique<AssignExpr>(std::move(expr), std::move(value));
    }
    return expr;
}
std::unique_ptr<Expr> Parser::equality()   { auto e=comparison(); while(match({TokenType::BANG_EQUAL,TokenType::EQUAL_EQUAL})){Token o=previous();auto r=comparison();e=std::make_unique<BinaryExpr>(std::move(e),o,std::move(r));}return e; }
std::unique_ptr<Expr> Parser::comparison() { auto e=term(); while(match({TokenType::GREATER,TokenType::GREATER_EQUAL,TokenType::LESS,TokenType::LESS_EQUAL})){Token o=previous();auto r=term();e=std::make_unique<BinaryExpr>(std::move(e),o,std::move(r));}return e; }
std::unique_ptr<Expr> Parser::term()       { auto e=factor(); while(match({TokenType::PLUS,TokenType::MINUS})){Token o=previous();auto r=factor();e=std::make_unique<BinaryExpr>(std::move(e),o,std::move(r));}return e; }
std::unique_ptr<Expr> Parser::factor()     { auto e=unary(); while(match({TokenType::STAR, TokenType::SLASH})){Token o=previous();auto r=unary();e=std::make_unique<BinaryExpr>(std::move(e),o,std::move(r));}return e; }
std::unique_ptr<Expr> Parser::unary() {
    if (match({TokenType::MINUS, TokenType::AMPERSAND, TokenType::STAR})) {
        Token op = previous();
        auto right = unary();
        if (op.type == TokenType::MINUS) return std::make_unique<UnaryExpr>(op, std::move(right));
        if (op.type == TokenType::AMPERSAND) return std::make_unique<AddressOfExpr>(op, std::move(right));
        return std::make_unique<DereferenceExpr>(op, std::move(right));
    }
    return postfix();
}
std::unique_ptr<Expr> Parser::postfix() {
    auto expr = primary();
    while (true) {
        if (match({TokenType::LPAREN})) {
            std::vector<std::unique_ptr<Expr>> arguments;
            if (!check(TokenType::RPAREN)) {
                do { arguments.push_back(expression()); } while (match({TokenType::COMMA}));
            }
            Token paren = consume(TokenType::RPAREN, "Expect ')' after arguments.");
            expr = std::make_unique<CallExpr>(std::move(expr), paren, std::move(arguments));
        } else if (match({TokenType::LBRACKET})) {
            Token bracket = previous();
            auto index = expression();
            consume(TokenType::RBRACKET, "Expect ']' after subscript index.");
            expr = std::make_unique<SubscriptExpr>(std::move(expr), bracket, std::move(index));
        } else if (match({TokenType::DOT})) {
            Token member = consume(TokenType::IDENTIFIER, "Expect member name after '.'.");
            expr = std::make_unique<MemberAccessExpr>(std::move(expr), member);
        } else { break; }
    }
    return expr;
}
std::unique_ptr<Expr> Parser::primary() {
    if (match({TokenType::LITERAL_INTEGER})) return std::make_unique<LiteralExpr>(previous());
    if (match({TokenType::IDENTIFIER})) return std::make_unique<VariableExpr>(previous());
    if (match({TokenType::LPAREN})) {
        Token paren_token = previous();
        // Check if it's a cast expression, e.g., (word)my_byte
        if (isAtStartOfDeclaration()) {
            Type cast_type = parseType();
            consume(TokenType::RPAREN, "Expect ')' after type in cast expression.");
            auto right = unary(); // Casts have high precedence, like other unary ops
            return std::make_unique<CastExpr>(paren_token, cast_type, std::move(right));
        } else {
            // It's a regular grouping parenthesis, I think
            auto expr = expression();
            consume(TokenType::RPAREN, "Expect ')' after expression.");
            return expr;
        }
    }
    throw CompilerError("Expected primary expression.", peek().line_number, peek().col_number);
}

bool Parser::isAtEnd() { return m_tokens[m_current].type == TokenType::END_OF_FILE; }
Token Parser::peek() { return m_tokens[m_current]; }
Token Parser::previous() { return m_tokens[m_current - 1]; }
Token Parser::advance() { if (!isAtEnd()) m_current++; return previous(); }
bool Parser::check(TokenType type) { if (isAtEnd()) return false; return peek().type == type; }
bool Parser::match(const std::vector<TokenType>& types) { for (auto t : types) { if (check(t)) { advance(); return true; } } return false; }
Token Parser::consume(TokenType type, const std::string& message) { 
    if (check(type)) return advance(); 
    Token prev = previous();
    throw CompilerError(message, prev.line_number, prev.col_number + (int)prev.lexeme.length());
}
std::unique_ptr<Stmt> Parser::plotStatement() {
    consume(TokenType::LPAREN, "Expect '(' after 'plot'.");
    auto x = expression();
    consume(TokenType::COMMA, "Expect ',' to separate plot arguments.");
    auto y = expression();
    consume(TokenType::RPAREN, "Expect ')' after plot arguments.");
    consume(TokenType::SEMICOLON, "Expect ';' after plot statement.");
    return std::make_unique<PlotStmt>(std::move(x), std::move(y));
}
std::unique_ptr<Stmt> Parser::plotBeginStatement() {
    consume(TokenType::SEMICOLON, "Expect ';' after plot_begin.");
    return std::make_unique<PlotBeginStmt>();
}
std::unique_ptr<Stmt> Parser::plotEndStatement() {
    consume(TokenType::SEMICOLON, "Expect ';' after plot_end.");
    return std::make_unique<PlotEndStmt>();
}
std::unique_ptr<Stmt> Parser::setColorStatement() {
    consume(TokenType::LPAREN, "Expect '(' after 'set_color'.");
    auto value = expression();
    consume(TokenType::RPAREN, "Expect ')' after color value.");
    consume(TokenType::SEMICOLON, "Expect ';' after set_color statement.");
    return std::make_unique<SetColorStmt>(std::move(value));
}
std::unique_ptr<Stmt> Parser::setPlotOptionsStatement() {
    consume(TokenType::LPAREN, "Expect '(' after 'set_plot_options'.");
    auto value = expression();
    consume(TokenType::RPAREN, "Expect ')' after options value.");
    consume(TokenType::SEMICOLON, "Expect ';' after set_plot_options statement.");
    return std::make_unique<CmodeStmt>(std::move(value));
}
std::unique_ptr<Stmt> Parser::flushPixelsStatement() {
    consume(TokenType::LPAREN, "Expect '(' after 'flush_pixels'.");
    consume(TokenType::RPAREN, "Expect ')' after 'flush_pixels'.");
    consume(TokenType::SEMICOLON, "Expect ';' after flush_pixels.");
    return std::make_unique<RpixStmt>();
}
std::unique_ptr<Stmt> Parser::drawStatement() {
    std::vector<std::unique_ptr<Stmt>> statements;
    consume(TokenType::KEYWORD_AT, "Expect 'at' in draw statement.");
    consume(TokenType::LPAREN, "Expect '(' after 'at'.");
    auto x = expression();
    consume(TokenType::COMMA, "Expect ',' to separate coordinates.");
    auto y = expression();
    consume(TokenType::RPAREN, "Expect ')' after coordinates.");
    if (match({TokenType::KEYWORD_WITH})) {
        consume(TokenType::KEYWORD_COLOR, "Expect 'color' after 'with'.");
        auto color = expression();
        statements.push_back(std::make_unique<SetColorStmt>(std::move(color)));
    }
    consume(TokenType::SEMICOLON, "Expect ';' after draw statement.");
    statements.push_back(std::make_unique<PlotStmt>(std::move(x), std::move(y)));
    return std::make_unique<BlockStmt>(std::move(statements));
}
