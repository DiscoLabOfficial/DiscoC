#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include "Token.hpp"

class Lexer {
public:
    // Constructor takes the source code to be scanned.
    Lexer(const std::string& source);

    // The main function that performs the scan.
    std::vector<Token> scanTokens();

private:
    const std::string& m_source;
    std::vector<Token> m_tokens;

    std::size_t m_start = 0;   // Start of the current lexeme being scanned
    std::size_t m_current = 0; // Current character we are looking at
    int m_line = 1;    // Current line number for error reporting
	int m_col = 1;     // Current column number for error reporting
	int m_token_line = 1;
	int m_token_col = 1;

    bool isAtEnd();
    char advance();
    void addToken(TokenType type);
    bool match(char expected);

    // The main scanning logic gets its own function.
    void scanToken();

    // New helper methods
	char peek();
    char peekNext();
    bool isDigit(char c);
    bool isAlpha(char c);
    bool isAlphaNumeric(char c);

    void number(char first_digit);
    void identifier();
};
