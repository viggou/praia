#include "lexer.h"
#include <iostream>
#include <unordered_map>

static const std::unordered_map<std::string, TokenType> keywords = {
    {"let",    TokenType::LET},
    {"func",   TokenType::FUNC},
    {"if",     TokenType::IF},
    {"else",   TokenType::ELSE},
    {"elif",   TokenType::ELIF},
    {"while",  TokenType::WHILE},
    {"for",    TokenType::FOR},
    {"in",     TokenType::IN},
    {"return",   TokenType::RETURN},
    {"break",    TokenType::BREAK},
    {"continue", TokenType::CONTINUE},
    {"try",    TokenType::TRY},
    {"catch",  TokenType::CATCH},
    {"throw",  TokenType::THROW},
    {"ensure", TokenType::ENSURE},
    {"async",   TokenType::ASYNC},
    {"await",   TokenType::AWAIT},
    {"enum",    TokenType::ENUM},
    {"class",   TokenType::CLASS},
    {"extends", TokenType::EXTENDS},
    {"this",    TokenType::THIS},
    {"super",   TokenType::SUPER},
    {"lam",     TokenType::LAM},
    {"use",    TokenType::USE},
    {"export", TokenType::EXPORT},
    {"true",   TokenType::TRUE},
    {"false",  TokenType::FALSE},
    {"nil",    TokenType::NIL},
};

Lexer::Lexer(const std::string& source) : source(source) {}

std::vector<Token> Lexer::tokenize() {
    while (!isAtEnd()) {
        start = current;
        scanToken();
    }
    tokens.push_back({TokenType::EOF_TOKEN, "", line});
    return tokens;
}

void Lexer::scanToken() {
    char c = advance();
    switch (c) {
        case '(': addToken(TokenType::LPAREN); break;
        case ')': addToken(TokenType::RPAREN); break;
        case '{': addToken(TokenType::LBRACE); break;
        case '}': addToken(TokenType::RBRACE); break;
        case '[': addToken(TokenType::LBRACKET); break;
        case ']': addToken(TokenType::RBRACKET); break;
        case ',': addToken(TokenType::COMMA); break;
        case '*': addToken(match('=') ? TokenType::STAR_ASSIGN : TokenType::STAR); break;
        case '%': addToken(match('=') ? TokenType::PERCENT_ASSIGN : TokenType::PERCENT); break;
        case '.':
            if (match('.')) {
                addToken(match('.') ? TokenType::SPREAD : TokenType::DOT_DOT);
            } else {
                addToken(TokenType::DOT);
            }
            break;
        case ':': addToken(TokenType::COLON); break;
        case '?': addToken(TokenType::QUESTION); break;

        case '+':
            if (match('+')) addToken(TokenType::INCREMENT);
            else if (match('=')) addToken(TokenType::PLUS_ASSIGN);
            else addToken(TokenType::PLUS);
            break;
        case '-':
            if (match('-')) addToken(TokenType::DECREMENT);
            else if (match('=')) addToken(TokenType::MINUS_ASSIGN);
            else addToken(TokenType::MINUS);
            break;
        case '!':
            addToken(match('=') ? TokenType::NEQ : TokenType::NOT);
            break;
        case '=':
            addToken(match('=') ? TokenType::EQ : TokenType::ASSIGN);
            break;
        case '<':
            if (match('<')) addToken(TokenType::SHL);
            else addToken(match('=') ? TokenType::LTE : TokenType::LT);
            break;
        case '>':
            if (match('>')) addToken(TokenType::SHR);
            else addToken(match('=') ? TokenType::GTE : TokenType::GT);
            break;

        case '&':
            addToken(match('&') ? TokenType::AND : TokenType::BIT_AND);
            break;
        case '|':
            if (match('|')) addToken(TokenType::OR);
            else if (match('>')) addToken(TokenType::PIPE);
            else addToken(TokenType::BIT_OR);
            break;
        case '^': addToken(TokenType::BIT_XOR); break;
        case '~': addToken(TokenType::BIT_NOT); break;

        case '/':
            if (match('/')) {
                while (peek() != '\n' && !isAtEnd()) advance();
            } else if (match('*')) {
                blockComment();
            } else if (match('=')) {
                addToken(TokenType::SLASH_ASSIGN);
            } else {
                addToken(TokenType::SLASH);
            }
            break;

        case ' ':
        case '\r':
        case '\t':
            break;
        case '\n':
            line++;
            break;

        case '"':
            if (peek() == '"' && peekNext() == '"') {
                advance(); advance(); // consume the other two quotes
                tripleString('"');
            } else {
                string('"');
            }
            break;
        case '\'':
            if (peek() == '\'' && peekNext() == '\'') {
                advance(); advance();
                tripleString('\'');
            } else {
                string('\'');
            }
            break;

        default:
            if (std::isdigit(c)) {
                number();
            } else if (std::isalpha(c) || c == '_') {
                identifier();
            } else {
                error(std::string("Unexpected character '") + c + "'");
            }
            break;
    }
}

// --- Character helpers ---

char Lexer::advance() {
    return source[current++];
}

char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return source[current];
}

char Lexer::peekNext() const {
    if (current + 1 >= static_cast<int>(source.size())) return '\0';
    return source[current + 1];
}

bool Lexer::match(char expected) {
    if (isAtEnd() || source[current] != expected) return false;
    current++;
    return true;
}

bool Lexer::isAtEnd() const {
    return current >= static_cast<int>(source.size());
}

// --- Token scanners ---

void Lexer::number() {
    while (std::isdigit(peek())) advance();

    if (peek() == '.' && std::isdigit(peekNext())) {
        advance(); // consume '.'
        while (std::isdigit(peek())) advance();
    }

    addToken(TokenType::NUMBER);
}

