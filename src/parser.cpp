#include "parser.h"
#include <iostream>

Parser::Parser(const std::vector<Token>& tokens) : tokens(tokens) {}

std::vector<StmtPtr> Parser::parse() {
    std::vector<StmtPtr> statements;
    while (!isAtEnd()) {
        try {
            statements.push_back(statement());
            // Drain pending statements (emitted by decorator desugaring)
            while (!pending_.empty()) {
                statements.push_back(std::move(pending_.front()));
                pending_.erase(pending_.begin());
            }
        } catch (const ParseError&) {
            synchronize();
        }
    }
    return statements;
}

// ── Statements ───────────────────────────────────────────────

StmtPtr Parser::statement() {
    // Decorator: @expr func name(...) { ... }
    // Desugars to: func name(...){...}; name = expr(name)
    if (check(TokenType::AT)) {
        std::vector<ExprPtr> decorators;
        while (match(TokenType::AT)) {
            decorators.push_back(call());
        }
        if (!match(TokenType::FUNC))
            throw error(peek(), "Expected 'func' after decorator");
        auto funcStmt = funcStatement();
        std::string funcName = static_cast<FuncStmt*>(funcStmt.get())->name;
        int ln = static_cast<FuncStmt*>(funcStmt.get())->line;

        // Build reassignment: name = outer(inner(name))
        // decorators[0] is outermost, decorators[last] is innermost (closest to func)
        ExprPtr wrapped = std::make_unique<IdentifierExpr>();
        static_cast<IdentifierExpr*>(wrapped.get())->name = funcName;
        static_cast<IdentifierExpr*>(wrapped.get())->line = ln;

        for (int i = static_cast<int>(decorators.size()) - 1; i >= 0; i--) {
            auto callExpr = std::make_unique<CallExpr>();
            callExpr->line = ln;
            callExpr->callee = std::move(decorators[i]);
            callExpr->args.push_back(std::move(wrapped));
            wrapped = std::move(callExpr);
        }

        auto assign = std::make_unique<AssignExpr>();
        assign->line = ln;
        assign->name = funcName;
        assign->value = std::move(wrapped);

        auto assignStmt = std::make_unique<ExprStmt>();
        assignStmt->line = ln;
        assignStmt->expr = std::move(assign);
        pending_.push_back(std::move(assignStmt));

        return funcStmt;
    }

    if (match(TokenType::LET))    return letStatement();
    if (match(TokenType::FUNC))   return funcStatement();
    if (match(TokenType::CLASS))  return classStatement();
    if (match(TokenType::ENUM))   return enumStatement();
    if (match(TokenType::IF))     return ifStatement();
    if (match(TokenType::MATCH))  return matchStatement();
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
    int ln = previous().line;
    auto stmt = std::make_unique<LetStmt>();
    stmt->line = ln;

    // Array destructuring: let [a, b, ...rest] = expr
    if (match(TokenType::LBRACKET)) {
        stmt->isArrayPattern = true;
        if (!check(TokenType::RBRACKET)) {
            do {
                PatternEntry entry;
                if (match(TokenType::SPREAD)) {
                    entry.name = consume(TokenType::IDENTIFIER, "Expected name after '...'").lexeme;
                    entry.isRest = true;
                } else {
                    entry.name = consume(TokenType::IDENTIFIER, "Expected variable name").lexeme;
                }
                stmt->pattern.push_back(std::move(entry));
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RBRACKET, "Expected ']' after destructuring pattern");
        consume(TokenType::ASSIGN, "Expected '=' after destructuring pattern");
        stmt->initializer = expression();
        return stmt;
    }

    // Map destructuring: let {name, age, ...rest} = expr
    if (match(TokenType::LBRACE)) {
        stmt->isArrayPattern = false;
        if (!check(TokenType::RBRACE)) {
            do {
                PatternEntry entry;
                if (match(TokenType::SPREAD)) {
                    entry.name = consume(TokenType::IDENTIFIER, "Expected name after '...'").lexeme;
                    entry.isRest = true;
                } else {
                    if (!isNameToken(peek().type))
                        throw error(peek(), "Expected variable name in destructuring");
                    entry.name = advance().lexeme;
                    entry.key = entry.name;
                    // Optional rename: {key: varName}
                    if (match(TokenType::COLON)) {
                        entry.key = entry.name;
                        entry.name = consume(TokenType::IDENTIFIER, "Expected variable name after ':'").lexeme;
                    }
                }
                stmt->pattern.push_back(std::move(entry));
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RBRACE, "Expected '}' after destructuring pattern");
        consume(TokenType::ASSIGN, "Expected '=' after destructuring pattern");
        stmt->initializer = expression();
        return stmt;
    }

    // Simple: let name = expr
    Token name = consume(TokenType::IDENTIFIER, "Expected variable name after 'let'");
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
    std::vector<ExprPtr> defaults;
    bool seenDefault = false;
    if (!check(TokenType::RPAREN)) {
        do {
            params.push_back(
                consume(TokenType::IDENTIFIER, "Expected parameter name").lexeme);
            if (match(TokenType::ASSIGN)) {
                defaults.push_back(expression());
                seenDefault = true;
            } else {
                if (seenDefault)
                    throw error(previous(), "Non-default parameter after default parameter");
                defaults.push_back(nullptr);
            }
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RPAREN, "Expected ')' after parameters");
    consume(TokenType::LBRACE, "Expected '{' before function body");

    auto stmt = std::make_unique<FuncStmt>();
    stmt->line = name.line;
    stmt->name = name.lexeme;
    stmt->params = std::move(params);
    stmt->defaults = std::move(defaults);
    int savedYieldCount = yieldCount;
    yieldCount = 0;
    functionDepth++;
    stmt->body = block();
    functionDepth--;
    stmt->isGenerator = (yieldCount > 0);
    yieldCount = savedYieldCount;
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
        consume(TokenType::FUNC, "Expected 'func' before method name");
        method.name = consume(TokenType::IDENTIFIER, "Expected method name").lexeme;
        consume(TokenType::LPAREN, "Expected '(' after method name");

        bool seenMethodDefault = false;
        if (!check(TokenType::RPAREN)) {
            do {
                method.params.push_back(
                    consume(TokenType::IDENTIFIER, "Expected parameter name").lexeme);
                if (match(TokenType::ASSIGN)) {
                    method.defaults.push_back(expression());
                    seenMethodDefault = true;
                } else {
                    if (seenMethodDefault)
                        throw error(previous(), "Non-default parameter after default parameter");
                    method.defaults.push_back(nullptr);
                }
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "Expected ')' after parameters");
        consume(TokenType::LBRACE, "Expected '{' before method body");

        int savedYieldCount = yieldCount;
        yieldCount = 0;
        functionDepth++;
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            method.body.push_back(statement());
        }
        functionDepth--;
        method.isGenerator = (yieldCount > 0);
        yieldCount = savedYieldCount;
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

StmtPtr Parser::enumStatement() {
    int ln = previous().line;
    Token name = consume(TokenType::IDENTIFIER, "Expected enum name");
    consume(TokenType::LBRACE, "Expected '{' after enum name");

    auto stmt = std::make_unique<EnumStmt>();
    stmt->line = ln;
    stmt->name = name.lexeme;

    if (!check(TokenType::RBRACE)) {
        do {
            if (!isNameToken(peek().type))
                throw error(peek(), "Expected enum member name");
            stmt->members.push_back(advance().lexeme);
            if (match(TokenType::ASSIGN)) {
                stmt->values.push_back(expression());
            } else {
                stmt->values.push_back(nullptr);
            }
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RBRACE, "Expected '}' after enum body");
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

StmtPtr Parser::matchStatement() {
    int ln = previous().line;
    consume(TokenType::LPAREN, "Expected '(' after 'match'");
    auto subject = expression();
    consume(TokenType::RPAREN, "Expected ')' after match subject");
    consume(TokenType::LBRACE, "Expected '{' after match subject");

    auto stmt = std::make_unique<MatchStmt>();
    stmt->line = ln;
    stmt->subject = std::move(subject);

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        MatchStmt::CaseBranch branch;
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "_") {
            advance(); // consume '_'
            branch.pattern = nullptr;
        } else {
            branch.pattern = expression();
        }
        consume(TokenType::LBRACE, "Expected '{' after match case");
        branch.body = block();
        stmt->cases.push_back(std::move(branch));
        if (!stmt->cases.back().pattern) break; // default must be last
    }

    consume(TokenType::RBRACE, "Expected '}' to close match");
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
    loopDepth++;
    stmt->body = block();
    loopDepth--;
    return stmt;
}

StmtPtr Parser::forStatement() {
    int ln = previous().line;
    consume(TokenType::LPAREN, "Expected '(' after 'for'");

    // Check for destructuring: for ({key, value} in ...)
    std::vector<std::string> destructureKeys;
    std::string varName;

    if (match(TokenType::LBRACE)) {
        // Map destructuring pattern
        do {
            destructureKeys.push_back(
                consume(TokenType::IDENTIFIER, "Expected identifier in destructuring pattern").lexeme);
        } while (match(TokenType::COMMA));
        consume(TokenType::RBRACE, "Expected '}' after destructuring pattern");
        consume(TokenType::IN, "Expected 'in' after destructuring pattern");
    } else {
        Token var = consume(TokenType::IDENTIFIER, "Expected loop variable");
        varName = var.lexeme;
        consume(TokenType::IN, "Expected 'in' after loop variable");
    }

    auto iterExpr = expression();

    if (destructureKeys.empty() && match(TokenType::DOT_DOT)) {
        // Range for: for (i in start..end)
        auto endExpr = expression();
        consume(TokenType::RPAREN, "Expected ')' after range");
        consume(TokenType::LBRACE, "Expected '{' after for");
        auto stmt = std::make_unique<ForStmt>();
        stmt->line = ln;
        stmt->varName = varName;
        stmt->start = std::move(iterExpr);
        stmt->end = std::move(endExpr);
        loopDepth++;
        stmt->body = block();
        loopDepth--;
        return stmt;
    }

    // For-in: for (item in array) or for ({key, value} in map)
    consume(TokenType::RPAREN, "Expected ')' after iterable");
    consume(TokenType::LBRACE, "Expected '{' after for");
    auto stmt = std::make_unique<ForInStmt>();
    stmt->line = ln;
    stmt->varName = varName;
    stmt->destructureKeys = destructureKeys;
    stmt->iterable = std::move(iterExpr);
    loopDepth++;
    stmt->body = block();
    loopDepth--;
    return stmt;
}

StmtPtr Parser::returnStatement() {
    Token tok = previous();
    if (functionDepth == 0)
        throw error(tok, "'return' outside of function");
    int ln = tok.line;
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
        case TokenType::YIELD:
        case TokenType::THIS:
        case TokenType::SUPER:
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
    Token tok = previous();
    if (loopDepth == 0)
        throw error(tok, "'break' outside of loop");
    auto stmt = std::make_unique<BreakStmt>();
    stmt->line = tok.line;
    return stmt;
}

StmtPtr Parser::continueStatement() {
    Token tok = previous();
    if (loopDepth == 0)
        throw error(tok, "'continue' outside of loop");
    auto stmt = std::make_unique<ContinueStmt>();
    stmt->line = tok.line;
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

    // Optional: use "path" as customName
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "as") {
        advance(); // consume "as"
        alias = consume(TokenType::IDENTIFIER, "Expected alias name after 'as'").lexeme;
    }

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
            while (!pending_.empty()) {
                blk->statements.push_back(std::move(pending_.front()));
                pending_.erase(pending_.begin());
            }
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

    // Check for compound assignment: +=, -=, *=, /=, %=
    TokenType compoundOp = TokenType::EOF_TOKEN;
    if (match(TokenType::PLUS_ASSIGN))    compoundOp = TokenType::PLUS;
    else if (match(TokenType::MINUS_ASSIGN))  compoundOp = TokenType::MINUS;
    else if (match(TokenType::STAR_ASSIGN))   compoundOp = TokenType::STAR;
    else if (match(TokenType::SLASH_ASSIGN))  compoundOp = TokenType::SLASH;
    else if (match(TokenType::PERCENT_ASSIGN)) compoundOp = TokenType::PERCENT;

    if (compoundOp != TokenType::EOF_TOKEN) {
        int ln = previous().line;
        auto value = assignment();

        if (auto* ident = dynamic_cast<IdentifierExpr*>(expr.get())) {
            // x += expr → x = x + expr
            auto lhs = std::make_unique<IdentifierExpr>();
            lhs->line = ln;
            lhs->name = ident->name;
            auto binop = std::make_unique<BinaryExpr>();
            binop->line = ln;
            binop->left = std::move(lhs);
            binop->op = compoundOp;
            binop->right = std::move(value);
            auto assign = std::make_unique<AssignExpr>();
            assign->line = ln;
            assign->name = ident->name;
            assign->value = std::move(binop);
            return assign;
        }

        throw error(previous(), "Compound assignment only works on variables");
    }

    if (match(TokenType::ASSIGN)) {
        int ln = previous().line;
        auto value = assignment();

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
    auto left = ternary();
    while (match(TokenType::PIPE)) {
        int ln = previous().line;
        auto right = ternary();
        auto e = std::make_unique<PipeExpr>();
        e->line = ln;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ternary() {
    auto expr = logicOr();
    if (match(TokenType::QUESTION)) {
        int ln = previous().line;
        auto thenExpr = expression(); // full expression between ? and :
        consume(TokenType::COLON, "Expected ':' in ternary expression");
        auto elseExpr = ternary(); // right-associative
        auto t = std::make_unique<TernaryExpr>();
        t->line = ln;
        t->condition = std::move(expr);
        t->thenExpr = std::move(thenExpr);
        t->elseExpr = std::move(elseExpr);
        return t;
    }
    return expr;
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
    auto left = bitOr();
    while (match(TokenType::AND)) {
        int ln = previous().line;
        auto right = bitOr();
        auto expr = std::make_unique<BinaryExpr>();
        expr->line = ln;
        expr->left = std::move(left);
        expr->op = TokenType::AND;
        expr->right = std::move(right);
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::bitOr() {
    auto left = bitXor();
    while (match(TokenType::BIT_OR)) {
        Token op = previous();
        auto right = bitXor();
        auto expr = std::make_unique<BinaryExpr>();
        expr->line = op.line;
        expr->left = std::move(left);
        expr->op = op.type;
        expr->right = std::move(right);
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::bitXor() {
    auto left = bitAnd();
    while (match(TokenType::BIT_XOR)) {
        Token op = previous();
        auto right = bitAnd();
        auto expr = std::make_unique<BinaryExpr>();
        expr->line = op.line;
        expr->left = std::move(left);
        expr->op = op.type;
        expr->right = std::move(right);
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::bitAnd() {
    auto left = equality();
    while (match(TokenType::BIT_AND)) {
        Token op = previous();
        auto right = equality();
        auto expr = std::make_unique<BinaryExpr>();
        expr->line = op.line;
        expr->left = std::move(left);
        expr->op = op.type;
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
    auto left = shift();
    while (match(TokenType::LT) || match(TokenType::GT) ||
           match(TokenType::LTE) || match(TokenType::GTE)) {
        Token op = previous();
        auto right = shift();
        auto expr = std::make_unique<BinaryExpr>();
        expr->line = op.line;
        expr->left = std::move(left);
        expr->op = op.type;
        expr->right = std::move(right);
        left = std::move(expr);
    }
    return left;
}

ExprPtr Parser::shift() {
    auto left = addition();
    while (match(TokenType::SHL) || match(TokenType::SHR)) {
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
    if (match(TokenType::NOT) || match(TokenType::MINUS) || match(TokenType::BIT_NOT)) {
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
            std::vector<std::string> argNames;
            bool seenNamed = false;
            if (!check(TokenType::RPAREN)) {
                do {
                    // Named argument: identifier followed by COLON
                    if (check(TokenType::IDENTIFIER) &&
                        current + 1 < static_cast<int>(tokens.size()) &&
                        tokens[current + 1].type == TokenType::COLON) {
                        seenNamed = true;
                        std::string argName = advance().lexeme; // consume identifier
                        advance(); // consume colon
                        for (auto& existing : argNames) {
                            if (existing == argName)
                                throw error(previous(), "Duplicate named argument '" + argName + "'");
                        }
                        argNames.push_back(argName);
                        args.push_back(expression());
                    } else {
                        if (seenNamed)
                            throw error(peek(), "Positional argument after named argument");
                        argNames.push_back(""); // empty = positional
                        args.push_back(expression());
                    }
                } while (match(TokenType::COMMA));
            }
            consume(TokenType::RPAREN, "Expected ')' after arguments");

            auto c = std::make_unique<CallExpr>();
            c->line = ln;
            c->callee = std::move(expr);
            c->args = std::move(args);
            c->argNames = std::move(argNames);
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
            // Accept identifiers and keywords as field names (e.g. app.use, obj.class)
            if (!isNameToken(peek().type))
                throw error(peek(), "Expected field name after '.'");
            Token field = advance();
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
        std::string lex = previous().lexeme;
        if (lex.find('.') == std::string::npos) {
            try {
                e->isInt = true;
                e->intValue = std::stoll(lex, nullptr, 0); // base 0: auto-detect hex/oct/dec
            } catch (const std::out_of_range&) {
                error(previous(), "Integer literal too large: " + lex);
                e->isInt = true;
                e->intValue = 0;
            }
        } else {
            try {
                e->isInt = false;
                e->floatValue = std::stod(lex);
            } catch (const std::out_of_range&) {
                error(previous(), "Float literal too large: " + lex);
                e->isInt = false;
                e->floatValue = 0.0;
            }
        }
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
        auto expr = unary();
        auto e = std::make_unique<AwaitExpr>();
        e->line = ln;
        e->expr = std::move(expr);
        return e;
    }

    if (match(TokenType::YIELD)) {
        if (functionDepth == 0)
            throw error(previous(), "'yield' outside of function");
        int ln = previous().line;
        yieldCount++;
        auto e = std::make_unique<YieldExpr>();
        e->line = ln;
        // Parse optional value — check if what follows could be a value expression
        if (!check(TokenType::RBRACE) && !check(TokenType::EOF_TOKEN) && !isAtEnd()) {
            auto t = peek().type;
            if (t == TokenType::NUMBER || t == TokenType::STRING ||
                t == TokenType::IDENTIFIER || t == TokenType::LPAREN ||
                t == TokenType::LBRACKET || t == TokenType::LBRACE ||
                t == TokenType::MINUS || t == TokenType::NOT ||
                t == TokenType::TRUE || t == TokenType::FALSE ||
                t == TokenType::NIL || t == TokenType::THIS ||
                t == TokenType::SUPER || t == TokenType::LAM ||
                t == TokenType::ASYNC || t == TokenType::AWAIT ||
                t == TokenType::YIELD || t == TokenType::INTERP_START) {
                e->value = expression();
            }
        }
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
                if (match(TokenType::SPREAD)) {
                    auto spread = std::make_unique<SpreadExpr>();
                    spread->line = previous().line;
                    spread->expr = expression();
                    arr->elements.push_back(std::move(spread));
                } else {
                    arr->elements.push_back(expression());
                }
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
        std::vector<ExprPtr> defaults;
        bool seenLamDefault = false;
        if (!check(TokenType::IN)) {
            do {
                params.push_back(
                    consume(TokenType::IDENTIFIER, "Expected parameter name").lexeme);
                if (match(TokenType::ASSIGN)) {
                    defaults.push_back(expression());
                    seenLamDefault = true;
                } else {
                    if (seenLamDefault)
                        throw error(previous(), "Non-default parameter after default parameter");
                    defaults.push_back(nullptr);
                }
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::IN, "Expected 'in' in lambda");

        // Parse body statements until '}'
        auto lam = std::make_unique<LambdaExpr>();
        lam->line = ln;
        lam->params = std::move(params);
        lam->defaults = std::move(defaults);
        int savedYieldCount = yieldCount;
        yieldCount = 0;
        functionDepth++;
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            lam->body.push_back(statement());
            while (!pending_.empty()) {
                lam->body.push_back(std::move(pending_.front()));
                pending_.erase(pending_.begin());
            }
        }
        functionDepth--;
        lam->isGenerator = (yieldCount > 0);
        yieldCount = savedYieldCount;
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
                // Spread: {...other}
                if (match(TokenType::SPREAD)) {
                    auto spread = std::make_unique<SpreadExpr>();
                    spread->line = previous().line;
                    spread->expr = expression();
                    map->keys.push_back("");  // empty key = spread
                    map->values.push_back(std::move(spread));
                    continue;
                }
                std::string key;
                if (isNameToken(peek().type)) {
                    key = advance().lexeme;
                } else if (match(TokenType::STRING)) {
                    key = previous().lexeme;
                } else {
                    throw error(peek(), "Expected map key (identifier or string)");
                }
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

bool Parser::isNameToken(TokenType t) const {
    return t == TokenType::IDENTIFIER ||
           t == TokenType::LET || t == TokenType::FUNC || t == TokenType::CLASS || t == TokenType::ENUM ||
           t == TokenType::IF || t == TokenType::ELSE || t == TokenType::ELIF || t == TokenType::MATCH ||
           t == TokenType::WHILE || t == TokenType::FOR || t == TokenType::IN ||
           t == TokenType::RETURN || t == TokenType::BREAK || t == TokenType::CONTINUE ||
           t == TokenType::TRY || t == TokenType::CATCH || t == TokenType::THROW ||
           t == TokenType::ENSURE || t == TokenType::USE || t == TokenType::EXPORT ||
           t == TokenType::EXTENDS || t == TokenType::THIS || t == TokenType::SUPER ||
           t == TokenType::LAM || t == TokenType::ASYNC || t == TokenType::AWAIT || t == TokenType::YIELD ||
           t == TokenType::TRUE || t == TokenType::FALSE || t == TokenType::NIL;
}

void Parser::synchronize() {
    advance();
    while (!isAtEnd()) {
        if (previous().type == TokenType::RBRACE) return;
        switch (peek().type) {
            case TokenType::LET:
            case TokenType::FUNC:
            case TokenType::CLASS:
            case TokenType::ENUM:
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
