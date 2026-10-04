#include "IntegerLiteral.hpp"
#include "Parser.hpp"
#include <stdexcept>
#include <algorithm>
#include "CompilerError.hpp"

namespace {

constexpr std::int64_t MaxArrayElements = 1'000'000;

// Bound recursion before descending, even when parentheses produce no AST
// node. The separate AST limit also bounds left-associative operator chains.
class ParseDepthGuard {
public:
    ParseDepthGuard(std::size_t& depth, const Token& source) : m_depth(depth) {
        if (depth >= 128)
            throw CompilerError("Parser nesting exceeds the supported limit of 128.", source);
        ++m_depth;
    }
    ~ParseDepthGuard() { --m_depth; }
    ParseDepthGuard(const ParseDepthGuard&) = delete;
    ParseDepthGuard& operator=(const ParseDepthGuard&) = delete;
private:
    std::size_t& m_depth;
};

} // namespace

Parser::Parser(const std::vector<Token>& tokens) : m_tokens(tokens) {}

bool Parser::preambleImportAhead() const {
    // Imported aliases are not available yet. Inspect only token structure:
    // parsing the first declaration's attribute expressions here would try
    // to resolve its casts before ModuleLoader has loaded the interfaces.
    auto cursor = m_current;
    while (cursor < m_tokens.size() && m_tokens[cursor].type == TokenType::AT_SIGN) {
        ++cursor;
        if (cursor >= m_tokens.size() ||
            (m_tokens[cursor].type != TokenType::IDENTIFIER &&
             m_tokens[cursor].type != TokenType::KEYWORD_CACHE)) return false;
        ++cursor;
        if (cursor < m_tokens.size() && m_tokens[cursor].type == TokenType::LPAREN) {
            std::size_t depth = 1;
            ++cursor;
            while (cursor < m_tokens.size() && depth != 0) {
                const auto kind = m_tokens[cursor].type;
                if (kind == TokenType::END_OF_FILE) return false;
                if (kind == TokenType::LPAREN) ++depth;
                else if (kind == TokenType::RPAREN) --depth;
                ++cursor;
            }
            if (depth != 0) return false;
        }
    }
    return cursor < m_tokens.size() && m_tokens[cursor].type == TokenType::KEYWORD_IMPORT;
}

void Parser::parsePreamble() {
    if (m_preamble_parsed) return;
    m_preamble_parsed = true;
    if (match({TokenType::KEYWORD_MODULE})) {
        m_module_name = consume(TokenType::IDENTIFIER, "Expect module name.").lexeme;
        consume(TokenType::SEMICOLON, "Expect ';' after module name.");
    }
    std::size_t imports = 0;
    while (!isAtEnd()) {
        if (!preambleImportAhead()) break;
        auto attributes = parseAttributes();
        consume(TokenType::KEYWORD_IMPORT, "Expect import after attributes.");
        for (const auto& attribute : attributes)
            if (attribute.name.lexeme != "cfg")
                throw CompilerError("Only @cfg is supported on imports.", attribute.name);
        const bool enabled = configurationEnabled(attributes);
        auto path = consume(TokenType::LITERAL_STRING, "Expect quoted .dc source or .dci interface path.");
        if (path.lexeme.size() > 4096) throw CompilerError("Import path exceeds 4096 bytes.", path);
        if (path.lexeme.empty() || path.lexeme.front() == '/' || path.lexeme.front() == '\\' ||
            path.lexeme.find(':') != std::string::npos ||
            std::any_of(path.lexeme.begin(), path.lexeme.end(), [](char value) { return static_cast<unsigned char>(value) < 32 || value == 127; }))
            throw CompilerError("Imports require a relative .dc source or .dci interface path.", path);
        const auto dot = path.lexeme.find_last_of('.');
        if (dot == std::string::npos || (path.lexeme.substr(dot) != ".dc" && path.lexeme.substr(dot) != ".dci"))
            throw CompilerError("Imports require a .dc source or .dci interface path.", path);
        if (++imports > 128) throw CompilerError("Import count exceeds 128.", path);
        consume(TokenType::SEMICOLON, "Expect ';' after import.");
        if (enabled) m_imports.push_back(std::move(path));
    }
}

void Parser::addImportedAlias(const TypeAliasBinding& binding) {
    const auto found = m_type_aliases.find(binding.declaration.lexeme);
    if (found != m_type_aliases.end()) {
        const auto& original = found->second.declaration;
        if (original.source_path != binding.declaration.source_path ||
            original.line_number != binding.declaration.line_number ||
            original.col_number != binding.declaration.col_number)
            throw CompilerError("Conflicting type alias '" + binding.declaration.lexeme + "'.", binding.declaration);
        return;
    }
    if (m_type_aliases.size() >= MaxTypeAliases)
        throw CompilerError("Type alias count exceeds the supported limit of 4096.", binding.declaration);
    m_type_aliases.emplace(binding.declaration.lexeme, binding);
}

void Parser::validateValueName(const Token& name) const {
    if (m_type_aliases.count(name.lexeme))
        throw CompilerError("Type alias name cannot be used as a value name: " + name.lexeme, name);
}

