#include "parser.h"
#include <iostream>

Parser::Parser(const std::vector<Token>& tokens) : tokens(tokens) {}

std::vector<StmtPtr> Parser::parse() {
    std::vector<StmtPtr> statements;
    while (!isAtEnd()) {
        try {
            statements.push_back(statement());
        } catch (const ParseError&) {
            synchronize();
        }
    }
    return statements;
}

// ── Statements ───────────────────────────────────────────────

StmtPtr Parser::statement() {
    if (match(TokenType::LET))    return letStatement();
    if (match(TokenType::FUNC))   return funcStatement();
    if (match(TokenType::CLASS))  return classStatement();
    if (match(TokenType::IF))     return ifStatement();
    if (match(TokenType::WHILE))  return whileStatement();
    if (match(TokenType::FOR))    return forStatement();
    if (match(TokenType::RETURN)) return returnStatement();
    if (match(TokenType::BREAK))    return breakStatement();
    if (match(TokenType::CONTINUE)) return continueStatement();
    if (match(TokenType::THROW))  return throwStatement();
    if (match(TokenType::TRY))    return tryCatchStatement();
    if (match(TokenType::ENSURE)) return ensureStatement();
    if (match(TokenType::USE))    return useStatement();
    if (match(TokenType::EXPORT)) return exportStatement();
    return expressionStatement();
}

StmtPtr Parser::letStatement() {
    Token name = consume(TokenType::IDENTIFIER, "Expected variable name after 'let'");

    auto stmt = std::make_unique<LetStmt>();
    stmt->line = name.line;
    stmt->name = name.lexeme;

    if (match(TokenType::ASSIGN)) {
        stmt->initializer = expression();
    }

    return stmt;
}

StmtPtr Parser::funcStatement() {
    Token name = consume(TokenType::IDENTIFIER, "Expected function name after 'func'");
    consume(TokenType::LPAREN, "Expected '(' after function name");

    std::vector<std::string> params;
    if (!check(TokenType::RPAREN)) {
        do {
            params.push_back(
                consume(TokenType::IDENTIFIER, "Expected parameter name").lexeme);
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RPAREN, "Expected ')' after parameters");
    consume(TokenType::LBRACE, "Expected '{' before function body");

    auto stmt = std::make_unique<FuncStmt>();
    stmt->line = name.line;
    stmt->name = name.lexeme;
    stmt->params = std::move(params);
    stmt->body = block();
    return stmt;
}

StmtPtr Parser::classStatement() {
    int ln = previous().line;
    Token name = consume(TokenType::IDENTIFIER, "Expected class name");

    std::string superclass;
    if (match(TokenType::EXTENDS)) {
        superclass = consume(TokenType::IDENTIFIER, "Expected superclass name").lexeme;
    }

    consume(TokenType::LBRACE, "Expected '{' before class body");

    std::vector<ClassMethod> methods;
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        ClassMethod method;
        method.line = peek().line;
        method.name = consume(TokenType::IDENTIFIER, "Expected method name").lexeme;
        consume(TokenType::LPAREN, "Expected '(' after method name");

        if (!check(TokenType::RPAREN)) {
            do {
                method.params.push_back(
                    consume(TokenType::IDENTIFIER, "Expected parameter name").lexeme);
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "Expected ')' after parameters");
        consume(TokenType::LBRACE, "Expected '{' before method body");

        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            method.body.push_back(statement());
        }
        consume(TokenType::RBRACE, "Expected '}' after method body");

        methods.push_back(std::move(method));
    }

    consume(TokenType::RBRACE, "Expected '}' after class body");

    auto stmt = std::make_unique<ClassStmt>();
    stmt->line = ln;
    stmt->name = name.lexeme;
    stmt->superclass = superclass;
    stmt->methods = std::move(methods);
    return stmt;
}

