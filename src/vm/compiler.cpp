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
    if (!stmt->pattern.empty()) {
        // Destructuring
        compileExpr(stmt->initializer.get());

        if (stmt->isArrayPattern) {
            // Array destructuring: let [a, b, ...rest] = expr
            // Value is on top of stack. Store it in a temp local.
            addLocal("__destr__");
            int arrSlot = resolveLocal(current, "__destr__");

            for (size_t i = 0; i < stmt->pattern.size(); i++) {
                auto& p = stmt->pattern[i];
                if (p.isRest) {
                    // rest = __arraySlice(arr, i)
                    emit(OpCode::OP_GET_GLOBAL, stmt->line);
                    emitU16(identifierConstant("__arraySlice"), stmt->line);
                    emit(OpCode::OP_GET_LOCAL, stmt->line);
                    emitU16(static_cast<uint16_t>(arrSlot), stmt->line);
                    emitConstant(Value(static_cast<int64_t>(i)), stmt->line);
                    emit(OpCode::OP_CALL, stmt->line);
                    emit(2, stmt->line);
                } else {
                    emit(OpCode::OP_GET_LOCAL, stmt->line);
                    emitU16(static_cast<uint16_t>(arrSlot), stmt->line);
                    emitConstant(Value(static_cast<int64_t>(i)), stmt->line);
                    emit(OpCode::OP_INDEX_GET, stmt->line);
                }
                if (current->scopeDepth > 0) {
                    addLocal(p.name);
                } else {
                    emit(OpCode::OP_DEFINE_GLOBAL, stmt->line);
                    emitU16(identifierConstant(p.name), stmt->line);
                }
            }
            return;
        } else {
            // Map destructuring: let {name, age, ...rest} = expr
            addLocal("__destr__");
            int mapSlot = resolveLocal(current, "__destr__");

            // Collect extracted keys for rest computation
            std::vector<std::string> extractedKeys;
            for (auto& p : stmt->pattern) {
                if (!p.isRest) {
                    std::string key = p.key.empty() ? p.name : p.key;
                    extractedKeys.push_back(key);
                }
            }

            for (auto& p : stmt->pattern) {
                if (p.isRest) {
                    // rest = __mapRest(map, [extracted_keys...])
                    emit(OpCode::OP_GET_GLOBAL, stmt->line);
                    emitU16(identifierConstant("__mapRest"), stmt->line);
                    emit(OpCode::OP_GET_LOCAL, stmt->line);
                    emitU16(static_cast<uint16_t>(mapSlot), stmt->line);
                    // Build array of extracted key names
                    for (auto& k : extractedKeys)
                        emitConstant(Value(k), stmt->line);
                    emit(OpCode::OP_BUILD_ARRAY, stmt->line);
                    emitU16(static_cast<uint16_t>(extractedKeys.size()), stmt->line);
                    emit(OpCode::OP_CALL, stmt->line);
                    emit(2, stmt->line);
                } else {
                    emit(OpCode::OP_GET_LOCAL, stmt->line);
                    emitU16(static_cast<uint16_t>(mapSlot), stmt->line);
                    std::string key = p.key.empty() ? p.name : p.key;
                    emitConstant(Value(key), stmt->line);
                    emit(OpCode::OP_INDEX_GET, stmt->line);
                }
                if (current->scopeDepth > 0) {
                    addLocal(p.name);
                } else {
                    emit(OpCode::OP_DEFINE_GLOBAL, stmt->line);
                    emitU16(identifierConstant(p.name), stmt->line);
                }
            }
            return;
        }
    }

    // Simple let
    if (stmt->initializer) {
        compileExpr(stmt->initializer.get());
    } else {
        emit(OpCode::OP_NIL, stmt->line);
    }

    if (current->scopeDepth > 0) {
        addLocal(stmt->name);
    } else {
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

    current->loops.push_back({loopStart, loopStart, {}, {}, current->scopeDepth});

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
    current->loops.push_back({loopStart, loopStart, {}, {}, current->scopeDepth});

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

    // Continue target: increment section
    current->loops.back().continueTarget = currentChunk().size();
    // Patch any continue jumps to here
    for (int j : current->loops.back().continueJumps) patchJump(j);

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
    // Compiled as:
    // let __iter__ = __iterEntries(<iterable>)   // normalizes maps/strings to arrays
    // let __idx__ = 0
    // while (__idx__ < len(__iter__)) {
    //     let varName = __iter__[__idx__]
    //     <body>
    //     __idx__ = __idx__ + 1
    // }

    beginScope();

    // Normalize iterable via __iterEntries (arrays pass through,
    // maps become [{key,value},...], strings become char arrays)
    emit(OpCode::OP_GET_GLOBAL, stmt->line);
    emitU16(identifierConstant("__iterEntries"), stmt->line);
    compileExpr(stmt->iterable.get());
    emit(OpCode::OP_CALL, stmt->line);
    emit(1, stmt->line);
    addLocal("__iter__");
    int iterSlot = resolveLocal(current, "__iter__");

    // Index counter = 0
    emitConstant(Value(static_cast<int64_t>(0)), stmt->line);
    addLocal("__idx__");
    int idxSlot = resolveLocal(current, "__idx__");

    // Cache len(iterable) — call once, store as local
    emit(OpCode::OP_GET_GLOBAL, stmt->line);
    emitU16(identifierConstant("len"), stmt->line);
    emit(OpCode::OP_GET_LOCAL, stmt->line);
    emitU16(static_cast<uint16_t>(iterSlot), stmt->line);
    emit(OpCode::OP_CALL, stmt->line);
    emit(1, stmt->line);
    addLocal("__len__");
    int lenSlot = resolveLocal(current, "__len__");

    int loopStart = currentChunk().size();
    current->loops.push_back({loopStart, loopStart, {}, {}, current->scopeDepth});

    // Condition: __idx__ < __len__
    emit(OpCode::OP_GET_LOCAL, stmt->line);
    emitU16(static_cast<uint16_t>(idxSlot), stmt->line);
    emit(OpCode::OP_GET_LOCAL, stmt->line);
    emitU16(static_cast<uint16_t>(lenSlot), stmt->line);
    emit(OpCode::OP_LESS, stmt->line);
    int exitJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line);

    // Loop var = iterable[idx]
    beginScope();
    emit(OpCode::OP_GET_LOCAL, stmt->line);
    emitU16(static_cast<uint16_t>(iterSlot), stmt->line);
    emit(OpCode::OP_GET_LOCAL, stmt->line);
    emitU16(static_cast<uint16_t>(idxSlot), stmt->line);
    emit(OpCode::OP_INDEX_GET, stmt->line);
    addLocal(stmt->varName);

    // Body
    compileStmt(stmt->body.get());

    endScope(stmt->line); // pops loop var

    // Continue target
    current->loops.back().continueTarget = currentChunk().size();
    for (int j : current->loops.back().continueJumps) patchJump(j);

    // Increment: __idx__ += 1
    emit(OpCode::OP_GET_LOCAL, stmt->line);
    emitU16(static_cast<uint16_t>(idxSlot), stmt->line);
    emitConstant(Value(static_cast<int64_t>(1)), stmt->line);
    emit(OpCode::OP_ADD, stmt->line);
    emit(OpCode::OP_SET_LOCAL, stmt->line);
    emitU16(static_cast<uint16_t>(idxSlot), stmt->line);
    emit(OpCode::OP_POP, stmt->line);

    emitLoop(loopStart, stmt->line);
    patchJump(exitJump);

    auto& loop = current->loops.back();
    for (int j : loop.breakJumps) patchJump(j);
    current->loops.pop_back();

    endScope(stmt->line); // pops __iter__, __idx__
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

    // Emit default parameter evaluation: if param is nil and default exists, replace it
    for (size_t i = 0; i < stmt->defaults.size(); i++) {
        if (stmt->defaults[i]) {
            int slot = static_cast<int>(i) + 1; // +1 for slot 0 (function itself)
            emit(OpCode::OP_GET_LOCAL, stmt->line);
            emitU16(static_cast<uint16_t>(slot), stmt->line);
            emit(OpCode::OP_NIL, stmt->line);
            emit(OpCode::OP_EQUAL, stmt->line);
            int skipJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line);
            // Replace with default value
            compileExpr(stmt->defaults[i].get());
            emit(OpCode::OP_SET_LOCAL, stmt->line);
            emitU16(static_cast<uint16_t>(slot), stmt->line);
            emit(OpCode::OP_POP, stmt->line);
            patchJump(skipJump);
        }
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
    // Pop locals inside the loop body
    for (int i = static_cast<int>(current->locals.size()) - 1; i >= 0; i--) {
        if (current->locals[i].depth <= loop.scopeDepthAtLoop) break;
        emit(OpCode::OP_POP, stmt->line);
    }
    // Jump forward to the increment section (will be patched later)
    loop.continueJumps.push_back(emitJump(OpCode::OP_JUMP, stmt->line));
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
        // Push superclass (resolve as local first, then global)
        int superSlot = resolveLocal(current, stmt->superclass);
        if (superSlot != -1) {
            emit(OpCode::OP_GET_LOCAL, stmt->line);
            emitU16(static_cast<uint16_t>(superSlot), stmt->line);
        } else {
            int upval = resolveUpvalue(current, stmt->superclass);
            if (upval != -1) {
                emit(OpCode::OP_GET_UPVALUE, stmt->line);
                emit(static_cast<uint8_t>(upval), stmt->line);
            } else {
                emit(OpCode::OP_GET_GLOBAL, stmt->line);
                emitU16(identifierConstant(stmt->superclass), stmt->line);
            }
        }

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

        // Default parameter evaluation: if param is nil and default exists, replace it
        for (size_t i = 0; i < method.defaults.size(); i++) {
            if (method.defaults[i]) {
                int slot = static_cast<int>(i) + 1; // +1 for slot 0 (this)
                emit(OpCode::OP_GET_LOCAL, method.line);
                emitU16(static_cast<uint16_t>(slot), method.line);
                emit(OpCode::OP_NIL, method.line);
                emit(OpCode::OP_EQUAL, method.line);
                int skipJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, method.line);
                compileExpr(method.defaults[i].get());
                emit(OpCode::OP_SET_LOCAL, method.line);
                emitU16(static_cast<uint16_t>(slot), method.line);
                emit(OpCode::OP_POP, method.line);
                patchJump(skipJump);
            }
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
void Compiler::compileEnumStmt(const EnumStmt* stmt) {
    // Compile as a map: {Name1: 0, Name2: 1, ...}
    int64_t nextVal = 0;
    int count = 0;
    for (size_t i = 0; i < stmt->members.size(); i++) {
        emitConstant(Value(stmt->members[i]), stmt->line); // key
        if (stmt->values[i]) {
            // Custom value — compile the expression
            compileExpr(stmt->values[i].get());
            // Try to extract compile-time constant for auto-increment tracking
            auto* numExpr = dynamic_cast<const NumberExpr*>(stmt->values[i].get());
            if (numExpr && numExpr->isInt) {
                nextVal = numExpr->intValue + 1;
            } else {
                nextVal++; // best guess if not a constant
            }
        } else {
            emitConstant(Value(nextVal), stmt->line);
            nextVal++;
        }
        count++;
    }
    emit(OpCode::OP_BUILD_MAP, stmt->line);
    emitU16(static_cast<uint16_t>(count), stmt->line);

    if (current->scopeDepth > 0) {
        addLocal(stmt->name);
    } else {
        emit(OpCode::OP_DEFINE_GLOBAL, stmt->line);
        emitU16(identifierConstant(stmt->name), stmt->line);
    }
}
void Compiler::compileThrowStmt(const ThrowStmt* stmt) {
    compileExpr(stmt->value.get());
    emit(OpCode::OP_THROW, stmt->line);
}