std::vector<std::unique_ptr<Stmt>> Parser::parseProgram() {
    parsePreamble();
    std::vector<std::unique_ptr<Stmt>> statements;
    while (!isAtEnd()) {
        if (check(TokenType::KEYWORD_MODULE))
            throw CompilerError("module must appear once before imports and declarations.", peek());
        if (check(TokenType::KEYWORD_IMPORT))
            throw CompilerError("Imports must precede declarations.", peek());
        if (check(TokenType::KEYWORD_SET)) parseDirective();
        auto declaration = globalDeclaration();
        if (configurationEnabled(declaration->attributes))
            statements.push_back(std::move(declaration));
    }
    return statements;
}

const CompilerConfig& Parser::getConfig() const { return m_config; }
CompilerConfig& Parser::getConfigForUpdate() { return m_config; }

void Parser::parseDirective() {
    throw CompilerError("Source-level set configuration has been removed; use --memory-mapping, --execution-memory and --origin on discc (or --origin on discld).", peek());
}

bool Parser::isAtStartOfDeclaration() {
    return (peek().type == TokenType::IDENTIFIER && m_type_aliases.count(peek().lexeme)) ||
           peek().type == TokenType::KEYWORD_ENUM ||
           peek().type == TokenType::KEYWORD_ROM ||
           peek().type == TokenType::KEYWORD_RAM ||
           peek().type == TokenType::KEYWORD_CONST ||
           peek().type == TokenType::KEYWORD_VOLATILE ||
           peek().type == TokenType::KEYWORD_BOOL ||
           peek().type == TokenType::KEYWORD_UNSIGNED ||
           peek().type == TokenType::KEYWORD_WORD ||
           peek().type == TokenType::KEYWORD_BYTE ||
           peek().type == TokenType::KEYWORD_VOID ||
		   peek().type == TokenType::KEYWORD_STRUCT ||
           peek().type == TokenType::KEYWORD_FAR;
}

Type Parser::parseType() {
    Type type;
    bool from_alias = false;
    bool requested_far = false;
    while (true) {
        if (peek().type == TokenType::KEYWORD_CONST && !type.is_const) {
            advance(); type.is_const = true;
        } else if (peek().type == TokenType::KEYWORD_VOLATILE && !type.is_volatile) {
            advance(); type.is_volatile = true;
        } else if (peek().type == TokenType::KEYWORD_RAM && type.space == AddressSpace::NONE) {
            advance(); type.space = AddressSpace::RAM;
        } else if (peek().type == TokenType::KEYWORD_ROM && type.space == AddressSpace::NONE) {
            advance(); type.space = AddressSpace::ROM;
        } else if (peek().type == TokenType::KEYWORD_ENUM && type.base == BaseType::NONE) {
            advance(); type.base = BaseType::WORD;
            type.enum_name = consume(TokenType::IDENTIFIER, "Expect enum type name.").lexeme;
        } else if (peek().type == TokenType::KEYWORD_STRUCT && type.base == BaseType::NONE) {
            advance(); type.base = BaseType::STRUCT;
            type.structName = consume(TokenType::IDENTIFIER, "Expect struct name after 'struct' keyword.").lexeme;
        } else if (peek().type == TokenType::KEYWORD_UNSIGNED && !type.is_unsigned) {
            if (from_alias) throw CompilerError("Signedness is fixed by a type alias; select u8/u16 or use a cast.", peek());
            advance(); type.is_unsigned = true;
        } else if (peek().type == TokenType::KEYWORD_FAR && !requested_far) {
            advance(); requested_far = true;
        } else if (peek().type == TokenType::KEYWORD_WORD && type.base == BaseType::NONE) {
            advance(); type.base = BaseType::WORD;
        } else if (peek().type == TokenType::KEYWORD_BOOL && type.base == BaseType::NONE) {
            advance(); type.base = BaseType::BOOL;
        } else if (peek().type == TokenType::KEYWORD_BYTE && type.base == BaseType::NONE) {
            advance(); type.base = BaseType::BYTE;
        } else if (peek().type == TokenType::KEYWORD_VOID && type.base == BaseType::NONE) {
            advance(); type.base = BaseType::VOID;
        } else if (peek().type == TokenType::IDENTIFIER && type.base == BaseType::NONE) {
            const auto name = advance();
            const auto binding = m_type_aliases.find(name.lexeme);
            if (binding == m_type_aliases.end())
                throw CompilerError("Unknown type alias '" + name.lexeme + "'.", name);
            if (type.is_unsigned)
                throw CompilerError("Signedness is fixed by a type alias; select u8/u16 or use a cast.", name);
            auto resolved = binding->second.type;
            if (type.space != AddressSpace::NONE) {
                if (resolved.pointer_level > 0 && type.space != resolved.space)
                    throw CompilerError("A pointer alias fixes its address space.", name);
                resolved.space = type.space;
            }
            resolved.is_const = resolved.is_const || type.is_const;
            resolved.is_volatile = resolved.is_volatile || type.is_volatile;
            type = std::move(resolved);
            from_alias = true;
        } else break;
    }

    if (type.base == BaseType::NONE)
        throw CompilerError("Parse Error: Expected a base type specifier (word, byte, void, or type alias).", peek());
    if (type.space == AddressSpace::NONE) type.space = AddressSpace::RAM;

    while (match({TokenType::STAR})) {
        if (type.pointer_level == MaxPointerDepth)
            throw CompilerError("Pointer depth exceeds the supported limit of 32.", previous());
        ++type.pointer_level;
        type.pointee_qualifiers.push_back({type.is_const, type.is_volatile});
        type.is_const = false; type.is_volatile = false;
        bool reach = false;
        while (true) {
            if (!reach && match({TokenType::KEYWORD_FAR})) reach = true;
            else if (!type.is_const && match({TokenType::KEYWORD_CONST})) type.is_const = true;
            else if (!type.is_volatile && match({TokenType::KEYWORD_VOLATILE})) type.is_volatile = true;
            else break;
        }
        type.pointer_reach.push_back(reach);
        if (!type.pointer_spaces.empty()) type.pointer_spaces.push_back(AddressSpace::RAM);
    }
    if (requested_far && type.pointer_level == 0)
        throw CompilerError("Parse Error: The 'far' keyword can only be applied to pointer types.", peek());
    if (type.pointer_level > 0) {
        if (requested_far) type.pointer_reach.back() = true;
        normalizePointerLayers(type);
    }
    return type;
}

