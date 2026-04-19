#include "compiler.h"
#include "vm.h"

void Compiler::compileExpr(const Expr* expr) {
    if (auto* e = dynamic_cast<const NumberExpr*>(expr)) compileNumberExpr(e);
    else if (auto* e = dynamic_cast<const StringExpr*>(expr)) compileStringExpr(e);
    else if (auto* e = dynamic_cast<const BoolExpr*>(expr)) compileBoolExpr(e);
    else if (dynamic_cast<const NilExpr*>(expr)) compileNilExpr(static_cast<const NilExpr*>(expr));
    else if (auto* e = dynamic_cast<const IdentifierExpr*>(expr)) compileIdentifierExpr(e);
    else if (auto* e = dynamic_cast<const AssignExpr*>(expr)) compileAssignExpr(e);
    else if (auto* e = dynamic_cast<const UnaryExpr*>(expr)) compileUnaryExpr(e);
    else if (auto* e = dynamic_cast<const PostfixExpr*>(expr)) compilePostfixExpr(e);
    else if (auto* e = dynamic_cast<const BinaryExpr*>(expr)) compileBinaryExpr(e);
    else if (auto* e = dynamic_cast<const CallExpr*>(expr)) compileCallExpr(e);
    else if (auto* e = dynamic_cast<const TernaryExpr*>(expr)) compileTernaryExpr(e);
    else if (auto* e = dynamic_cast<const PipeExpr*>(expr)) compilePipeExpr(e);
    else if (auto* e = dynamic_cast<const LambdaExpr*>(expr)) compileLambdaExpr(e);
    else if (auto* e = dynamic_cast<const ArrayLiteralExpr*>(expr)) compileArrayLiteralExpr(e);
    else if (auto* e = dynamic_cast<const MapLiteralExpr*>(expr)) compileMapLiteralExpr(e);
    else if (auto* e = dynamic_cast<const IndexExpr*>(expr)) compileIndexExpr(e);
    else if (auto* e = dynamic_cast<const IndexAssignExpr*>(expr)) compileIndexAssignExpr(e);
    else if (auto* e = dynamic_cast<const DotExpr*>(expr)) compileDotExpr(e);
    else if (auto* e = dynamic_cast<const DotAssignExpr*>(expr)) compileDotAssignExpr(e);
    else if (auto* e = dynamic_cast<const InterpolatedStringExpr*>(expr)) compileInterpolatedStringExpr(e);
    else if (auto* e = dynamic_cast<const ThisExpr*>(expr)) compileThisExpr(e);
    else if (auto* e = dynamic_cast<const SuperExpr*>(expr)) compileSuperExpr(e);
    else if (auto* e = dynamic_cast<const AsyncExpr*>(expr)) compileAsyncExpr(e);
    else if (auto* e = dynamic_cast<const AwaitExpr*>(expr)) compileAwaitExpr(e);
    else error("Unknown expression type", expr->line);
}

// ── Phase 1: Literals, arithmetic, variables, calls ─────────

void Compiler::compileNumberExpr(const NumberExpr* expr) {
    if (expr->isInt)
        emitConstant(Value(expr->intValue), expr->line);
    else
        emitConstant(Value(expr->floatValue), expr->line);
}

void Compiler::compileStringExpr(const StringExpr* expr) {
    emitConstant(Value(expr->value), expr->line);
}

void Compiler::compileBoolExpr(const BoolExpr* expr) {
    emit(expr->value ? OpCode::OP_TRUE : OpCode::OP_FALSE, expr->line);
}

void Compiler::compileNilExpr(const NilExpr* expr) {
    emit(OpCode::OP_NIL, expr->line);
}

void Compiler::compileIdentifierExpr(const IdentifierExpr* expr) {
    int slot = resolveLocal(current, expr->name);
    if (slot != -1) {
        emit(OpCode::OP_GET_LOCAL, expr->line);
        emitU16(static_cast<uint16_t>(slot), expr->line);
    } else {
        int upvalue = resolveUpvalue(current, expr->name);
        if (upvalue != -1) {
            emit(OpCode::OP_GET_UPVALUE, expr->line);
            emit(static_cast<uint8_t>(upvalue), expr->line);
        } else {
            uint16_t nameIdx = identifierConstant(expr->name);
            emit(OpCode::OP_GET_GLOBAL, expr->line);
            emitU16(nameIdx, expr->line);
        }
    }
}

void Compiler::compileAssignExpr(const AssignExpr* expr) {
    compileExpr(expr->value.get());

    int slot = resolveLocal(current, expr->name);
    if (slot != -1) {
        emit(OpCode::OP_SET_LOCAL, expr->line);
        emitU16(static_cast<uint16_t>(slot), expr->line);
    } else {
        int upvalue = resolveUpvalue(current, expr->name);
        if (upvalue != -1) {
            emit(OpCode::OP_SET_UPVALUE, expr->line);
            emit(static_cast<uint8_t>(upvalue), expr->line);
        } else {
            uint16_t nameIdx = identifierConstant(expr->name);
            emit(OpCode::OP_SET_GLOBAL, expr->line);
            emitU16(nameIdx, expr->line);
        }
    }
}

