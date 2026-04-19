#include "compiler.h"
#include "vm.h"
#include <iostream>

// ── Helpers ──────────────────────────────────────────────────

Chunk& Compiler::currentChunk() { return current->function->chunk; }

void Compiler::emit(OpCode op, int line) { currentChunk().write(op, line); }
void Compiler::emit(uint8_t byte, int line) { currentChunk().write(byte, line); }
void Compiler::emitU16(uint16_t val, int line) { currentChunk().writeU16(val, line); }

void Compiler::emitConstant(Value value, int line) {
    uint16_t idx = currentChunk().addConstant(std::move(value));
    emit(OpCode::OP_CONSTANT, line);
    emitU16(idx, line);
}

uint16_t Compiler::identifierConstant(const std::string& name) {
    return currentChunk().addConstant(Value(name));
}

int Compiler::emitJump(OpCode op, int line) {
    emit(op, line);
    emit(0xFF, line); // placeholder
    emit(0xFF, line);
    return currentChunk().size() - 2;
}

void Compiler::patchJump(int offset) {
    int jump = currentChunk().size() - offset - 2;
    currentChunk().patchU16(offset, static_cast<uint16_t>(jump));
}

void Compiler::emitLoop(int loopStart, int line) {
    emit(OpCode::OP_JUMP_BACK, line);
    int offset = currentChunk().size() - loopStart + 2;
    emitU16(static_cast<uint16_t>(offset), line);
}

void Compiler::error(const std::string& msg, int line) {
    std::cerr << "[line " << line << "] Compile error: " << msg << std::endl;
    hadError = true;
}

// ── Scope ────────────────────────────────────────────────────

void Compiler::beginScope() { current->scopeDepth++; }

void Compiler::endScope(int line) {
    current->scopeDepth--;
    while (!current->locals.empty() &&
           current->locals.back().depth > current->scopeDepth) {
        if (current->locals.back().isCaptured) {
            emit(OpCode::OP_CLOSE_UPVALUE, line);
        } else {
            emit(OpCode::OP_POP, line);
        }
        current->locals.pop_back();
    }
}

void Compiler::addLocal(const std::string& name) {
    current->locals.push_back({name, current->scopeDepth, false});
}

int Compiler::resolveLocal(CompilerState* state, const std::string& name) {
    for (int i = static_cast<int>(state->locals.size()) - 1; i >= 0; i--) {
        if (state->locals[i].name == name) return i;
    }
    return -1;
}

int Compiler::addUpvalue(CompilerState* state, uint16_t index, bool isLocal) {
    for (int i = 0; i < static_cast<int>(state->upvalues.size()); i++) {
        auto& uv = state->upvalues[i];
        if (uv.index == index && uv.isLocal == isLocal) return i;
    }
    state->upvalues.push_back({index, isLocal});
    state->function->upvalueCount = static_cast<int>(state->upvalues.size());
    return static_cast<int>(state->upvalues.size()) - 1;
}

int Compiler::resolveUpvalue(CompilerState* state, const std::string& name) {
    if (!state->enclosing) return -1;

    int local = resolveLocal(state->enclosing, name);
    if (local != -1) {
        state->enclosing->locals[local].isCaptured = true;
        return addUpvalue(state, static_cast<uint16_t>(local), true);
    }

    int upvalue = resolveUpvalue(state->enclosing, name);
    if (upvalue != -1) {
        return addUpvalue(state, static_cast<uint16_t>(upvalue), false);
    }

    return -1;
}

// ── Compile entry point ──────────────────────────────────────

std::shared_ptr<CompiledFunction> Compiler::compile(const std::vector<StmtPtr>& program) {
    auto fn = std::make_shared<CompiledFunction>();
    fn->name = "<script>";

    CompilerState state;
    state.function = fn;
    current = &state;

    // Reserve slot 0 for the script itself (convention from Crafting Interpreters)
    addLocal("");

    for (auto& stmt : program) {
        compileStmt(stmt.get());
    }

    emit(OpCode::OP_NIL, 0);
    emit(OpCode::OP_RETURN, 0);

    current = nullptr;
    return hadError ? nullptr : fn;
}

// ── Statement compilation ────────────────────────────────────