std::vector<Attribute> Parser::parseAttributes() {
    std::vector<Attribute> attributes;
    std::vector<std::string> names;
    while (match({TokenType::AT_SIGN})) {
        const auto name = advance();
        if (name.type != TokenType::IDENTIFIER && name.type != TokenType::KEYWORD_CACHE)
            throw CompilerError("Expect attribute name after '@'.", name);
        if (attributes.size() >= 32 || std::find(names.begin(), names.end(), name.lexeme) != names.end())
            throw CompilerError("Duplicate attribute or attribute limit exceeded.", name);
        names.push_back(name.lexeme);
        Attribute attribute{name, {}};
        if (match({TokenType::LPAREN})) {
            if (!check(TokenType::RPAREN)) {
                do {
                    if (attribute.arguments.size() >= 16)
                        throw CompilerError("Too many attribute arguments.", name);
                    attribute.arguments.push_back(expression());
                } while (match({TokenType::COMMA}));
            }
            consume(TokenType::RPAREN, "Expect ')' after attribute arguments.");
        }
        attributes.push_back(std::move(attribute));
    }
    return attributes;
}

bool Parser::configurationEnabled(const std::vector<Attribute>& attributes) const {
    for (const auto& attribute : attributes) {
        if (attribute.name.lexeme != "cfg") continue;
        const auto* target = attribute.arguments.size() == 1
            ? dynamic_cast<const VariableExpr*>(attribute.arguments.front().get()) : nullptr;
        if (!target || (target->token.lexeme != "gsu" && target->token.lexeme != "spc700"))
            throw CompilerError("@cfg requires exactly one target name: gsu or spc700.", attribute.name);
        return target->token.lexeme == (m_config.target == TargetKind::GSU ? "gsu" : "spc700");
    }
    return true;
}

std::unique_ptr<Stmt> Parser::applyAttributes(std::unique_ptr<Stmt> statement, std::vector<Attribute> attributes, AttributeSite site) {
    const bool enabled = configurationEnabled(attributes);
    for (const auto& attribute : attributes) {
        const auto& name = attribute.name;
        const auto count = attribute.arguments.size();
        auto* function = dynamic_cast<FunctionDeclStmt*>(statement.get());
        auto* loop = dynamic_cast<WhileStmt*>(statement.get());
        auto* for_loop = dynamic_cast<ForStmt*>(statement.get());
        auto* block = dynamic_cast<BlockStmt*>(statement.get());
        if (block && block->statements.size() == 2) loop = dynamic_cast<WhileStmt*>(block->statements.back().get());
        if (name.lexeme == "cfg") {
            if (site != AttributeSite::TopLevel)
                throw CompilerError("@cfg is only supported on top-level declarations or imports.", name);
        } else if (name.lexeme == "cache") {
            if (count != 0 || (!function && !loop && !for_loop) || (function && function->is_prototype))
                throw CompilerError("@cache requires a function definition or loop and no arguments.", name);
            if (function) function->is_cached = true;
            if (loop) loop->is_cached = true;
            if (for_loop) for_loop->is_cached = true;
        } else if (name.lexeme == "packed" || name.lexeme == "align") {
            if (!dynamic_cast<StructDefStmt*>(statement.get()) || count != (name.lexeme == "packed" ? 0u : 1u))
                throw CompilerError("Layout attributes require a struct definition; @align takes one argument.", name);
        } else if (name.lexeme == "target") {
            if (!function || count != 1)
                throw CompilerError("@target requires one target name on a function.", name);
            const auto* target = dynamic_cast<VariableExpr*>(attribute.arguments.front().get());
            if (!target || (target->token.lexeme != "gsu" && target->token.lexeme != "spc700") ||
                (enabled && target->token.lexeme != (m_config.target == TargetKind::GSU ? "gsu" : "spc700")))
                throw CompilerError("@target does not match the compilation target.", name);
        } else if (isReservedAttribute(name.lexeme)) {
            throw CompilerError("Attribute '@" + name.lexeme + "' is reserved; its ABI/lowering contract is not implemented yet.", name);
        } else {
            throw CompilerError("Unsupported attribute '@" + name.lexeme + "'.", name);
        }
    }
    statement->attributes = std::move(attributes);
    return statement;
}