void Compiler::compileTryCatchStmt(const TryCatchStmt* stmt) {
    // OP_TRY_BEGIN [catch_offset]
    // <try body>
    // OP_TRY_END
    // OP_JUMP [end_offset]  -- skip catch if try succeeded
    // catch:
    //   <error value is on stack>
    //   define error variable as local
    //   <catch body>
    // end:

    int tryBegin = emitJump(OpCode::OP_TRY_BEGIN, stmt->line);

    // Try body
    compileStmt(stmt->tryBody.get());

    emit(OpCode::OP_TRY_END, stmt->line);
    int endJump = emitJump(OpCode::OP_JUMP, stmt->line);

    // Patch TRY_BEGIN to jump here on exception
    patchJump(tryBegin);

    // Catch: error value is on top of stack
    beginScope();
    addLocal(stmt->errorVar); // the error value becomes a local

    compileStmt(stmt->catchBody.get());

    endScope(stmt->line);

    patchJump(endJump);
}

void Compiler::compileEnsureStmt(const EnsureStmt* stmt) {
    // ensure (condition) else { body }
    // Compiled as: if (!condition) { body }
    compileExpr(stmt->condition.get());
    int elseJump = emitJump(OpCode::OP_POP_JUMP_IF_FALSE, stmt->line);
    int endJump = emitJump(OpCode::OP_JUMP, stmt->line);
    patchJump(elseJump);

    compileStmt(stmt->elseBody.get());

    patchJump(endJump);
}
void Compiler::compileUseStmt(const UseStmt* stmt) {
    // OP_IMPORT [path_idx] [alias_idx]
    // VM resolves, compiles, and executes the grain, pushes the exports map
    uint16_t pathIdx = identifierConstant(stmt->path);
    uint16_t aliasIdx = identifierConstant(stmt->alias);
    emit(OpCode::OP_IMPORT, stmt->line);
    emitU16(pathIdx, stmt->line);
    emitU16(aliasIdx, stmt->line);

    // The import pushes the exports map — define as a variable
    if (current->scopeDepth > 0) {
        addLocal(stmt->alias);
    } else {
        emit(OpCode::OP_DEFINE_GLOBAL, stmt->line);
        emitU16(identifierConstant(stmt->alias), stmt->line);
    }
}

void Compiler::compileExportStmt(const ExportStmt* stmt) {
    // Build a map from exported names, then signal export
    // Push each name as key and its value
    for (auto& name : stmt->names) {
        emitConstant(Value(name), stmt->line); // key

        // Look up the name
        int slot = resolveLocal(current, name);
        if (slot != -1) {
            emit(OpCode::OP_GET_LOCAL, stmt->line);
            emitU16(static_cast<uint16_t>(slot), stmt->line);
        } else {
            emit(OpCode::OP_GET_GLOBAL, stmt->line);
            emitU16(identifierConstant(name), stmt->line);
        }
    }
    emit(OpCode::OP_BUILD_MAP, stmt->line);
    emitU16(static_cast<uint16_t>(stmt->names.size()), stmt->line);
    emit(OpCode::OP_EXPORT, stmt->line);
    emit(static_cast<uint8_t>(0), stmt->line); // unused count byte
}
