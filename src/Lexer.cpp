#include "Lexer.hpp"
#include <map>
#include <cctype>
#include <stdexcept>
#include "CompilerError.hpp"

Lexer::Lexer(const std::string& source) : m_source(source) {}

static const std::map<std::string, TokenType> keywords = {
    {"switch", TokenType::KEYWORD_SWITCH},
    {"case",   TokenType::KEYWORD_CASE},
    {"default",TokenType::KEYWORD_DEFAULT},
    {"break",  TokenType::KEYWORD_BREAK},
    {"struct", TokenType::KEYWORD_STRUCT},
    {"far",    TokenType::KEYWORD_FAR},
    {"set",    TokenType::KEYWORD_SET},
    {"const",  TokenType::KEYWORD_CONST},
    {"volatile", TokenType::KEYWORD_VOLATILE},
    {"ram", TokenType::KEYWORD_RAM},
    {"bool", TokenType::KEYWORD_BOOL},
    {"true", TokenType::KEYWORD_TRUE},
    {"false", TokenType::KEYWORD_FALSE},
    {"internal", TokenType::KEYWORD_INTERNAL},
    {"export", TokenType::KEYWORD_EXPORT},
    {"extern", TokenType::KEYWORD_EXTERN},
    {"rom",      TokenType::KEYWORD_ROM},
    {"draw",   TokenType::KEYWORD_DRAW},
    {"at",     TokenType::KEYWORD_AT},
    {"with",   TokenType::KEYWORD_WITH},
    {"color",  TokenType::KEYWORD_COLOR},
    {"set_plot_options", TokenType::KEYWORD_SET_PLOT_OPTIONS},
    {"flush_pixels",     TokenType::KEYWORD_FLUSH_PIXELS},
    {"set_color",  TokenType::KEYWORD_SET_COLOR},
    {"plot_begin", TokenType::KEYWORD_PLOT_BEGIN},
    {"plot_end",   TokenType::KEYWORD_PLOT_END},
    {"plot",   TokenType::KEYWORD_PLOT},
    {"unsigned", TokenType::KEYWORD_UNSIGNED},
    {"word",     TokenType::KEYWORD_WORD},
    {"byte",     TokenType::KEYWORD_BYTE},
    {"for",    TokenType::KEYWORD_FOR},
    {"while",  TokenType::KEYWORD_WHILE},
    {"if",     TokenType::KEYWORD_IF},
    {"else",   TokenType::KEYWORD_ELSE},
    {"void",   TokenType::KEYWORD_VOID},
    {"return", TokenType::KEYWORD_RETURN},
    {"cache",  TokenType::KEYWORD_CACHE},
    {"constexpr", TokenType::KEYWORD_CONSTEXPR},
    {"enum", TokenType::KEYWORD_ENUM},
    {"null", TokenType::KEYWORD_NULL},
    {"module", TokenType::KEYWORD_MODULE},
    {"import", TokenType::KEYWORD_IMPORT},
    {"type", TokenType::KEYWORD_TYPE},
    {"continue", TokenType::KEYWORD_CONTINUE},
    {"fallthrough", TokenType::KEYWORD_FALLTHROUGH},
    {"sizeof", TokenType::KEYWORD_SIZEOF},
    {"alignof", TokenType::KEYWORD_ALIGNOF},
    {"offsetof", TokenType::KEYWORD_OFFSETOF},
    {"static_assert", TokenType::KEYWORD_STATIC_ASSERT},
};

bool Lexer::match(char expected) {
    if (isAtEnd() || m_source[m_current] != expected) return false;
    advance();
    return true;
}

std::vector<Token> Lexer::scanTokens() {
    while (!isAtEnd()) {
        // We are at the beginning of the next lexeme.
        m_start = m_current;
        m_token_line = m_line;
        m_token_col = m_col;
        scanToken();
    }

    // Add one final token to mark the end.
    m_tokens.emplace_back(TokenType::END_OF_FILE, "", m_line, m_col);
    return m_tokens;
}

bool Lexer::isAtEnd() {
    return m_current >= m_source.length();
}