std::unique_ptr<Stmt> Parser::globalDeclaration() {
    auto attributes = parseAttributes();
    if (check(TokenType::KEYWORD_IMPORT)) throw CompilerError("Imports must precede declarations.", peek());
    if (match({TokenType::KEYWORD_TYPE})) {
        const bool enabled = configurationEnabled(attributes);
        auto declaration = typeAliasDeclaration(enabled);
        return applyAttributes(std::move(declaration), std::move(attributes), AttributeSite::TopLevel);
    }
    if (match({TokenType::KEYWORD_STATIC_ASSERT}))
        return applyAttributes(staticAssertion(), std::move(attributes), AttributeSite::TopLevel);
    if (check(TokenType::KEYWORD_ENUM) && m_current + 2 < m_tokens.size() &&
        (m_tokens[m_current + 2].type == TokenType::LBRACE || m_tokens[m_current + 2].type == TokenType::COLON))
        return applyAttributes(enumDeclaration(), std::move(attributes), AttributeSite::TopLevel);
    const bool constant = match({TokenType::KEYWORD_CONSTEXPR});
    const bool internal = match({TokenType::KEYWORD_INTERNAL});
    const bool exported = !internal && match({TokenType::KEYWORD_EXPORT});
    const bool external_declaration = match({TokenType::KEYWORD_EXTERN});
    if (internal && external_declaration)
        throw CompilerError("'internal' and 'extern' cannot be combined.", peek().line_number, peek().col_number);
    const auto apply_linkage = [&](std::unique_ptr<Stmt> declaration) {
        declaration->linkage = internal ? Linkage::Internal : Linkage::External;
        return applyAttributes(std::move(declaration), std::move(attributes), AttributeSite::TopLevel);
    };
    bool is_cached = match({TokenType::KEYWORD_CACHE});
    if (peek().type == TokenType::KEYWORD_STRUCT && m_current + 2 < m_tokens.size() &&
        m_tokens[m_current + 2].type == TokenType::LBRACE) {
        if (internal || exported || external_declaration)
            throw CompilerError("Struct definitions do not have linkage.", peek().line_number, peek().col_number);
        return applyAttributes(structDeclaration(), std::move(attributes), AttributeSite::TopLevel);
    }
    Type type = parseType();
    if (constant) type.is_const = true;
    Token name = consume(TokenType::IDENTIFIER, "Expect identifier after type.");
    validateValueName(name);
    if (constant && check(TokenType::LPAREN)) throw CompilerError("constexpr declares a scalar constant, not a function.", name);
    if (type.space == AddressSpace::ROM && type.pointer_level == 0 && !check(TokenType::LPAREN)) {
        if (is_cached) throw CompilerError("'cache' cannot be applied to ROM data.", name);
        validateValueType(type, name, "ROM data");
        if (external_declaration) throw CompilerError("External ROM declarations are not supported yet.", name);
        return apply_linkage(romConstDeclaration(type, name));
    }
    if (!check(TokenType::LPAREN)) {
        if (is_cached) throw CompilerError("'cache' cannot be applied to global storage.", name);
        auto declaration = varDeclaration(type, name);
        static_cast<VarDeclStmt&>(*declaration).is_global = true;
        static_cast<VarDeclStmt&>(*declaration).is_constexpr = constant;
        static_cast<VarDeclStmt&>(*declaration).is_extern = external_declaration;
        return apply_linkage(std::move(declaration));
    }
    // It must be a function if we see a '('.
    consume(TokenType::LPAREN, "Expect '(' to begin function declaration.");
    auto function = functionDeclaration(is_cached, type, name);
    if (external_declaration && !static_cast<FunctionDeclStmt&>(*function).is_prototype)
        throw CompilerError("An extern function declaration cannot have a body.", name);
    return apply_linkage(std::move(function));
}

std::unique_ptr<Stmt> Parser::romConstDeclaration(Type type, Token name) {
    std::vector<std::unique_ptr<Expr>> initializers;
    bool is_array = false;
    std::unique_ptr<Expr> extent;
    const auto finish = [&]() {
        auto result = std::make_unique<ConstDataStmt>(name, type, std::move(initializers), is_array);
        result->array_extent = std::move(extent);
        return result;
    };

    // Check for array vs. single variable syntax
    if (match({TokenType::LBRACKET})) {
        is_array = true;
        if (!check(TokenType::RBRACKET)) extent = expression();
        // Extents are resolved by the same typed constant evaluator as RAM arrays.
        consume(TokenType::RBRACKET, "Expect ']' for rom data array.");
        consume(TokenType::EQUAL, "Expect '=' for rom data initialization.");
        if (match({TokenType::LITERAL_STRING})) {
            if (type.base != BaseType::BYTE) throw CompilerError("String storage requires a byte array.", name);
            const auto text = previous();
            for (const unsigned char byte : text.lexeme) {
                const int value = type.is_unsigned || byte < 128 ? byte : static_cast<int>(byte) - 256;
                initializers.push_back(std::make_unique<LiteralExpr>(Token(TokenType::LITERAL_INTEGER, std::to_string(value), text)));
            }
            initializers.push_back(std::make_unique<LiteralExpr>(Token(TokenType::LITERAL_INTEGER, "0", text)));
            consume(TokenType::SEMICOLON, "Expect ';' after string storage.");
            return finish();
        }
        consume(TokenType::LBRACE, "Expect '{' for rom data list.");
        if (!check(TokenType::RBRACE)) {
            do {
                if (initializers.size() >= static_cast<std::size_t>(MaxArrayElements)) {
                    throw CompilerError("ROM data initializer list is too large.",
                                        peek().line_number, peek().col_number);
                }
                initializers.push_back(expression());
            } while (match({TokenType::COMMA}));
        }
        consume(TokenType::RBRACE, "Expect '}' to close rom data list.");
    } else {
        // Single variable syntax: rom const word MAX_VAL = 100;
        consume(TokenType::EQUAL, "Expect '=' for rom const initialization.");
        initializers.push_back(expression());
    }
    consume(TokenType::SEMICOLON, "Expect ';' after rom data declaration.");
    return finish();
}