StmtPtr Parser::ifStatement() {
    int ln = previous().line;
    consume(TokenType::LPAREN, "Expected '(' after 'if'");
    auto condition = expression();
    consume(TokenType::RPAREN, "Expected ')' after condition");
    consume(TokenType::LBRACE, "Expected '{' after if condition");
    auto thenBranch = block();

    auto stmt = std::make_unique<IfStmt>();
    stmt->line = ln;
    stmt->condition = std::move(condition);
    stmt->thenBranch = std::move(thenBranch);

    while (match(TokenType::ELIF)) {
        consume(TokenType::LPAREN, "Expected '(' after 'elif'");
        auto elifCond = expression();
        consume(TokenType::RPAREN, "Expected ')' after condition");
        consume(TokenType::LBRACE, "Expected '{' after elif condition");
        auto elifBody = block();

        IfStmt::ElifBranch branch;
        branch.condition = std::move(elifCond);
        branch.body = std::move(elifBody);
        stmt->elifBranches.push_back(std::move(branch));
    }

    if (match(TokenType::ELSE)) {
        consume(TokenType::LBRACE, "Expected '{' after 'else'");
        stmt->elseBranch = block();
    }

    return stmt;
}

StmtPtr Parser::whileStatement() {
    int ln = previous().line;
    consume(TokenType::LPAREN, "Expected '(' after 'while'");
    auto condition = expression();
    consume(TokenType::RPAREN, "Expected ')' after condition");
    consume(TokenType::LBRACE, "Expected '{' after while condition");

    auto stmt = std::make_unique<WhileStmt>();
    stmt->line = ln;
    stmt->condition = std::move(condition);
    stmt->body = block();
    return stmt;
}

StmtPtr Parser::forStatement() {
    int ln = previous().line;
    consume(TokenType::LPAREN, "Expected '(' after 'for'");
    Token var = consume(TokenType::IDENTIFIER, "Expected loop variable");
    consume(TokenType::IN, "Expected 'in' after loop variable");
    auto iterExpr = expression();

    if (match(TokenType::DOT_DOT)) {
        // Range for: for (i in start..end)
        auto endExpr = expression();
        consume(TokenType::RPAREN, "Expected ')' after range");
        consume(TokenType::LBRACE, "Expected '{' after for");
        auto stmt = std::make_unique<ForStmt>();
        stmt->line = ln;
        stmt->varName = var.lexeme;
        stmt->start = std::move(iterExpr);
        stmt->end = std::move(endExpr);
        stmt->body = block();
        return stmt;
    }

    // For-in: for (item in array)
    consume(TokenType::RPAREN, "Expected ')' after iterable");
    consume(TokenType::LBRACE, "Expected '{' after for");
    auto stmt = std::make_unique<ForInStmt>();
    stmt->line = ln;
    stmt->varName = var.lexeme;
    stmt->iterable = std::move(iterExpr);
    stmt->body = block();
    return stmt;
}

StmtPtr Parser::returnStatement() {
    int ln = previous().line;
    auto stmt = std::make_unique<ReturnStmt>();
    stmt->line = ln;

    // Only parse a return value if the next token can start an expression
    switch (peek().type) {
        case TokenType::NUMBER:
        case TokenType::STRING:
        case TokenType::INTERP_START:
        case TokenType::TRUE:
        case TokenType::FALSE:
        case TokenType::NIL:
        case TokenType::IDENTIFIER:
        case TokenType::LPAREN:
        case TokenType::LBRACKET:
        case TokenType::LBRACE:
        case TokenType::LAM:
        case TokenType::ASYNC:
        case TokenType::AWAIT:
        case TokenType::NOT:
        case TokenType::MINUS:
            stmt->value = expression();
            break;
        default:
            break;
    }

    return stmt;
}

StmtPtr Parser::breakStatement() {
    auto stmt = std::make_unique<BreakStmt>();
    stmt->line = previous().line;
    return stmt;
}

StmtPtr Parser::continueStatement() {
    auto stmt = std::make_unique<ContinueStmt>();
    stmt->line = previous().line;
    return stmt;
}

