#pragma once

#include "token.h"
#include <string>
#include <vector>

class Lexer {
public:
    Lexer(const std::string& source);
    std::vector<Token> tokenize();
    bool hasError() const { return hadError; }

private:
    void scanToken();

    char advance();
    char peek() const;
    char peekNext() const;
    bool match(char expected);
    bool isAtEnd() const;

    void number();
    void string(char quote);
    void identifier();
    void blockComment();

    void addToken(TokenType type);
    void addToken(TokenType type, const std::string& lexeme);
    void error(const std::string& message);

    std::string source;
    std::vector<Token> tokens;
    int start = 0;
    int current = 0;
    int line = 1;
    bool hadError = false;
};