std::unique_ptr<Stmt> Parser::functionDeclaration(bool is_cached, Type returnType, Token name) {
    std::vector<Parameter> params;
    if (!check(TokenType::RPAREN)) {
        do {
            if (!isAtStartOfDeclaration()) {
                throw CompilerError("Parse Error: Expect type specifier for parameter.",
                                    peek().line_number, peek().col_number);
            }
            Type paramType = parseType();
            validateValueType(paramType, peek(), "function parameter");
            Token paramName = consume(TokenType::IDENTIFIER, "Expect parameter name.");
            validateValueName(paramName);
            params.push_back({paramType, paramName, {}});
        } while (match({TokenType::COMMA}));
    }
    consume(TokenType::RPAREN, "Expect ')' after parameters.");
    if (match({TokenType::SEMICOLON})) {
        if (is_cached) {
            throw CompilerError("'cache' cannot be applied to a function prototype.",
                                name);
        }
        return std::make_unique<FunctionDeclStmt>(
            name, false, returnType, std::move(params),
            std::vector<std::unique_ptr<Stmt>>{}, true);
    }
    auto body = statement();
    if (auto* block = dynamic_cast<BlockStmt*>(body.get())) {
        return std::make_unique<FunctionDeclStmt>(name, is_cached, returnType, std::move(params), std::move(block->statements));
    }
    throw CompilerError("Function body must be a block statement { ... }.",
                        name);
}

std::unique_ptr<Stmt> Parser::typeAliasDeclaration(bool enabled) {
    const auto name = consume(TokenType::IDENTIFIER, "Expect type alias name.");
    if (enabled && m_type_aliases.count(name.lexeme))
        throw CompilerError("Type alias already declared or reserved: " + name.lexeme, name);
    consume(TokenType::EQUAL, "Expect '=' in type alias.");
    auto type = parseType();
    consume(TokenType::SEMICOLON, "Expect ';' after type alias (array/function aliases are unsupported).");
    if (enabled) {
        if (m_type_aliases.size() >= MaxTypeAliases)
            throw CompilerError("Type alias count exceeds the supported limit of 4096.", name);
        m_type_aliases.emplace(name.lexeme, TypeAliasBinding{type, name});
    }
    return std::make_unique<TypeAliasDeclStmt>(name, std::move(type));
}

std::unique_ptr<Stmt> Parser::enumDeclaration() {
    consume(TokenType::KEYWORD_ENUM, "Expect 'enum'.");
    const auto name = consume(TokenType::IDENTIFIER, "Expect enum name.");
    Type underlying{BaseType::WORD, "", 2, false};
    if (match({TokenType::COLON})) underlying = parseType();
    if (underlying.pointer_level != 0 || (underlying.base != BaseType::BYTE && underlying.base != BaseType::WORD) || !underlying.enum_name.empty())
        throw CompilerError("Enum underlying type must be byte or word.", name);
    consume(TokenType::LBRACE, "Expect '{' in enum definition.");
    std::vector<EnumEntry> entries;
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        const auto entry = consume(TokenType::IDENTIFIER, "Expect enumerator name.");
        validateValueName(entry);
        std::unique_ptr<Expr> value;
        if (match({TokenType::EQUAL})) value = expression();
        entries.push_back({entry, std::move(value)});
        if (entries.size() > 65536) throw CompilerError("Too many enum entries.", name);
        if (!match({TokenType::COMMA})) break;
    }
    consume(TokenType::RBRACE, "Expect '}' after enum entries.");
    consume(TokenType::SEMICOLON, "Expect ';' after enum.");
    return std::make_unique<EnumDeclStmt>(name, underlying, std::move(entries));
}

std::unique_ptr<Stmt> Parser::staticAssertion() {
    const auto keyword = previous();
    consume(TokenType::LPAREN, "Expect '(' after static_assert.");
    auto condition = expression();
    consume(TokenType::RPAREN, "Expect ')' after static assertion.");
    consume(TokenType::SEMICOLON, "Expect ';' after static_assert.");
    return std::make_unique<StaticAssertStmt>(keyword, std::move(condition));
}