void Compiler::compileUnaryExpr(const UnaryExpr* expr) {
    compileExpr(expr->operand.get());
    switch (expr->op) {
        case TokenType::MINUS: emit(OpCode::OP_NEGATE, expr->line); break;
        case TokenType::NOT:   emit(OpCode::OP_NOT, expr->line); break;
        case TokenType::BIT_NOT: emit(OpCode::OP_BIT_NOT, expr->line); break;
        default: error("Unknown unary operator", expr->line);
    }
}

void Compiler::compilePostfixExpr(const PostfixExpr* expr) {
    auto* ident = dynamic_cast<const IdentifierExpr*>(expr->operand.get());
    if (!ident) { error("Postfix operator requires a variable", expr->line); return; }

    int slot = resolveLocal(current, ident->name);
    if (slot != -1) {
        emit(expr->op == TokenType::INCREMENT ? OpCode::OP_POST_INC_LOCAL : OpCode::OP_POST_DEC_LOCAL, expr->line);
        emitU16(static_cast<uint16_t>(slot), expr->line);
    } else {
        uint16_t nameIdx = identifierConstant(ident->name);
        emit(expr->op == TokenType::INCREMENT ? OpCode::OP_POST_INC_GLOBAL : OpCode::OP_POST_DEC_GLOBAL, expr->line);
        emitU16(nameIdx, expr->line);
    }
}

void Compiler::compileBinaryExpr(const BinaryExpr* expr) {
    // Short-circuit for && and ||
    if (expr->op == TokenType::AND) {
        compileExpr(expr->left.get());
        int endJump = emitJump(OpCode::OP_JUMP_IF_FALSE, expr->line);
        emit(OpCode::OP_POP, expr->line);
        compileExpr(expr->right.get());
        patchJump(endJump);
        return;
    }
    if (expr->op == TokenType::OR) {
        compileExpr(expr->left.get());
        int endJump = emitJump(OpCode::OP_JUMP_IF_TRUE, expr->line);
        emit(OpCode::OP_POP, expr->line);
        compileExpr(expr->right.get());
        patchJump(endJump);
        return;
    }

    compileExpr(expr->left.get());
    compileExpr(expr->right.get());

    switch (expr->op) {
        case TokenType::PLUS:    emit(OpCode::OP_ADD, expr->line); break;
        case TokenType::MINUS:   emit(OpCode::OP_SUBTRACT, expr->line); break;
        case TokenType::STAR:    emit(OpCode::OP_MULTIPLY, expr->line); break;
        case TokenType::SLASH:   emit(OpCode::OP_DIVIDE, expr->line); break;
        case TokenType::PERCENT: emit(OpCode::OP_MODULO, expr->line); break;
        case TokenType::LT:      emit(OpCode::OP_LESS, expr->line); break;
        case TokenType::GT:      emit(OpCode::OP_GREATER, expr->line); break;
        case TokenType::LTE:     emit(OpCode::OP_LESS_EQUAL, expr->line); break;
        case TokenType::GTE:     emit(OpCode::OP_GREATER_EQUAL, expr->line); break;
        case TokenType::EQ:      emit(OpCode::OP_EQUAL, expr->line); break;
        case TokenType::NEQ:     emit(OpCode::OP_NOT_EQUAL, expr->line); break;
        case TokenType::BIT_AND: emit(OpCode::OP_BIT_AND, expr->line); break;
        case TokenType::BIT_OR:  emit(OpCode::OP_BIT_OR, expr->line); break;
        case TokenType::BIT_XOR: emit(OpCode::OP_BIT_XOR, expr->line); break;
        case TokenType::SHL:     emit(OpCode::OP_SHL, expr->line); break;
        case TokenType::SHR:     emit(OpCode::OP_SHR, expr->line); break;
        default: error("Unknown binary operator", expr->line);
    }
}

void Compiler::compileCallExpr(const CallExpr* expr) {
    compileExpr(expr->callee.get());
    for (auto& arg : expr->args) {
        compileExpr(arg.get());
    }
    emit(OpCode::OP_CALL, expr->line);
    emit(static_cast<uint8_t>(expr->args.size()), expr->line);
}

void Compiler::compileTernaryExpr(const TernaryExpr* expr) {
    compileExpr(expr->condition.get());
    int elseJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, expr->line);
    compileExpr(expr->thenExpr.get());
    int endJump = emitJump(OpCode::OP_JUMP, expr->line);
    patchJump(elseJump);
    compileExpr(expr->elseExpr.get());
    patchJump(endJump);
}