void Compiler::compileStmt(const Stmt* stmt) {
    if (auto* s = dynamic_cast<const ExprStmt*>(stmt)) compileExprStmt(s);
    else if (auto* s = dynamic_cast<const LetStmt*>(stmt)) compileLetStmt(s);
    else if (auto* s = dynamic_cast<const BlockStmt*>(stmt)) compileBlockStmt(s);
    else if (auto* s = dynamic_cast<const IfStmt*>(stmt)) compileIfStmt(s);
    else if (auto* s = dynamic_cast<const WhileStmt*>(stmt)) compileWhileStmt(s);
    else if (auto* s = dynamic_cast<const ForStmt*>(stmt)) compileForStmt(s);
    else if (auto* s = dynamic_cast<const ForInStmt*>(stmt)) compileForInStmt(s);
    else if (auto* s = dynamic_cast<const FuncStmt*>(stmt)) compileFuncStmt(s);
    else if (auto* s = dynamic_cast<const ReturnStmt*>(stmt)) compileReturnStmt(s);
    else if (auto* s = dynamic_cast<const BreakStmt*>(stmt)) compileBreakStmt(s);
    else if (auto* s = dynamic_cast<const ContinueStmt*>(stmt)) compileContinueStmt(s);
    else if (auto* s = dynamic_cast<const ClassStmt*>(stmt)) compileClassStmt(s);
    else if (auto* s = dynamic_cast<const EnumStmt*>(stmt)) compileEnumStmt(s);
    else if (auto* s = dynamic_cast<const ThrowStmt*>(stmt)) compileThrowStmt(s);
    else if (auto* s = dynamic_cast<const TryCatchStmt*>(stmt)) compileTryCatchStmt(s);
    else if (auto* s = dynamic_cast<const EnsureStmt*>(stmt)) compileEnsureStmt(s);
    else if (auto* s = dynamic_cast<const UseStmt*>(stmt)) compileUseStmt(s);
    else if (auto* s = dynamic_cast<const ExportStmt*>(stmt)) compileExportStmt(s);
    else error("Unknown statement type", stmt->line);
}

void Compiler::compileExprStmt(const ExprStmt* stmt) {
    compileExpr(stmt->expr.get());
    emit(OpCode::OP_POP, stmt->line);
}

void Compiler::compileLetStmt(const LetStmt* stmt) {
    // Simple let (no destructuring for now)
    if (stmt->initializer) {
        compileExpr(stmt->initializer.get());
    } else {
        emit(OpCode::OP_NIL, stmt->line);
    }

    if (current->scopeDepth > 0) {
        // Local variable
        addLocal(stmt->name);
    } else {
        // Global variable
        uint16_t nameIdx = identifierConstant(stmt->name);
        emit(OpCode::OP_DEFINE_GLOBAL, stmt->line);
        emitU16(nameIdx, stmt->line);
    }
}

void Compiler::compileBlockStmt(const BlockStmt* stmt) {
    beginScope();
    for (auto& s : stmt->statements) {
        compileStmt(s.get());
    }
    endScope(stmt->line);
}

// ── Phase 2+ stubs (will be implemented incrementally) ──────

void Compiler::compileIfStmt(const IfStmt* stmt) {
    // Condition
    compileExpr(stmt->condition.get());
    int thenJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line);

    // Then branch
    compileStmt(stmt->thenBranch.get());
    int elseJump = emitJump(OpCode::OP_JUMP, stmt->line);

    patchJump(thenJump);

    // Elif branches
    std::vector<int> elifEndJumps;
    for (auto& elif : stmt->elifBranches) {
        compileExpr(elif.condition.get());
        int elifJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line);
        compileStmt(elif.body.get());
        elifEndJumps.push_back(emitJump(OpCode::OP_JUMP, stmt->line));
        patchJump(elifJump);
    }

    // Else branch
    if (stmt->elseBranch) {
        compileStmt(stmt->elseBranch.get());
    }

    patchJump(elseJump);
    for (int j : elifEndJumps) patchJump(j);
}

void Compiler::compileWhileStmt(const WhileStmt* stmt) {
    int loopStart = currentChunk().size();

    current->loops.push_back({loopStart, {}, current->scopeDepth});

    compileExpr(stmt->condition.get());
    int exitJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line);

    compileStmt(stmt->body.get());
    emitLoop(loopStart, stmt->line);

    patchJump(exitJump);

    // Patch break jumps
    auto& loop = current->loops.back();
    for (int j : loop.breakJumps) patchJump(j);
    current->loops.pop_back();
}