std::unique_ptr<Stmt> Parser::structDeclaration() {
    consume(TokenType::KEYWORD_STRUCT, "Expect 'struct' keyword.");
    Token name = consume(TokenType::IDENTIFIER, "Expect struct name.");
    consume(TokenType::LBRACE, "Expect '{' to begin struct body.");

    std::vector<Member> members;
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        Type memberType = parseType();
        validateValueType(memberType, peek(), "struct member");
        Token memberName = consume(TokenType::IDENTIFIER, "Expect member name.");
        std::unique_ptr<Expr> extent;
        if (match({TokenType::LBRACKET})) {
            extent = expression();
            consume(TokenType::RBRACKET, "Expect ']' after member array extent.");
        }
        consume(TokenType::SEMICOLON, "Expect ';' after struct member.");
        members.push_back({memberType, memberName, std::move(extent)});
    }

    consume(TokenType::RBRACE, "Expect '}' to close struct body.");
    consume(TokenType::SEMICOLON, "Expect ';' after struct declaration.");
    return std::make_unique<StructDefStmt>(name, std::move(members));
}

std::unique_ptr<Stmt> Parser::statement() {
    if (check(TokenType::KEYWORD_IMPORT))
        throw CompilerError("Imports are allowed only at file scope, before declarations.", peek());
    ParseDepthGuard depth(m_statement_depth, peek());
    if (check(TokenType::KEYWORD_TYPE))
        throw CompilerError("Type aliases are top-level declarations.", peek());
    if (check(TokenType::AT_SIGN)) {
        auto attributes = parseAttributes();
        return applyAttributes(statement(), std::move(attributes));
    }
    bool is_cached = match({TokenType::KEYWORD_CACHE});

    if (match({TokenType::KEYWORD_STATIC_ASSERT})) return staticAssertion();
    if (match({TokenType::KEYWORD_CONSTEXPR})) {
        Type type = parseType(); type.is_const = true;
        const auto name = consume(TokenType::IDENTIFIER, "Expect constexpr name.");
        auto declaration = varDeclaration(type, name);
        static_cast<VarDeclStmt&>(*declaration).is_constexpr = true;
        return declaration;
    }
    if (match({TokenType::KEYWORD_IF})) return ifStatement();
    if (match({TokenType::KEYWORD_FOR})) return forStatement(is_cached);
    if (match({TokenType::KEYWORD_WHILE})) return whileStatement(is_cached);
    if (match({TokenType::KEYWORD_SWITCH})) return switchStatement();
    if (match({TokenType::KEYWORD_CONTINUE, TokenType::KEYWORD_FALLTHROUGH})) {
        const auto keyword = previous(); consume(TokenType::SEMICOLON, "Expect ';' after control-flow marker.");
        if (keyword.type == TokenType::KEYWORD_CONTINUE) return std::make_unique<ContinueStmt>(keyword);
        return std::make_unique<FallthroughStmt>(keyword);
    }
    if (match({TokenType::KEYWORD_BREAK})) {
        Token keyword = previous();
        consume(TokenType::SEMICOLON, "Expect ';' after 'break'.");
        return std::make_unique<BreakStmt>(keyword);
    }
    if (match({TokenType::LBRACE})) return blockStatement();
    if (match({TokenType::KEYWORD_RETURN})) return returnStatement();
    if (check(TokenType::KEYWORD_PLOT) && (peekNext().type == TokenType::LBRACE || peekNext().type == TokenType::LPAREN)) { advance(); return plotStatement(); }
    if (match({TokenType::KEYWORD_PLOT_BEGIN})) return plotBeginStatement();
    if (match({TokenType::KEYWORD_PLOT_END})) return plotEndStatement();
    if (match({TokenType::KEYWORD_SET_COLOR})) return setColorStatement();
    if (match({TokenType::KEYWORD_SET_PLOT_OPTIONS})) return setPlotOptionsStatement();
    if (match({TokenType::KEYWORD_FLUSH_PIXELS})) return flushPixelsStatement();
    if (match({TokenType::KEYWORD_DRAW})) return drawStatement();

    if (isAtStartOfDeclaration()) {
        if (is_cached) throw CompilerError("Parse Error: 'cache' cannot precede a variable declaration.",
                                           peek().line_number, peek().col_number);
        Type type = parseType();
        Token name = consume(TokenType::IDENTIFIER, "Expect identifier for variable declaration.");
        return varDeclaration(type, name);
    }

    if (is_cached) throw CompilerError("Parse Error: 'cache' can only be applied to a for or while loop.",
                                       peek().line_number, peek().col_number);

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
    validateValueName(name);
    validateValueType(type, name, "variable declaration");
    std::unique_ptr<Expr> initializer = nullptr;
    std::unique_ptr<Expr> extent;
    if (match({TokenType::LBRACKET})) {
        if (check(TokenType::RBRACKET)) type.array_size = -1;
        else extent = expression();
        consume(TokenType::RBRACKET, "Expect ']' after array size.");
    }
    if (match({TokenType::EQUAL})) initializer = expression();
    consume(TokenType::SEMICOLON, "Expect ';' after variable declaration.");
    auto declaration = std::make_unique<VarDeclStmt>(type, name, std::move(initializer));
    declaration->array_extent = std::move(extent);
    declaration->inferred_extent = type.array_size == -1;
    return declaration;
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
    const auto keyword = previous();
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
    
    if (!condition) condition = std::make_unique<LiteralExpr>(Token(TokenType::KEYWORD_TRUE, "true", keyword));
    return std::make_unique<ForStmt>(keyword, is_cached, std::move(initializer), std::move(condition), std::move(increment), std::move(body));
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
    ParseDepthGuard depth(m_expression_depth, peek());
    auto expr = logicalOr();
    if (match({TokenType::EQUAL})) {
        auto value = assignment();
        return std::make_unique<AssignExpr>(std::move(expr), std::move(value));
    }
    if (match({TokenType::PLUS_EQUAL, TokenType::MINUS_EQUAL, TokenType::STAR_EQUAL, TokenType::SLASH_EQUAL,
               TokenType::PERCENT_EQUAL, TokenType::AND_EQUAL, TokenType::OR_EQUAL, TokenType::XOR_EQUAL,
               TokenType::SHIFT_LEFT_EQUAL, TokenType::SHIFT_RIGHT_EQUAL})) {
        const auto operation = previous();
        return std::make_unique<UpdateExpr>(operation, std::move(expr), assignment());
    }
    return expr;
}
std::unique_ptr<Expr> Parser::equality()   { auto e=comparison(); while(match({TokenType::BANG_EQUAL,TokenType::EQUAL_EQUAL})){Token o=previous();auto r=comparison();e=std::make_unique<BinaryExpr>(std::move(e),o,std::move(r));}return e; }
std::unique_ptr<Expr> Parser::comparison() { return binaryLeft(&Parser::shift, {TokenType::GREATER,TokenType::GREATER_EQUAL,TokenType::LESS,TokenType::LESS_EQUAL}); }
std::unique_ptr<Expr> Parser::binaryLeft(std::unique_ptr<Expr> (Parser::*operand)(), const std::vector<TokenType>& operators) {
    auto left = (this->*operand)();
    while (match(operators)) {
        const auto operation = previous();
        auto right = (this->*operand)();
        left = std::make_unique<BinaryExpr>(std::move(left), operation, std::move(right));
    }
    return left;
}
std::unique_ptr<Expr> Parser::logicalOr() { return binaryLeft(&Parser::logicalAnd, {TokenType::OR_OR}); }
std::unique_ptr<Expr> Parser::logicalAnd() { return binaryLeft(&Parser::bitwiseOr, {TokenType::AND_AND}); }
std::unique_ptr<Expr> Parser::bitwiseOr() { return binaryLeft(&Parser::bitwiseXor, {TokenType::PIPE}); }
std::unique_ptr<Expr> Parser::bitwiseXor() { return binaryLeft(&Parser::bitwiseAnd, {TokenType::CARET}); }
std::unique_ptr<Expr> Parser::bitwiseAnd() { return binaryLeft(&Parser::equality, {TokenType::AMPERSAND}); }
std::unique_ptr<Expr> Parser::shift() { return binaryLeft(&Parser::term, {TokenType::SHIFT_LEFT, TokenType::SHIFT_RIGHT}); }
std::unique_ptr<Expr> Parser::term()       { auto e=factor(); while(match({TokenType::PLUS,TokenType::MINUS})){Token o=previous();auto r=factor();e=std::make_unique<BinaryExpr>(std::move(e),o,std::move(r));}return e; }
std::unique_ptr<Expr> Parser::factor() { return binaryLeft(&Parser::unary, {TokenType::STAR, TokenType::SLASH, TokenType::PERCENT}); }
std::unique_ptr<Expr> Parser::unary() {
    ParseDepthGuard depth(m_expression_depth, peek());
    if (match({TokenType::PLUS_PLUS, TokenType::MINUS_MINUS})) {
        const auto operation = previous();
        auto one = std::make_unique<LiteralExpr>(Token(TokenType::LITERAL_INTEGER, "1", operation));
        return std::make_unique<UpdateExpr>(operation, unary(), std::move(one));
    }
    if (match({TokenType::MINUS, TokenType::AMPERSAND, TokenType::STAR, TokenType::BANG, TokenType::TILDE})) {
        Token op = previous();
        auto right = unary();
        if (op.type == TokenType::MINUS || op.type == TokenType::BANG || op.type == TokenType::TILDE) return std::make_unique<UnaryExpr>(op, std::move(right));
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
        } else if (match({TokenType::DOT, TokenType::ARROW})) {
            const bool arrow = previous().type == TokenType::ARROW;
            Token member = consume(TokenType::IDENTIFIER, "Expect member name after '.'.");
            auto access = std::make_unique<MemberAccessExpr>(std::move(expr), member);
            access->through_pointer = arrow;
            expr = std::move(access);
        } else if (match({TokenType::PLUS_PLUS, TokenType::MINUS_MINUS})) {
            const auto operation = previous();
            auto one = std::make_unique<LiteralExpr>(Token(TokenType::LITERAL_INTEGER, "1", operation));
            expr = std::make_unique<UpdateExpr>(operation, std::move(expr), std::move(one), true);
            break;
        } else { break; }
    }
    return expr;
}
std::unique_ptr<Expr> Parser::primary() {
    if (match({TokenType::LITERAL_STRING})) return std::make_unique<StringExpr>(previous());
    if (match({TokenType::LITERAL_CHARACTER})) return std::make_unique<LiteralExpr>(previous());
    if (match({TokenType::LBRACE})) {
        const auto brace = previous();
        std::vector<std::unique_ptr<Expr>> values;
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            if (values.size() >= 65536) throw CompilerError("Initializer list exceeds one bank.", brace);
            values.push_back(expression());
            if (!match({TokenType::COMMA})) break;
        }
        consume(TokenType::RBRACE, "Expect '}' after initializer list.");
        return std::make_unique<InitializerListExpr>(brace, std::move(values));
    }
    if (match({TokenType::KEYWORD_NULL})) return std::make_unique<NullExpr>(previous());
    if (match({TokenType::KEYWORD_SIZEOF, TokenType::KEYWORD_ALIGNOF, TokenType::KEYWORD_OFFSETOF})) {
        const auto operation = previous();
        consume(TokenType::LPAREN, "Expect '(' after layout query.");
        Type type;
        std::unique_ptr<Expr> object;
        if (isAtStartOfDeclaration()) type = parseType();
        else if (operation.type != TokenType::KEYWORD_OFFSETOF) object = expression();
        else throw CompilerError("offsetof requires a type and member.", operation);
        auto query = std::make_unique<LayoutQueryExpr>(operation, type, std::move(object));
        if (operation.type == TokenType::KEYWORD_OFFSETOF) {
            consume(TokenType::COMMA, "Expect ',' in offsetof.");
            query->member = consume(TokenType::IDENTIFIER, "Expect member name in offsetof.");
        }
        consume(TokenType::RPAREN, "Expect ')' after layout query.");
        return query;
    }
    if (match({TokenType::KEYWORD_PLOT})) {
        consume(TokenType::DOT, "Expect '.' in plot coordinate access.");
        const auto member = consume(TokenType::IDENTIFIER, "Expect 'x' or 'y' after 'plot.'.");
        if (member.lexeme != "x" && member.lexeme != "y")
            throw CompilerError("Plot context has only x and y coordinates.", member);
        return std::make_unique<PlotCoordinateExpr>(member, member.lexeme == "y");
    }
    if (match({TokenType::KEYWORD_TRUE, TokenType::KEYWORD_FALSE}))
        return std::make_unique<LiteralExpr>(previous());
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

