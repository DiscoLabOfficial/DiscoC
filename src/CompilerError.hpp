#pragma once
#include <exception>
#include <string>
#include "Token.hpp"

class CompilerError : public std::exception {
public:
    CompilerError(std::string message, int line, int col)
        : m_message(std::move(message)), m_line(line), m_col(col) {
        m_what_buffer = m_message + " (line: " + std::to_string(m_line) + ", col: " + std::to_string(m_col) + ")";
    }

    CompilerError(std::string message, const Token& source)
        : CompilerError(std::move(message), source.line_number, source.col_number) { m_source_path = source.source_path; }
    const std::string& getSourcePath() const { return m_source_path; }
    void setSourcePath(std::string path) { if (m_source_path.empty()) m_source_path = std::move(path); }

    const char* what() const noexcept override {
        return m_what_buffer.c_str();
    }

    const std::string& getMessage() const { return m_message; }
    int getLine() const { return m_line; }
    int getCol() const { return m_col; }

private:
    std::string m_message;
    std::string m_source_path;
    int m_line;
    int m_col;
    std::string m_what_buffer;
};