void Compiler::compileForStmt(const ForStmt* stmt) {
    beginScope();

    // Evaluate bounds
    compileExpr(stmt->start.get());
    addLocal(stmt->varName);  // loop var

    compileExpr(stmt->end.get());
    addLocal("__end__");  // hidden end bound

    int loopStart = currentChunk().size();
    current->loops.push_back({loopStart, {}, current->scopeDepth});

    // Condition: i < end
    int iSlot = resolveLocal(current, stmt->varName);
    int endSlot = resolveLocal(current, "__end__");
    emit(OpCode::OP_GET_LOCAL, stmt->line);
    emitU16(static_cast<uint16_t>(iSlot), stmt->line);
    emit(OpCode::OP_GET_LOCAL, stmt->line);
    emitU16(static_cast<uint16_t>(endSlot), stmt->line);
    emit(OpCode::OP_LESS, stmt->line);
    int exitJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line);

    // Body
    compileStmt(stmt->body.get());

    // Increment: i = i + 1
    emit(OpCode::OP_GET_LOCAL, stmt->line);
    emitU16(static_cast<uint16_t>(iSlot), stmt->line);
    emitConstant(Value(static_cast<int64_t>(1)), stmt->line);
    emit(OpCode::OP_ADD, stmt->line);
    emit(OpCode::OP_SET_LOCAL, stmt->line);
    emitU16(static_cast<uint16_t>(iSlot), stmt->line);
    emit(OpCode::OP_POP, stmt->line);

    emitLoop(loopStart, stmt->line);
    patchJump(exitJump);

    auto& loop = current->loops.back();
    for (int j : loop.breakJumps) patchJump(j);
    current->loops.pop_back();

    endScope(stmt->line);
}

void Compiler::compileForInStmt(const ForInStmt* stmt) {
    // TODO: Phase 5
    error("for-in not yet implemented in VM", stmt->line);
}

void Compiler::compileFuncStmt(const FuncStmt* stmt) {
    // Compile the function body into a new CompiledFunction
    auto fn = std::make_shared<CompiledFunction>();
    fn->name = stmt->name;
    fn->arity = static_cast<int>(stmt->params.size());

    CompilerState funcState;
    funcState.enclosing = current;
    funcState.function = fn;
    funcState.scopeDepth = current->scopeDepth + 1;
    current = &funcState;

    // Slot 0 = the function itself (convention)
    addLocal(stmt->name);

    // Parameters become locals
    for (auto& param : stmt->params) {
        addLocal(param);
    }

    // Compile body
    auto* body = dynamic_cast<const BlockStmt*>(stmt->body.get());
    if (body) {
        for (auto& s : body->statements) compileStmt(s.get());
    }

    // Implicit nil return
    emit(OpCode::OP_NIL, stmt->line);
    emit(OpCode::OP_RETURN, stmt->line);

    current = funcState.enclosing;
    fn->upvalueCount = static_cast<int>(funcState.upvalues.size());

    // Create a prototype closure to store in the constant pool
    auto* proto = new ObjClosure(fn);
    // We need the VM to own this... store via a VMClosureCallable
    auto wrapper = std::make_shared<VMClosureCallable>(proto);
    // Note: this proto leaks unless the VM tracks it. For now, acceptable.

    uint16_t fnIdx = currentChunk().addConstant(
        Value(std::static_pointer_cast<Callable>(wrapper)));

    // Emit OP_CLOSURE
    emit(OpCode::OP_CLOSURE, stmt->line);
    emitU16(fnIdx, stmt->line);

    // Emit upvalue descriptors
    for (auto& uv : funcState.upvalues) {
        emit(uv.isLocal ? 1 : 0, stmt->line);
        emitU16(uv.index, stmt->line);
    }

    // Define in current scope
    if (current->scopeDepth > 0) {
        addLocal(stmt->name);
    } else {
        uint16_t nameIdx = identifierConstant(stmt->name);
        emit(OpCode::OP_DEFINE_GLOBAL, stmt->line);
        emitU16(nameIdx, stmt->line);
    }
}

void Compiler::compileReturnStmt(const ReturnStmt* stmt) {
    if (stmt->value) {
        compileExpr(stmt->value.get());
    } else {
        emit(OpCode::OP_NIL, stmt->line);
    }
    emit(OpCode::OP_RETURN, stmt->line);
}

void Compiler::compileBreakStmt(const BreakStmt* stmt) {
    if (current->loops.empty()) {
        error("'break' outside of loop", stmt->line);
        return;
    }
    // Pop locals in the loop scope
    auto& loop = current->loops.back();
    for (int i = static_cast<int>(current->locals.size()) - 1; i >= 0; i--) {
        if (current->locals[i].depth <= loop.scopeDepthAtLoop) break;
        emit(OpCode::OP_POP, stmt->line);
    }
    loop.breakJumps.push_back(emitJump(OpCode::OP_JUMP, stmt->line));
}