bool Parser::isAtEnd() { return m_current >= m_tokens.size() || m_tokens[m_current].type == TokenType::END_OF_FILE; }
Token Parser::peek() {
    if (m_current >= m_tokens.size()) return Token(TokenType::END_OF_FILE, "", 0, 0);
    return m_tokens[m_current];
}
Token Parser::peekNext() {
    if (m_current + 1 >= m_tokens.size()) return Token(TokenType::END_OF_FILE, "", 0, 0);
    return m_tokens[m_current + 1];
}
Token Parser::previous() {
    if (m_current == 0 || m_tokens.empty()) return Token(TokenType::END_OF_FILE, "", 0, 0);
    return m_tokens[std::min(m_current - 1, m_tokens.size() - 1)];
}
Token Parser::advance() { if (!isAtEnd()) m_current++; return previous(); }
bool Parser::check(TokenType type) { return !isAtEnd() && peek().type == type; }
bool Parser::match(const std::vector<TokenType>& types) { for (auto t : types) { if (check(t)) { advance(); return true; } } return false; }
Token Parser::consume(TokenType type, const std::string& message) { 
    if (check(type)) return advance(); 
    Token prev = previous();
    auto location = prev;
    location.col_number += static_cast<int>(prev.lexeme.length());
    throw CompilerError(message, location);
}