StmtPtr Parser::throwStatement() {
    int ln = previous().line;
    auto stmt = std::make_unique<ThrowStmt>();
    stmt->line = ln;
    stmt->value = expression();
    return stmt;
}

StmtPtr Parser::tryCatchStatement() {
    int ln = previous().line;
    consume(TokenType::LBRACE, "Expected '{' after 'try'");
    auto tryBody = block();

    consume(TokenType::CATCH, "Expected 'catch' after try block");
    consume(TokenType::LPAREN, "Expected '(' after 'catch'");
    Token errVar = consume(TokenType::IDENTIFIER, "Expected error variable name");
    consume(TokenType::RPAREN, "Expected ')' after error variable");
    consume(TokenType::LBRACE, "Expected '{' after catch");
    auto catchBody = block();

    auto stmt = std::make_unique<TryCatchStmt>();
    stmt->line = ln;
    stmt->tryBody = std::move(tryBody);
    stmt->errorVar = errVar.lexeme;
    stmt->catchBody = std::move(catchBody);
    return stmt;
}

StmtPtr Parser::ensureStatement() {
    int ln = previous().line;
    consume(TokenType::LPAREN, "Expected '(' after 'ensure'");
    auto condition = expression();
    consume(TokenType::RPAREN, "Expected ')' after condition");
    consume(TokenType::ELSE, "Expected 'else' after ensure condition");
    consume(TokenType::LBRACE, "Expected '{' after 'else'");
    auto elseBody = block();

    auto stmt = std::make_unique<EnsureStmt>();
    stmt->line = ln;
    stmt->condition = std::move(condition);
    stmt->elseBody = std::move(elseBody);
    return stmt;
}

StmtPtr Parser::useStatement() {
    int ln = previous().line;
    Token path = consume(TokenType::STRING, "Expected grain path string after 'use'");

    // Derive alias from the last path segment: "utils/math" -> "math"
    std::string alias = path.lexeme;
    auto slashPos = alias.rfind('/');
    if (slashPos != std::string::npos)
        alias = alias.substr(slashPos + 1);

    auto stmt = std::make_unique<UseStmt>();
    stmt->line = ln;
    stmt->path = path.lexeme;
    stmt->alias = alias;
    return stmt;
}

StmtPtr Parser::exportStatement() {
    int ln = previous().line;
    consume(TokenType::LBRACE, "Expected '{' after 'export'");

    auto stmt = std::make_unique<ExportStmt>();
    stmt->line = ln;

    if (!check(TokenType::RBRACE)) {
        do {
            stmt->names.push_back(
                consume(TokenType::IDENTIFIER, "Expected export name").lexeme);
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RBRACE, "Expected '}' after export list");
    return stmt;
}

StmtPtr Parser::expressionStatement() {
    auto expr = expression();
    auto stmt = std::make_unique<ExprStmt>();
    stmt->line = expr->line;
    stmt->expr = std::move(expr);
    return stmt;
}

StmtPtr Parser::block() {
    auto blk = std::make_unique<BlockStmt>();
    blk->line = previous().line;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        try {
            blk->statements.push_back(statement());
        } catch (const ParseError&) {
            synchronize();
        }
    }

    consume(TokenType::RBRACE, "Expected '}'");
    return blk;
}

// ── Expressions (precedence climbing) ────────────────────────

ExprPtr Parser::expression() {
    return assignment();
}

ExprPtr Parser::assignment() {
    auto expr = pipe();

    if (match(TokenType::ASSIGN)) {
        int ln = previous().line;
        auto value = assignment(); // right-associative

        if (auto* ident = dynamic_cast<IdentifierExpr*>(expr.get())) {
            auto assign = std::make_unique<AssignExpr>();
            assign->line = ln;
            assign->name = ident->name;
            assign->value = std::move(value);
            return assign;
        }

        if (auto* idx = dynamic_cast<IndexExpr*>(expr.get())) {
            auto ia = std::make_unique<IndexAssignExpr>();
            ia->line = ln;
            ia->object = std::move(idx->object);
            ia->index = std::move(idx->index);
            ia->value = std::move(value);
            return ia;
        }

        if (auto* dot = dynamic_cast<DotExpr*>(expr.get())) {
            auto da = std::make_unique<DotAssignExpr>();
            da->line = ln;
            da->object = std::move(dot->object);
            da->field = dot->field;
            da->value = std::move(value);
            return da;
        }

        throw error(previous(), "Invalid assignment target");
    }

    return expr;
}