void Compiler::compileContinueStmt(const ContinueStmt* stmt) {
    if (current->loops.empty()) {
        error("'continue' outside of loop", stmt->line);
        return;
    }
    auto& loop = current->loops.back();
    for (int i = static_cast<int>(current->locals.size()) - 1; i >= 0; i--) {
        if (current->locals[i].depth <= loop.scopeDepthAtLoop) break;
        emit(OpCode::OP_POP, stmt->line);
    }
    emitLoop(loop.loopStart, stmt->line);
}

void Compiler::compileClassStmt(const ClassStmt* stmt) {
    uint16_t nameIdx = identifierConstant(stmt->name);

    emit(OpCode::OP_CLASS, stmt->line);
    emitU16(nameIdx, stmt->line);

    // Define the class name immediately so methods can reference it
    if (current->scopeDepth > 0) {
        addLocal(stmt->name);
    } else {
        emit(OpCode::OP_DEFINE_GLOBAL, stmt->line);
        emitU16(identifierConstant(stmt->name), stmt->line);
    }

    // Inheritance
    if (!stmt->superclass.empty()) {
        // Push superclass
        uint16_t superIdx = identifierConstant(stmt->superclass);
        emit(OpCode::OP_GET_GLOBAL, stmt->line);
        emitU16(superIdx, stmt->line);

        // Push the class again for INHERIT
        if (current->scopeDepth > 0) {
            int slot = resolveLocal(current, stmt->name);
            emit(OpCode::OP_GET_LOCAL, stmt->line);
            emitU16(static_cast<uint16_t>(slot), stmt->line);
        } else {
            emit(OpCode::OP_GET_GLOBAL, stmt->line);
            emitU16(identifierConstant(stmt->name), stmt->line);
        }

        emit(OpCode::OP_INHERIT, stmt->line);
    }

    // Push class onto stack for METHOD opcodes
    if (current->scopeDepth > 0) {
        int slot = resolveLocal(current, stmt->name);
        emit(OpCode::OP_GET_LOCAL, stmt->line);
        emitU16(static_cast<uint16_t>(slot), stmt->line);
    } else {
        emit(OpCode::OP_GET_GLOBAL, stmt->line);
        emitU16(identifierConstant(stmt->name), stmt->line);
    }

    // Compile each method
    for (auto& method : stmt->methods) {
        auto fn = std::make_shared<CompiledFunction>();
        fn->name = method.name;
        fn->arity = static_cast<int>(method.params.size());

        CompilerState methodState;
        methodState.enclosing = current;
        methodState.function = fn;
        methodState.scopeDepth = 0;
        current = &methodState;

        // Slot 0 = "this"
        addLocal("this");

        // Parameters
        for (auto& param : method.params) {
            addLocal(param);
        }

        // Body
        for (auto& s : method.body) {
            compileStmt(s.get());
        }

        // Implicit nil return (init returns this)
        if (method.name == "init") {
            emit(OpCode::OP_GET_LOCAL, method.line);
            emitU16(0, method.line); // slot 0 = this
        } else {
            emit(OpCode::OP_NIL, method.line);
        }
        emit(OpCode::OP_RETURN, method.line);

        current = methodState.enclosing;
        fn->upvalueCount = static_cast<int>(methodState.upvalues.size());

        // Create closure for the method
        auto* proto = new ObjClosure(fn);
        auto wrapper = std::make_shared<VMClosureCallable>(proto);
        uint16_t fnIdx = currentChunk().addConstant(
            Value(std::static_pointer_cast<Callable>(wrapper)));

        emit(OpCode::OP_CLOSURE, method.line);
        emitU16(fnIdx, method.line);

        for (auto& uv : methodState.upvalues) {
            emit(uv.isLocal ? 1 : 0, method.line);
            emitU16(uv.index, method.line);
        }

        // Add method to class
        emit(OpCode::OP_METHOD, method.line);
        emitU16(identifierConstant(method.name), method.line);
    }

    // Pop the class from the stack
    emit(OpCode::OP_POP, stmt->line);
}
void Compiler::compileEnumStmt(const EnumStmt* stmt) { error("enum not yet in VM", stmt->line); }
void Compiler::compileThrowStmt(const ThrowStmt* stmt) { error("throw not yet in VM", stmt->line); }
void Compiler::compileTryCatchStmt(const TryCatchStmt* stmt) { error("try/catch not yet in VM", stmt->line); }
void Compiler::compileEnsureStmt(const EnsureStmt* stmt) { error("ensure not yet in VM", stmt->line); }
void Compiler::compileUseStmt(const UseStmt* stmt) { error("use not yet in VM", stmt->line); }
void Compiler::compileExportStmt(const ExportStmt* stmt) { error("export not yet in VM", stmt->line); }