std::int64_t Parser::parseIntegerLiteral(const Token& token, const std::string& context) {
    try {
        std::size_t parsed = 0;
        const auto value = DiscoNumeric::parse(token.lexeme, &parsed, 0);
        if (parsed != token.lexeme.size()) throw std::invalid_argument("trailing characters");
        return value;
    } catch (const std::exception&) {
        throw CompilerError("Invalid integer literal for " + context + ".",
                            token);
    }
}

void Parser::validateValueType(const Type& type, const Token& token, const std::string& context) {
    if (type.base == BaseType::VOID && type.pointer_level == 0) {
        throw CompilerError("Void is not a valid " + context + " type.",
                            token);
    }
    if (type.is_unsigned && type.base != BaseType::BYTE && type.base != BaseType::WORD) {
        throw CompilerError("Unsigned is only valid with byte or word types.",
                            token);
    }
}
std::unique_ptr<Stmt> Parser::plotStatement() {
    const Token keyword = previous();
    if (match({TokenType::LBRACE}))
        return std::make_unique<PlotBlockStmt>(keyword, blockStatement());
    consume(TokenType::LPAREN, "Expect '(' after 'plot'.");
    auto x = expression();
    consume(TokenType::COMMA, "Expect ',' to separate plot arguments.");
    auto y = expression();
    consume(TokenType::RPAREN, "Expect ')' after plot arguments.");
    consume(TokenType::SEMICOLON, "Expect ';' after plot statement.");
    return std::make_unique<PlotStmt>(std::move(x), std::move(y));
}
std::unique_ptr<Stmt> Parser::plotBeginStatement() {
    const Token keyword = previous();
    consume(TokenType::SEMICOLON, "Expect ';' after plot_begin.");
    auto stmt = std::make_unique<PlotBeginStmt>();
    stmt->token = keyword;
    return stmt;
}
std::unique_ptr<Stmt> Parser::plotEndStatement() {
    const Token keyword = previous();
    consume(TokenType::SEMICOLON, "Expect ';' after plot_end.");
    auto stmt = std::make_unique<PlotEndStmt>();
    stmt->token = keyword;
    return stmt;
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
