#pragma once

#include <iostream>
#include <string>

enum class TokenType {
    // Literals
    NUMBER,
    STRING,

    // String interpolation: "%{expr} text %{expr} text"
    // becomes: INTERP_START, <expr tokens>, INTERP_MID, <expr tokens>, INTERP_END
    INTERP_START,
    INTERP_MID,
    INTERP_END,

    // Identifier
    IDENTIFIER,

    // Keywords
    LET,
    FUNC,
    IF,
    ELSE,
    ELIF,
    WHILE,
    FOR,
    IN,
    RETURN,
    BREAK,
    CONTINUE,
    TRY,
    CATCH,
    THROW,
    ENSURE,
    ASYNC,
    AWAIT,
    CLASS,
    EXTENDS,
    THIS,
    SUPER,
    LAM,
    USE,
    EXPORT,
    TRUE,
    FALSE,
    NIL,

    // Operators
    PLUS,       // +
    MINUS,      // -
    STAR,       // *
    SLASH,      // /
    PERCENT,    // %
    ASSIGN,     // =
    EQ,         // ==
    NEQ,        // !=
    LT,         // <
    GT,         // >
    LTE,        // <=
    GTE,        // >=
    NOT,        // !
    INCREMENT,  // ++
    DECREMENT,  // --
    AND,        // &&
    OR,         // ||
    DOT,        // .
    DOT_DOT,    // ..
    PIPE,       // |>
    COLON,      // :

    // Delimiters
    LPAREN,     // (
    RPAREN,     // )
    LBRACE,     // {
    RBRACE,     // }
    LBRACKET,   // [
    RBRACKET,   // ]
    COMMA,      // ,

    // Special
    EOF_TOKEN,
};

struct Token {
    TokenType type;
    std::string lexeme;
    int line;
};

inline std::string tokenTypeToString(TokenType type) {
    switch (type) {
        case TokenType::NUMBER:      return "NUMBER";
        case TokenType::STRING:      return "STRING";
        case TokenType::INTERP_START: return "INTERP_START";
        case TokenType::INTERP_MID:  return "INTERP_MID";
        case TokenType::INTERP_END:  return "INTERP_END";
        case TokenType::IDENTIFIER:  return "IDENTIFIER";
        case TokenType::LET:         return "LET";
        case TokenType::FUNC:        return "FUNC";
        case TokenType::IF:          return "IF";
        case TokenType::ELSE:        return "ELSE";
        case TokenType::ELIF:        return "ELIF";
        case TokenType::WHILE:       return "WHILE";
        case TokenType::FOR:         return "FOR";
        case TokenType::IN:          return "IN";
        case TokenType::RETURN:      return "RETURN";
        case TokenType::BREAK:       return "BREAK";
        case TokenType::CONTINUE:    return "CONTINUE";
        case TokenType::TRY:         return "TRY";
        case TokenType::CATCH:       return "CATCH";
        case TokenType::THROW:       return "THROW";
        case TokenType::ENSURE:      return "ENSURE";
        case TokenType::ASYNC:       return "ASYNC";
        case TokenType::AWAIT:       return "AWAIT";
        case TokenType::CLASS:       return "CLASS";
        case TokenType::EXTENDS:     return "EXTENDS";
        case TokenType::THIS:        return "THIS";
        case TokenType::SUPER:       return "SUPER";
        case TokenType::LAM:         return "LAM";
        case TokenType::USE:         return "USE";
        case TokenType::EXPORT:      return "EXPORT";
        case TokenType::TRUE:        return "TRUE";
        case TokenType::FALSE:       return "FALSE";
        case TokenType::NIL:         return "NIL";
        case TokenType::PLUS:        return "PLUS";
        case TokenType::MINUS:       return "MINUS";
        case TokenType::STAR:        return "STAR";
        case TokenType::SLASH:       return "SLASH";
        case TokenType::PERCENT:     return "PERCENT";
        case TokenType::ASSIGN:      return "ASSIGN";
        case TokenType::EQ:          return "EQ";
        case TokenType::NEQ:         return "NEQ";
        case TokenType::LT:          return "LT";
        case TokenType::GT:          return "GT";
        case TokenType::LTE:         return "LTE";
        case TokenType::GTE:         return "GTE";
        case TokenType::NOT:         return "NOT";
        case TokenType::INCREMENT:   return "INCREMENT";
        case TokenType::DECREMENT:   return "DECREMENT";
        case TokenType::AND:         return "AND";
        case TokenType::OR:          return "OR";
        case TokenType::DOT:         return "DOT";
        case TokenType::DOT_DOT:     return "DOT_DOT";
        case TokenType::PIPE:        return "PIPE";
        case TokenType::COLON:       return "COLON";
        case TokenType::LPAREN:      return "LPAREN";
        case TokenType::RPAREN:      return "RPAREN";
        case TokenType::LBRACE:      return "LBRACE";
        case TokenType::RBRACE:      return "RBRACE";
        case TokenType::LBRACKET:    return "LBRACKET";
        case TokenType::RBRACKET:    return "RBRACKET";
        case TokenType::COMMA:       return "COMMA";
        case TokenType::EOF_TOKEN:   return "EOF";
    }
    return "UNKNOWN";
}

inline std::ostream& operator<<(std::ostream& os, const Token& token) {
    os << "[" << tokenTypeToString(token.type);
    if (!token.lexeme.empty()) {
        os << " \"" << token.lexeme << "\"";
    }
    os << " line:" << token.line << "]";
    return os;
}