void Compiler::compilePipeExpr(const PipeExpr* expr) {
    // a |> f → f(a)
    // a |> f(x) → f(a, x)
    if (auto* call = dynamic_cast<const CallExpr*>(expr->right.get())) {
        compileExpr(call->callee.get());
        compileExpr(expr->left.get());  // first arg
        for (auto& arg : call->args) compileExpr(arg.get());
        emit(OpCode::OP_CALL, expr->line);
        emit(static_cast<uint8_t>(call->args.size() + 1), expr->line);
    } else {
        compileExpr(expr->right.get());
        compileExpr(expr->left.get());
        emit(OpCode::OP_CALL, expr->line);
        emit(1, expr->line);
    }
}

// ── Phase 3+ stubs ──────────────────────────────────────────

void Compiler::compileLambdaExpr(const LambdaExpr* expr) {
    auto fn = std::make_shared<CompiledFunction>();
    fn->name = "<lambda>";
    fn->arity = static_cast<int>(expr->params.size());

    CompilerState lamState;
    lamState.enclosing = current;
    lamState.function = fn;
    lamState.scopeDepth = current->scopeDepth + 1;
    current = &lamState;

    addLocal(""); // slot 0

    for (auto& param : expr->params) {
        addLocal(param);
    }

    for (auto& s : expr->body) {
        compileStmt(s.get());
    }

    emit(OpCode::OP_NIL, expr->line);
    emit(OpCode::OP_RETURN, expr->line);

    current = lamState.enclosing;
    fn->upvalueCount = static_cast<int>(lamState.upvalues.size());

    auto* proto = new ObjClosure(fn);
    auto wrapper = std::make_shared<VMClosureCallable>(proto);
    uint16_t fnIdx = currentChunk().addConstant(
        Value(std::static_pointer_cast<Callable>(wrapper)));

    emit(OpCode::OP_CLOSURE, expr->line);
    emitU16(fnIdx, expr->line);

    for (auto& uv : lamState.upvalues) {
        emit(uv.isLocal ? 1 : 0, expr->line);
        emitU16(uv.index, expr->line);
    }
}
void Compiler::compileArrayLiteralExpr(const ArrayLiteralExpr* expr) { error("arrays not yet in VM", expr->line); }
void Compiler::compileMapLiteralExpr(const MapLiteralExpr* expr) { error("maps not yet in VM", expr->line); }
void Compiler::compileIndexExpr(const IndexExpr* expr) { error("indexing not yet in VM", expr->line); }
void Compiler::compileIndexAssignExpr(const IndexAssignExpr* expr) { error("index assign not yet in VM", expr->line); }
void Compiler::compileDotExpr(const DotExpr* expr) {
    compileExpr(expr->object.get());
    uint16_t nameIdx = identifierConstant(expr->field);
    emit(OpCode::OP_GET_PROPERTY, expr->line);
    emitU16(nameIdx, expr->line);
}

void Compiler::compileDotAssignExpr(const DotAssignExpr* expr) {
    compileExpr(expr->object.get());
    compileExpr(expr->value.get());
    uint16_t nameIdx = identifierConstant(expr->field);
    emit(OpCode::OP_SET_PROPERTY, expr->line);
    emitU16(nameIdx, expr->line);
}
void Compiler::compileInterpolatedStringExpr(const InterpolatedStringExpr* expr) { error("interp string not yet in VM", expr->line); }
void Compiler::compileThisExpr(const ThisExpr* expr) {
    // "this" is always local slot 0 in a method
    int slot = resolveLocal(current, "this");
    if (slot != -1) {
        emit(OpCode::OP_GET_LOCAL, expr->line);
        emitU16(static_cast<uint16_t>(slot), expr->line);
    } else {
        int uv = resolveUpvalue(current, "this");
        if (uv != -1) {
            emit(OpCode::OP_GET_UPVALUE, expr->line);
            emit(static_cast<uint8_t>(uv), expr->line);
        } else {
            error("'this' used outside of a method", expr->line);
        }
    }
}

void Compiler::compileSuperExpr(const SuperExpr* expr) {
    // Push "this" for method binding
    int slot = resolveLocal(current, "this");
    if (slot != -1) {
        emit(OpCode::OP_GET_LOCAL, expr->line);
        emitU16(static_cast<uint16_t>(slot), expr->line);
    } else {
        int uv = resolveUpvalue(current, "this");
        if (uv != -1) {
            emit(OpCode::OP_GET_UPVALUE, expr->line);
            emit(static_cast<uint8_t>(uv), expr->line);
        } else {
            error("'super' used outside of a method", expr->line);
        }
    }
    uint16_t nameIdx = identifierConstant(expr->method);
    emit(OpCode::OP_GET_SUPER, expr->line);
    emitU16(nameIdx, expr->line);
}
void Compiler::compileAsyncExpr(const AsyncExpr* expr) { error("async not yet in VM", expr->line); }
void Compiler::compileAwaitExpr(const AwaitExpr* expr) { error("await not yet in VM", expr->line); }