void Lexer::string(char quote) {
    std::string value;
    bool hasInterp = false;
    bool firstInterp = true;

    while (peek() != quote && !isAtEnd()) {
        if (peek() == '\n') line++;

        // String interpolation: %{expr} (works in both quote styles)
        if (peek() == '%' && peekNext() == '{') {
            hasInterp = true;
            advance(); // consume %
            advance(); // consume {

            TokenType partType = firstInterp ? TokenType::INTERP_START : TokenType::INTERP_MID;
            tokens.push_back({partType, value, line});
            value.clear();
            firstInterp = false;

            // Collect the expression source between %{ and the matching }
            std::string expr;
            int depth = 1;
            while (depth > 0 && !isAtEnd()) {
                char ch = peek();
                if (ch == '{') depth++;
                if (ch == '}') depth--;
                if (depth > 0) {
                    if (ch == '\n') line++;
                    expr += advance();
                } else {
                    advance(); // consume closing }
                }
            }

            // Lex the expression with a sub-lexer
            Lexer subLexer(expr);
            auto exprTokens = subLexer.tokenize();
            for (auto& t : exprTokens) {
                if (t.type != TokenType::EOF_TOKEN) {
                    t.line = line;
                    tokens.push_back(t);
                }
            }
            if (subLexer.hasError()) hadError = true;

            continue;
        }

        // Escape sequences
        if (peek() == '\\') {
            advance(); // consume backslash
            if (isAtEnd()) {
                error("Unterminated escape sequence");
                return;
            }
            char escaped = advance();
            switch (escaped) {
                case 'n':  value += '\n'; break;
                case 't':  value += '\t'; break;
                case '\\': value += '\\'; break;
                case '"':  value += '"';  break;
                case '\'': value += '\''; break;
                case '%':  value += '%';  break;
                default:
                    error(std::string("Unknown escape sequence '\\") + escaped + "'");
                    value += escaped;
                    break;
            }
            continue;
        }

        value += advance();
    }

    if (isAtEnd()) {
        error("Unterminated string");
        return;
    }

    advance(); // consume closing quote

    if (hasInterp) {
        tokens.push_back({TokenType::INTERP_END, value, line});
    } else {
        tokens.push_back({TokenType::STRING, value, line});
    }
}

void Lexer::tripleString(char quote) {
    // Skip the first newline after opening triple-quote
    if (peek() == '\n') { advance(); line++; }

    std::string value;
    bool hasInterp = false;
    bool firstInterp = true;

    while (!isAtEnd()) {
        // Check for closing triple-quote
        if (peek() == quote && peekNext() == quote &&
            current + 2 < static_cast<int>(source.size()) && source[current + 2] == quote) {
            advance(); advance(); advance(); // consume closing triple
            break;
        }

        if (peek() == '\n') line++;

        // String interpolation
        if (peek() == '%' && peekNext() == '{') {
            hasInterp = true;
            advance(); advance();
            TokenType partType = firstInterp ? TokenType::INTERP_START : TokenType::INTERP_MID;
            tokens.push_back({partType, value, line});
            value.clear();
            firstInterp = false;

            std::string expr;
            int depth = 1;
            while (depth > 0 && !isAtEnd()) {
                char ch = peek();
                if (ch == '{') depth++;
                if (ch == '}') depth--;
                if (depth > 0) {
                    if (ch == '\n') line++;
                    expr += advance();
                } else {
                    advance();
                }
            }

            Lexer subLexer(expr);
            auto exprTokens = subLexer.tokenize();
            for (auto& t : exprTokens) {
                if (t.type != TokenType::EOF_TOKEN) {
                    t.line = line;
                    tokens.push_back(t);
                }
            }
            if (subLexer.hasError()) hadError = true;
            continue;
        }

        // Escape sequences
        if (peek() == '\\') {
            advance();
            if (isAtEnd()) { error("Unterminated escape sequence"); return; }
            char escaped = advance();
            switch (escaped) {
                case 'n':  value += '\n'; break;
                case 't':  value += '\t'; break;
                case '\\': value += '\\'; break;
                case '"':  value += '"';  break;
                case '\'': value += '\''; break;
                case '%':  value += '%';  break;
                default: value += escaped; break;
            }
            continue;
        }

        value += advance();
    }

    if (hasInterp) {
        tokens.push_back({TokenType::INTERP_END, value, line});
    } else {
        tokens.push_back({TokenType::STRING, value, line});
    }
}

void Lexer::identifier() {
    while (std::isalnum(peek()) || peek() == '_') advance();

    std::string text = source.substr(start, current - start);
    auto it = keywords.find(text);
    TokenType type = (it != keywords.end()) ? it->second : TokenType::IDENTIFIER;
    addToken(type);
}

void Lexer::blockComment() {
    int depth = 1;
    while (depth > 0 && !isAtEnd()) {
        if (peek() == '/' && peekNext() == '*') {
            advance(); advance();
            depth++;
        } else if (peek() == '*' && peekNext() == '/') {
            advance(); advance();
            depth--;
        } else {
            if (peek() == '\n') line++;
            advance();
        }
    }

    if (depth > 0) {
        error("Unterminated block comment");
    }
}

// --- Helpers ---

void Lexer::addToken(TokenType type) {
    tokens.push_back({type, source.substr(start, current - start), line});
}

void Lexer::addToken(TokenType type, const std::string& lexeme) {
    tokens.push_back({type, lexeme, line});
}

void Lexer::error(const std::string& message) {
    std::cerr << "[line " << line << "] Error: " << message << std::endl;
    hadError = true;
}