char Lexer::advance() {
    const char value = m_source[m_current++];
    if (value == '\n') {
        ++m_line;
        m_col = 1;
    } else {
        ++m_col;
    }
    return value;
}

void Lexer::addToken(TokenType type) {
    std::string text = m_source.substr(m_start, m_current - m_start);
    m_tokens.emplace_back(type, text, m_token_line, m_token_col);
}

bool Lexer::isDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c));
}

bool Lexer::isAlpha(char c) {
    // Allows letters a-z, A-Z, and underscore for identifiers
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool Lexer::isAlphaNumeric(char c) {
    return isAlpha(c) || isDigit(c);
}

// A helper to "peek" at the current character without consuming it. Completely necessary IMO.
char Lexer::peek() {
    if (isAtEnd()) return '\0';
    return m_source[m_current];
}

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Looks at the character after the current one.
char Lexer::peekNext() {
    if (m_current + 1 >= m_source.length()) return '\0';
    return m_source[m_current + 1];
}

void Lexer::number(char first_digit) {
    // Check for special bases if the first digit was '0'
    if (first_digit == '0') {
        if (peek() == 'x' || peek() == 'X') {
            // Hexadecimal
            advance(); // Consume the 'x'
            while (isxdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        } else if (peek() == 'b' || peek() == 'B') {
            // Binary
            advance(); // Consume the 'b'
            while (peek() == '0' || peek() == '1') {
                advance();
            }
        } else {
            // Octal
            while (peek() >= '0' && peek() <= '7') {
                advance();
            }
        }
    } else {
        // Decimal
        while (isDigit(peek())) {
            advance();
        }
    }

    addToken(TokenType::LITERAL_INTEGER);
}

void Lexer::identifier() {
    while (isAlphaNumeric(peek())) {
        advance();
    }

    std::string text = m_source.substr(m_start, m_current - m_start);
    TokenType type;

    auto it = keywords.find(text);
    if (it == keywords.end()) {
        // It's not a reserved keyword, so it must be a user-defined identifier.
        type = TokenType::IDENTIFIER;
    } else {
        // It's a reserved keyword.
        type = it->second;
    }
    addToken(type);
}

void Lexer::quotedLiteral(char quote) {
    std::string decoded;
    while (!isAtEnd() && peek() != quote) {
        auto character = static_cast<unsigned char>(advance());
        if (character == '\n' || character == '\r')
            throw CompilerError("Newline in quoted literal.", m_token_line, m_token_col);
        if (character == '\\') {
            if (isAtEnd()) break;
            const char escape = advance();
            switch (escape) {
                case 'n': character = '\n'; break;
                case 'r': character = '\r'; break;
                case 't': character = '\t'; break;
                case '0': character = 0; break;
                case '\\': character = '\\'; break;
                case '\'': character = '\''; break;
                case '"': character = '"'; break;
                case 'x': {
                    unsigned value = 0;
                    for (int digit = 0; digit < 2; ++digit) {
                        const char hex = peek();
                        if (!isHexDigit(hex)) throw CompilerError("Hex escape requires exactly two digits.", m_token_line, m_token_col);
                        advance();
                        value = value * 16u + static_cast<unsigned>(hex <= '9' ? hex - '0' : hex <= 'F' ? hex - 'A' + 10 : hex - 'a' + 10);
                    }
                    character = static_cast<unsigned char>(value);
                    break;
                }
                default: throw CompilerError("Unsupported escape sequence.", m_token_line, m_token_col);
            }
        } else if (character >= 128) throw CompilerError("Text literals use ASCII; encode other bytes with hex escapes.", m_token_line, m_token_col);
        if (decoded.size() >= 65534) throw CompilerError("Quoted literal exceeds 65534 bytes.", m_token_line, m_token_col);
        decoded.push_back(static_cast<char>(character));
    }
    if (isAtEnd()) throw CompilerError("Unterminated quoted literal.", m_token_line, m_token_col);
    advance();
    if (quote == '\'') {
        if (decoded.size() != 1) throw CompilerError("Character literal must contain exactly one byte.", m_token_line, m_token_col);
        m_tokens.emplace_back(TokenType::LITERAL_CHARACTER, std::to_string(static_cast<unsigned char>(decoded.front())), m_token_line, m_token_col);
    } else m_tokens.emplace_back(TokenType::LITERAL_STRING, std::move(decoded), m_token_line, m_token_col);
}

void Lexer::scanToken() {
    char c = advance();
    switch (c) {
        // Single-character tokens (no change here)
        case '"': quotedLiteral('"'); break;
        case '\'': quotedLiteral('\''); break;
        case '(': addToken(TokenType::LPAREN); break;
        case ')': addToken(TokenType::RPAREN); break;
        case '{': addToken(TokenType::LBRACE); break;
        case '}': addToken(TokenType::RBRACE); break;
        case ';': addToken(TokenType::SEMICOLON); break;
        case ':': addToken(TokenType::COLON); break;
        case ',': addToken(TokenType::COMMA); break;
        case '.': addToken(TokenType::DOT); break;
        case '@': addToken(TokenType::AT_SIGN); break;
		case '&': addToken(match('&') ? TokenType::AND_AND : match('=') ? TokenType::AND_EQUAL : TokenType::AMPERSAND); break;
        case '|': addToken(match('|') ? TokenType::OR_OR : match('=') ? TokenType::OR_EQUAL : TokenType::PIPE); break;
        case '^': addToken(match('=') ? TokenType::XOR_EQUAL : TokenType::CARET); break;
        case '~': addToken(TokenType::TILDE); break;
        case '%': addToken(match('=') ? TokenType::PERCENT_EQUAL : TokenType::PERCENT); break;
		case '[': addToken(TokenType::LBRACKET); break;
		case ']': addToken(TokenType::RBRACKET); break;
        case '=': addToken(match('=') ? TokenType::EQUAL_EQUAL : TokenType::EQUAL); break;
        case '!': addToken(match('=') ? TokenType::BANG_EQUAL : TokenType::BANG); break;
        case '<': addToken(match('<') ? (match('=') ? TokenType::SHIFT_LEFT_EQUAL : TokenType::SHIFT_LEFT) : match('=') ? TokenType::LESS_EQUAL : TokenType::LESS); break;
        case '>': addToken(match('>') ? (match('=') ? TokenType::SHIFT_RIGHT_EQUAL : TokenType::SHIFT_RIGHT) : match('=') ? TokenType::GREATER_EQUAL : TokenType::GREATER); break;
        case '+': addToken(match('+') ? TokenType::PLUS_PLUS : match('=') ? TokenType::PLUS_EQUAL : TokenType::PLUS); break;
        case '-': addToken(match('-') ? TokenType::MINUS_MINUS : match('>') ? TokenType::ARROW : match('=') ? TokenType::MINUS_EQUAL : TokenType::MINUS); break;
        case '*': addToken(match('=') ? TokenType::STAR_EQUAL : TokenType::STAR); break;
        case '/':
            if (match('/')) {
                // A single-line comment goes until the end of the line.
                // We just consume the characters without adding a token.
                while (peek() != '\n' && !isAtEnd()) {
                    advance();
                }
            } else if (match('*')) {
                // A multi-line comment.
                int start_line = m_line;
                while (!(peek() == '*' && peekNext() == '/') && !isAtEnd()) {
                    advance();
                }

                if (isAtEnd()) {
                    // We reached the end of the file without finding the closing */
                    throw CompilerError("Unterminated multi-line comment.", start_line, 0);
                }

                // Consume the closing "*/"
                advance(); // Consume '*'
                advance(); // Consume '/'
            } else {
                // If it's not a comment, it's a division operator.
                addToken(match('=') ? TokenType::SLASH_EQUAL : TokenType::SLASH);
            }
            break;
        
        // Ignore whitespace
        case ' ':
        case '\r':
        case '\t':
            break;
        case '\n':
            break;

        default:
            if (isDigit(c)) {
                // It's the start of a number.
                number(c);
            } else if (isAlpha(c)) {
                // It's the start of an identifier or a keyword.
                identifier();
            } else {
                // We truly don't know what this is.
                addToken(TokenType::UNKNOWN);
            }
            break;
    }
}