ExprPtr Parser::pipe() {
    auto left = logicOr();
    while (match(TokenType::PIPE)) {
        int ln = previous().line;
        auto right = logicOr();
        auto e = std::make_unique<PipeExpr>();
        e->line = ln;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::logicOr() {
    auto left = logicAnd();
    while (match(TokenType::OR)) {
        int ln = previous().line;
        auto right = logicAnd();
        auto expr = std::make_unique<BinaryExpr>();
        expr->line = ln;
        expr->left = std::move(left);
        expr->op = TokenType::OR;
        expr->right = std::move(right);
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::logicAnd() {
    auto left = equality();
    while (match(TokenType::AND)) {
        int ln = previous().line;
        auto right = equality();
        auto expr = std::make_unique<BinaryExpr>();
        expr->line = ln;
        expr->left = std::move(left);
        expr->op = TokenType::AND;
        expr->right = std::move(right);
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::equality() {
    auto left = comparison();
    while (match(TokenType::EQ) || match(TokenType::NEQ)) {
        Token op = previous();
        auto right = comparison();
        auto expr = std::make_unique<BinaryExpr>();
        expr->line = op.line;
        expr->left = std::move(left);
        expr->op = op.type;
        expr->right = std::move(right);
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::comparison() {
    auto left = addition();
    while (match(TokenType::LT) || match(TokenType::GT) ||
           match(TokenType::LTE) || match(TokenType::GTE)) {
        Token op = previous();
        auto right = addition();
        auto expr = std::make_unique<BinaryExpr>();
        expr->line = op.line;
        expr->left = std::move(left);
        expr->op = op.type;
        expr->right = std::move(right);
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::addition() {
    auto left = multiplication();
    while (match(TokenType::PLUS) || match(TokenType::MINUS)) {
        Token op = previous();
        auto right = multiplication();
        auto expr = std::make_unique<BinaryExpr>();
        expr->line = op.line;
        expr->left = std::move(left);
        expr->op = op.type;
        expr->right = std::move(right);
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::multiplication() {
    auto left = unary();
    while (match(TokenType::STAR) || match(TokenType::SLASH) ||
           match(TokenType::PERCENT)) {
        Token op = previous();
        auto right = unary();
        auto expr = std::make_unique<BinaryExpr>();
        expr->line = op.line;
        expr->left = std::move(left);
        expr->op = op.type;
        expr->right = std::move(right);
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::unary() {
    if (match(TokenType::NOT) || match(TokenType::MINUS)) {
        Token op = previous();
        auto operand = unary(); // right-recursive for chaining: !!x, --x
        auto expr = std::make_unique<UnaryExpr>();
        expr->line = op.line;
        expr->op = op.type;
        expr->operand = std::move(operand);
        return expr;
    }
    return postfix();
}

ExprPtr Parser::postfix() {
    auto expr = call();
    if (match(TokenType::INCREMENT) || match(TokenType::DECREMENT)) {
        Token op = previous();
        auto post = std::make_unique<PostfixExpr>();
        post->line = op.line;
        post->operand = std::move(expr);
        post->op = op.type;
        return post;
    }
    return expr;
}

ExprPtr Parser::call() {
    auto expr = primary();

    while (true) {
        if (match(TokenType::LPAREN)) {
            int ln = previous().line;
            std::vector<ExprPtr> args;
            if (!check(TokenType::RPAREN)) {
                do {
                    args.push_back(expression());
                } while (match(TokenType::COMMA));
            }
            consume(TokenType::RPAREN, "Expected ')' after arguments");

            auto c = std::make_unique<CallExpr>();
            c->line = ln;
            c->callee = std::move(expr);
            c->args = std::move(args);
            expr = std::move(c);
        } else if (match(TokenType::LBRACKET)) {
            int ln = previous().line;
            auto index = expression();
            consume(TokenType::RBRACKET, "Expected ']' after index");
            auto ie = std::make_unique<IndexExpr>();
            ie->line = ln;
            ie->object = std::move(expr);
            ie->index = std::move(index);
            expr = std::move(ie);
        } else if (match(TokenType::DOT)) {
            int ln = previous().line;
            Token field = consume(TokenType::IDENTIFIER, "Expected field name after '.'");
            auto de = std::make_unique<DotExpr>();
            de->line = ln;
            de->object = std::move(expr);
            de->field = field.lexeme;
            expr = std::move(de);
        } else {
            break;
        }
    }

    return expr;
}

ExprPtr Parser::primary() {
    if (match(TokenType::NUMBER)) {
        auto e = std::make_unique<NumberExpr>();
        e->line = previous().line;
        e->value = std::stod(previous().lexeme);
        return e;
    }

    if (match(TokenType::STRING)) {
        auto e = std::make_unique<StringExpr>();
        e->line = previous().line;
        e->value = previous().lexeme;
        return e;
    }

    if (match(TokenType::INTERP_START))
        return interpolatedString();

    if (match(TokenType::TRUE)) {
        auto e = std::make_unique<BoolExpr>();
        e->line = previous().line;
        e->value = true;
        return e;
    }

    if (match(TokenType::FALSE)) {
        auto e = std::make_unique<BoolExpr>();
        e->line = previous().line;
        e->value = false;
        return e;
    }

    if (match(TokenType::NIL)) {
        auto e = std::make_unique<NilExpr>();
        e->line = previous().line;
        return e;
    }

    if (match(TokenType::IDENTIFIER)) {
        auto e = std::make_unique<IdentifierExpr>();
        e->line = previous().line;
        e->name = previous().lexeme;
        return e;
    }

    if (match(TokenType::ASYNC)) {
        int ln = previous().line;
        auto expr = expression();
        auto e = std::make_unique<AsyncExpr>();
        e->line = ln;
        e->expr = std::move(expr);
        return e;
    }

    if (match(TokenType::AWAIT)) {
        int ln = previous().line;
        auto expr = expression();
        auto e = std::make_unique<AwaitExpr>();
        e->line = ln;
        e->expr = std::move(expr);
        return e;
    }

    if (match(TokenType::THIS)) {
        auto e = std::make_unique<ThisExpr>();
        e->line = previous().line;
        return e;
    }

    if (match(TokenType::SUPER)) {
        int ln = previous().line;
        consume(TokenType::DOT, "Expected '.' after 'super'");
        Token method = consume(TokenType::IDENTIFIER, "Expected method name after 'super.'");
        auto e = std::make_unique<SuperExpr>();
        e->line = ln;
        e->method = method.lexeme;
        return e;
    }

    if (match(TokenType::LPAREN)) {
        auto expr = expression();
        consume(TokenType::RPAREN, "Expected ')'");
        return expr;
    }

    if (match(TokenType::LBRACKET)) {
        int ln = previous().line;
        auto arr = std::make_unique<ArrayLiteralExpr>();
        arr->line = ln;
        if (!check(TokenType::RBRACKET)) {
            do {
                arr->elements.push_back(expression());
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RBRACKET, "Expected ']' after array elements");
        return arr;
    }

    if (match(TokenType::LAM)) {
        int ln = previous().line;
        consume(TokenType::LBRACE, "Expected '{' after 'lam'");

        // Parse optional params before 'in'
        std::vector<std::string> params;
        if (!check(TokenType::IN)) {
            do {
                params.push_back(
                    consume(TokenType::IDENTIFIER, "Expected parameter name").lexeme);
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::IN, "Expected 'in' in lambda");

        // Parse body statements until '}'
        auto lam = std::make_unique<LambdaExpr>();
        lam->line = ln;
        lam->params = std::move(params);
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            lam->body.push_back(statement());
        }
        consume(TokenType::RBRACE, "Expected '}' after lambda body");

        // If the body is a single expression statement, make it auto-return
        if (lam->body.size() == 1) {
            if (auto* es = dynamic_cast<ExprStmt*>(lam->body[0].get())) {
                auto ret = std::make_unique<ReturnStmt>();
                ret->line = es->line;
                ret->value = std::move(es->expr);
                lam->body[0] = std::move(ret);
            }
        }

        return lam;
    }

    if (match(TokenType::LBRACE)) {
        int ln = previous().line;
        auto map = std::make_unique<MapLiteralExpr>();
        map->line = ln;
        if (!check(TokenType::RBRACE)) {
            do {
                std::string key;
                if (match(TokenType::IDENTIFIER))
                    key = previous().lexeme;
                else if (match(TokenType::STRING))
                    key = previous().lexeme;
                else
                    throw error(peek(), "Expected map key (identifier or string)");
                consume(TokenType::COLON, "Expected ':' after map key");
                map->keys.push_back(key);
                map->values.push_back(expression());
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RBRACE, "Expected '}' after map");
        return map;
    }

    throw error(peek(), "Expected expression");
}

ExprPtr Parser::interpolatedString() {
    int ln = previous().line;
    auto interp = std::make_unique<InterpolatedStringExpr>();
    interp->line = ln;

    // Leading string fragment from INTERP_START
    auto first = std::make_unique<StringExpr>();
    first->line = ln;
    first->value = previous().lexeme;
    interp->parts.push_back(std::move(first));

    // First interpolated expression
    interp->parts.push_back(expression());

    // Optional middle parts: INTERP_MID string + expression
    while (match(TokenType::INTERP_MID)) {
        auto mid = std::make_unique<StringExpr>();
        mid->line = previous().line;
        mid->value = previous().lexeme;
        interp->parts.push_back(std::move(mid));
        interp->parts.push_back(expression());
    }

    // Trailing string fragment from INTERP_END
    Token end = consume(TokenType::INTERP_END, "Expected end of interpolated string");
    auto last = std::make_unique<StringExpr>();
    last->line = end.line;
    last->value = end.lexeme;
    interp->parts.push_back(std::move(last));

    return interp;
}

// ── Helpers ──────────────────────────────────────────────────

Token Parser::advance() {
    if (!isAtEnd()) current++;
    return previous();
}

Token Parser::peek() const {
    return tokens[current];
}

Token Parser::previous() const {
    return tokens[current - 1];
}

bool Parser::check(TokenType type) const {
    if (isAtEnd()) return false;
    return peek().type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) { advance(); return true; }
    return false;
}

Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) return advance();
    throw error(peek(), message);
}

bool Parser::isAtEnd() const {
    return tokens[current].type == TokenType::EOF_TOKEN;
}

Parser::ParseError Parser::error(const Token& token, const std::string& message) {
    if (token.type == TokenType::EOF_TOKEN) {
        std::cerr << "[line " << token.line << "] Error at end: " << message << std::endl;
    } else {
        std::cerr << "[line " << token.line << "] Error at '" << token.lexeme
                  << "': " << message << std::endl;
    }
    hadError = true;
    return ParseError(message);
}

void Parser::synchronize() {
    advance();
    while (!isAtEnd()) {
        if (previous().type == TokenType::RBRACE) return;
        switch (peek().type) {
            case TokenType::LET:
            case TokenType::FUNC:
            case TokenType::CLASS:
            case TokenType::IF:
            case TokenType::WHILE:
            case TokenType::FOR:
            case TokenType::RETURN:
            case TokenType::BREAK:
            case TokenType::CONTINUE:
            case TokenType::TRY:
            case TokenType::ENSURE:
            case TokenType::USE:
            case TokenType::EXPORT:
                return;
            default:
                advance();
        }
    }
}
