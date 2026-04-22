#include "builtins.h"
#include "grain_resolve.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

// ── Operator overloading helpers ──
// Call a dunder method on an instance if it exists. Returns {true, result} if found.
static std::pair<bool, Value> callDunder(Interpreter& interp,
                                          const std::shared_ptr<PraiaInstance>& inst,
                                          const std::string& methodName,
                                          const std::vector<Value>& args) {
    auto* decl = inst->klass->findMethod(methodName);
    if (!decl) return {false, Value()};
    auto bound = std::make_shared<PraiaMethod>();
    bound->methodName = methodName;
    bound->params = decl->params;
    bound->decl = decl;
    bound->instance = inst;
    auto walk = inst->klass;
    while (walk && !walk->methods.count(methodName))
        walk = walk->superclass;
    bound->definingClass = walk ? walk : inst->klass;
    bound->closure = bound->definingClass->closure;
    return {true, bound->call(interp, args)};
}

// Map binary operator token to dunder method name
static std::string binaryDunder(TokenType op) {
    switch (op) {
        case TokenType::PLUS:    return "__add";
        case TokenType::MINUS:   return "__sub";
        case TokenType::STAR:    return "__mul";
        case TokenType::SLASH:   return "__div";
        case TokenType::PERCENT: return "__mod";
        case TokenType::EQ:      return "__eq";
        case TokenType::NEQ:     return "__eq";
        case TokenType::LT:      return "__lt";
        case TokenType::GT:      return "__gt";
        case TokenType::LTE:     return "__gt"; // !(a > b) → !(a.__gt(b))
        case TokenType::GTE:     return "__lt"; // !(a < b) → !(a.__lt(b))
        default: return "";
    }
}

// Reorder named arguments to match parameter positions.
// Returns a vector in parameter order with Value() for unfilled positions.
static std::vector<Value> reorderNamedArgs(
    const std::shared_ptr<Callable>& callable,
    const std::vector<Value>& args,
    const std::vector<std::string>& names,
    int line) {
    const auto* params = callable->paramNames();
    if (!params)
        throw RuntimeError("Named arguments not supported for '" + callable->name() + "'", line);

    int paramCount = static_cast<int>(params->size());
    std::vector<Value> result(paramCount);
    std::vector<bool> filled(paramCount, false);
    int positionalIdx = 0;

    for (size_t i = 0; i < args.size(); i++) {
        if (names[i].empty()) {
            if (positionalIdx >= paramCount)
                throw RuntimeError(callable->name() + "() too many arguments", line);
            result[positionalIdx] = args[i];
            filled[positionalIdx] = true;
            positionalIdx++;
        } else {
            int found = -1;
            for (int p = 0; p < paramCount; p++) {
                if ((*params)[p] == names[i]) { found = p; break; }
            }
            if (found == -1)
                throw RuntimeError(callable->name() + "() unknown parameter '" + names[i] + "'", line);
            if (filled[found])
                throw RuntimeError(callable->name() + "() parameter '" + names[i] + "' specified twice", line);
            result[found] = args[i];
            filled[found] = true;
        }
    }
    return result;
}

// Invoke a Callable with arity check + line-context for errors.
// Native functions throw with line=0 because they don't know the caller;
// we rewrite that to the current call-site line so the user sees a real location.
static Value callWithContext(Interpreter& interp,
                             const std::shared_ptr<Callable>& func,
                             const std::vector<Value>& args,
                             int line) {
    int n = static_cast<int>(args.size());
    int a = func->arity();
    // Native functions require exact arity; user-defined functions allow
    // fewer args (missing params default to nil, like Python/JS).
    bool isNative = dynamic_cast<NativeFunction*>(func.get()) != nullptr;
    if (a != -1) {
        if (isNative && n != a) {
            throw RuntimeError(func->name() + "() expected " + std::to_string(a) +
                               " argument(s) but got " + std::to_string(n), line);
        } else if (!isNative && n > a) {
            throw RuntimeError(func->name() + "() expected at most " + std::to_string(a) +
                               " argument(s) but got " + std::to_string(n), line);
        }
    }
    interp.callStack.push_back({func->name(), line});
    try {
        Value result = func->call(interp, args);
        interp.callStack.pop_back(); // pop only on success
        return result;
    } catch (const RuntimeError& err) {
        // Leave frame on stack for trace, but fix line 0
        if (err.line == 0)
            throw RuntimeError(err.what(), line);
        throw;
    } catch (...) {
        // Leave frame on stack for trace
        throw;
    }
}

// ── Grain (module) loading ───────────────────────────────────

std::string Interpreter::resolveGrainPath(const std::string& path, int line) {
    // 1. Relative path (starts with ./ or ../)
    if (path.rfind("./", 0) == 0 || path.rfind("../", 0) == 0) {
        std::string base = currentFile.empty() ? fs::current_path().string()
                                                : fs::path(currentFile).parent_path().string();
        std::string resolved = (fs::path(base) / (path + ".praia")).string();
        if (fs::exists(resolved)) return fs::canonical(resolved).string();
        throw RuntimeError("Grain not found: " + path + " (looked in " + resolved + ")", line);
    }

    // 2. ext_grains/ (local dependencies installed by sand)
    if (!currentFile.empty()) {
        fs::path dir = fs::path(currentFile).parent_path();
        for (int i = 0; i < 10; i++) {
            auto r = tryResolveGrain(dir / "ext_grains", path);
            if (!r.empty()) return r;
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
    }
    {
        auto r = tryResolveGrain(fs::current_path() / "ext_grains", path);
        if (!r.empty()) return r;
    }

    // 3. grains/ directory (project-level, bundled grains)
    if (!currentFile.empty()) {
        fs::path dir = fs::path(currentFile).parent_path();
        for (int i = 0; i < 10; i++) {
            auto r = tryResolveGrain(dir / "grains", path);
            if (!r.empty()) return r;
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
    }
    {
        auto r = tryResolveGrain(fs::current_path() / "grains", path);
        if (!r.empty()) return r;
    }

    // 4. ~/.praia/ext_grains/ (user-global)
    {
        const char* home = std::getenv("HOME");
        if (home) {
            auto r = tryResolveGrain(fs::path(home) / ".praia" / "ext_grains", path);
            if (!r.empty()) return r;
        }
    }

    // 5. Bundled stdlib grains + system-global ext_grains
    if (g_praiaLibDir) {
        // Installed: LIBDIR/ext_grains/ (system-global, installed by sudo sand --global)
        auto r = tryResolveGrain(fs::path(g_praiaLibDir) / "ext_grains", path);
        if (!r.empty()) return r;
        // Installed: LIBDIR/grains/ (bundled stdlib)
        r = tryResolveGrain(fs::path(g_praiaLibDir) / "grains", path);
        if (!r.empty()) return r;
    } else if (!g_praiaInstallDir.empty()) {
        // Development layout: <bindir>/grains/
        auto r = tryResolveGrain(fs::path(g_praiaInstallDir) / "grains", path);
        if (!r.empty()) return r;
    }

    throw RuntimeError("Grain not found: " + path, line);
}

Value Interpreter::loadGrain(const std::string& importPath, int line) {
    std::string resolved = resolveGrainPath(importPath, line);

    // Check for duplicate import in the same file
    if (importedInCurrentFile.count(resolved))
        throw RuntimeError("Grain '" + importPath + "' is already imported in this file", line);
    importedInCurrentFile.insert(resolved);

    // Return cached grain if already loaded by another file
    auto cached = grainCache.find(resolved);
    if (cached != grainCache.end()) return cached->second;

    // Read source
    std::ifstream f(resolved);
    if (!f.is_open())
        throw RuntimeError("Cannot read grain: " + resolved, line);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string source = ss.str();

    // Lex + parse
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    if (lexer.hasError())
        throw RuntimeError("Syntax error in grain: " + importPath, line);

    Parser parser(tokens);
    auto program = parser.parse();
    if (parser.hasError())
        throw RuntimeError("Parse error in grain: " + importPath, line);

    // Execute in isolated scope
    auto grainEnv = std::make_shared<Environment>(globals);
    auto prevEnv = env;
    auto prevFile = currentFile;
    auto prevImports = importedInCurrentFile;
    env = grainEnv;
    currentFile = resolved;
    importedInCurrentFile.clear();

    Value exports;
    try {
        for (const auto& stmt : program)
            execute(stmt.get());
        // If no export statement was hit, export nothing
        exports = Value(std::make_shared<PraiaMap>());
    } catch (const ExportSignal& es) {
        exports = Value(es.exports);
    } catch (...) {
        env = prevEnv;
        currentFile = prevFile;
        importedInCurrentFile = prevImports;
        throw;
    }
    env = prevEnv;
    currentFile = prevFile;
    importedInCurrentFile = prevImports;

    // Keep the AST alive and cache the result
    grainAsts.push_back(std::move(program));
    grainCache[resolved] = exports;
    return exports;
}

std::string Interpreter::formatStackTrace() const {
    if (callStack.empty()) return "";
    std::string trace;
    for (int i = static_cast<int>(callStack.size()) - 1; i >= 0; --i) {
        auto& f = callStack[i];
        trace += "  at " + f.name + "()";
        if (f.line > 0) trace += " line " + std::to_string(f.line);
        trace += "\n";
    }
    return trace;
}

bool Interpreter::interpret(const std::vector<StmtPtr>& program) {
    try {
        for (const auto& stmt : program)
            execute(stmt.get());
        return true;
    } catch (const ThrowSignal& t) {
        std::cerr << "[line " << t.line << "] Uncaught error: "
                  << t.value.toString() << std::endl;
        std::cerr << formatStackTrace();
        callStack.clear();
        return false;
    } catch (const RuntimeError& e) {
        std::cerr << "[line " << e.line << "] Runtime error: " << e.what() << std::endl;
        std::cerr << formatStackTrace();
        callStack.clear();
        return false;
    }
}

void Interpreter::interpretRepl(const std::vector<StmtPtr>& program) {
    try {
        for (const auto& stmt : program) {
            if (auto* es = dynamic_cast<const ExprStmt*>(stmt.get())) {
                Value val = evaluate(es->expr.get());
                if (!val.isNil())
                    std::cout << val.toString() << "\n";
            } else {
                execute(stmt.get());
            }
        }
    } catch (const ThrowSignal& t) {
        std::cerr << "[line " << t.line << "] Uncaught error: "
                  << t.value.toString() << std::endl;
        std::cerr << formatStackTrace();
        callStack.clear();
    } catch (const RuntimeError& e) {
        std::cerr << "[line " << e.line << "] Runtime error: " << e.what() << std::endl;
        std::cerr << formatStackTrace();
        callStack.clear();
    }
}

void Interpreter::executeBlock(const BlockStmt* block,
                                std::shared_ptr<Environment> newEnv) {
    auto previous = env;
    env = newEnv;
    try {
        for (const auto& stmt : block->statements)
            execute(stmt.get());
    } catch (...) {
        env = previous;
        throw;
    }
    env = previous;
}

// ── Statement execution ──────────────────────────────────────

void Interpreter::execute(const Stmt* stmt) {
    if (auto* s = dynamic_cast<const ExprStmt*>(stmt)) {
        evaluate(s->expr.get());

    } else if (auto* s = dynamic_cast<const LetStmt*>(stmt)) {
        if (!s->pattern.empty()) {
            // Destructuring
            Value val = evaluate(s->initializer.get());
            if (s->isArrayPattern) {
                if (!val.isArray())
                    throw RuntimeError("Cannot destructure non-array value", s->line);
                auto& elems = val.asArray()->elements;
                for (size_t i = 0; i < s->pattern.size(); i++) {
                    auto& p = s->pattern[i];
                    if (p.isRest) {
                        auto rest = std::make_shared<PraiaArray>();
                        for (size_t j = i; j < elems.size(); j++)
                            rest->elements.push_back(elems[j]);
                        env->define(p.name, Value(rest));
                        break;
                    }
                    env->define(p.name, i < elems.size() ? elems[i] : Value());
                }
            } else {
                // Map destructuring
                if (!val.isMap())
                    throw RuntimeError("Cannot destructure non-map value", s->line);
                auto& entries = val.asMap()->entries;
                std::set<std::string> extracted;
                for (auto& p : s->pattern) {
                    if (p.isRest) {
                        auto rest = std::make_shared<PraiaMap>();
                        for (auto& [k, v] : entries) {
                            if (!extracted.count(k))
                                rest->entries[k] = v;
                        }
                        env->define(p.name, Value(rest));
                        break;
                    }
                    std::string key = p.key.empty() ? p.name : p.key;
                    extracted.insert(key);
                    auto it = entries.find(key);
                    env->define(p.name, it != entries.end() ? it->second : Value());
                }
            }
        } else {
            // Simple let
            Value val;
            if (s->initializer) val = evaluate(s->initializer.get());
            env->define(s->name, std::move(val));
        }

    } else if (auto* s = dynamic_cast<const BlockStmt*>(stmt)) {
        executeBlock(s, std::make_shared<Environment>(env));

    } else if (auto* s = dynamic_cast<const IfStmt*>(stmt)) {
        if (evaluate(s->condition.get()).isTruthy()) {
            execute(s->thenBranch.get());
        } else {
            bool handled = false;
            for (const auto& elif : s->elifBranches) {
                if (evaluate(elif.condition.get()).isTruthy()) {
                    execute(elif.body.get());
                    handled = true;
                    break;
                }
            }
            if (!handled && s->elseBranch)
                execute(s->elseBranch.get());
        }

    } else if (auto* s = dynamic_cast<const WhileStmt*>(stmt)) {
        try {
            while (evaluate(s->condition.get()).isTruthy()) {
                try { execute(s->body.get()); }
                catch (const ContinueSignal&) { /* skip to next iteration */ }
            }
        } catch (const BreakSignal&) { /* exit loop */ }

    } else if (auto* s = dynamic_cast<const ForStmt*>(stmt)) {
        Value startVal = evaluate(s->start.get());
        Value endVal   = evaluate(s->end.get());
        if (!startVal.isNumber() || !endVal.isNumber())
            throw RuntimeError("Range bounds must be numbers", s->line);

        int64_t from = static_cast<int64_t>(startVal.asNumber());
        int64_t to   = static_cast<int64_t>(endVal.asNumber());
        auto* bodyBlock = static_cast<const BlockStmt*>(s->body.get());

        try {
            for (int64_t i = from; i < to; i++) {
                auto iterEnv = std::make_shared<Environment>(env);
                iterEnv->define(s->varName, Value(i));
                try { executeBlock(bodyBlock, iterEnv); }
                catch (const ContinueSignal&) { /* skip to next iteration */ }
            }
        } catch (const BreakSignal&) { /* exit loop */ }

    } else if (auto* s = dynamic_cast<const ForInStmt*>(stmt)) {
        Value iterable = evaluate(s->iterable.get());
        auto* bodyBlock = static_cast<const BlockStmt*>(s->body.get());
        bool hasDestructure = !s->destructureKeys.empty();

        // Helper: define loop variables in the iteration env
        auto defineLoopVar = [&](std::shared_ptr<Environment> iterEnv, const Value& elem) {
            if (hasDestructure && elem.isMap()) {
                auto& entries = elem.asMap()->entries;
                for (auto& dk : s->destructureKeys) {
                    auto it = entries.find(dk);
                    iterEnv->define(dk, it != entries.end() ? it->second : Value());
                }
            } else {
                iterEnv->define(s->varName, elem);
            }
        };

        if (iterable.isArray()) {
            try {
                for (const auto& elem : iterable.asArray()->elements) {
                    auto iterEnv = std::make_shared<Environment>(env);
                    defineLoopVar(iterEnv, elem);
                    try { executeBlock(bodyBlock, iterEnv); }
                    catch (const ContinueSignal&) {}
                }
            } catch (const BreakSignal&) {}
        } else if (iterable.isMap()) {
            try {
                for (auto& [k, v] : iterable.asMap()->entries) {
                    auto entry = std::make_shared<PraiaMap>();
                    entry->entries["key"] = Value(k);
                    entry->entries["value"] = v;
                    auto iterEnv = std::make_shared<Environment>(env);
                    defineLoopVar(iterEnv, Value(entry));
                    try { executeBlock(bodyBlock, iterEnv); }
                    catch (const ContinueSignal&) {}
                }
            } catch (const BreakSignal&) {}
        } else if (iterable.isString()) {
            try {
                for (size_t i = 0; i < iterable.asString().size(); i++) {
                    auto iterEnv = std::make_shared<Environment>(env);
                    defineLoopVar(iterEnv, Value(std::string(1, iterable.asString()[i])));
                    try { executeBlock(bodyBlock, iterEnv); }
                    catch (const ContinueSignal&) {}
                }
            } catch (const BreakSignal&) {}
        } else {
            throw RuntimeError("for-in requires an array, map, or string", s->line);
        }

    } else if (auto* s = dynamic_cast<const FuncStmt*>(stmt)) {
        auto func = std::make_shared<PraiaFunction>();
        func->funcName = s->name;
        func->params = s->params;
        func->defaults = &s->defaults;
        func->body = static_cast<const BlockStmt*>(s->body.get());
        func->closure = env;
        env->define(s->name, Value(func));

    } else if (auto* s = dynamic_cast<const EnumStmt*>(stmt)) {
        auto enumMap = std::make_shared<PraiaMap>();
        int64_t nextVal = 0;
        for (size_t i = 0; i < s->members.size(); i++) {
            if (s->values[i]) {
                Value v = evaluate(s->values[i].get());
                if (!v.isNumber())
                    throw RuntimeError("Enum value must be a number", s->line);
                nextVal = v.isInt() ? v.asInt() : static_cast<int64_t>(v.asNumber());
            }
            enumMap->entries[s->members[i]] = Value(nextVal);
            nextVal++;
        }
        env->define(s->name, Value(enumMap));

    } else if (auto* s = dynamic_cast<const ClassStmt*>(stmt)) {
        std::shared_ptr<PraiaClass> superclass;
        if (!s->superclass.empty()) {
            Value superVal = env->get(s->superclass, s->line);
            if (!superVal.isCallable())
                throw RuntimeError("Superclass must be a class", s->line);
            superclass = std::dynamic_pointer_cast<PraiaClass>(superVal.asCallable());
            if (!superclass)
                throw RuntimeError("'" + s->superclass + "' is not a class", s->line);
        }

        auto klass = std::make_shared<PraiaClass>();
        klass->className = s->name;
        klass->superclass = superclass;
        klass->closure = env;
        for (auto& m : s->methods)
            klass->methods[m.name] = &m;
        env->define(s->name, Value(std::static_pointer_cast<Callable>(klass)));

    } else if (auto* s = dynamic_cast<const ReturnStmt*>(stmt)) {
        Value val;
        if (s->value) val = evaluate(s->value.get());
        throw ReturnSignal{std::move(val)};

    } else if (dynamic_cast<const BreakStmt*>(stmt)) {
        throw BreakSignal{};

    } else if (dynamic_cast<const ContinueStmt*>(stmt)) {
        throw ContinueSignal{};

    } else if (auto* s = dynamic_cast<const ThrowStmt*>(stmt)) {
        Value val = evaluate(s->value.get());
        throw ThrowSignal{std::move(val), s->line};

    } else if (auto* s = dynamic_cast<const TryCatchStmt*>(stmt)) {
        size_t savedStackSize = callStack.size();
        try {
            execute(s->tryBody.get());
        } catch (const ThrowSignal& ts) {
            callStack.resize(savedStackSize);
            auto catchEnv = std::make_shared<Environment>(env);
            catchEnv->define(s->errorVar, ts.value);
            executeBlock(static_cast<const BlockStmt*>(s->catchBody.get()), catchEnv);
        } catch (const RuntimeError& re) {
            callStack.resize(savedStackSize);
            auto catchEnv = std::make_shared<Environment>(env);
            catchEnv->define(s->errorVar, Value(std::string(re.what())));
            executeBlock(static_cast<const BlockStmt*>(s->catchBody.get()), catchEnv);
        }

    } else if (auto* s = dynamic_cast<const EnsureStmt*>(stmt)) {
        Value cond = evaluate(s->condition.get());
        if (!cond.isTruthy())
            execute(s->elseBody.get());

    } else if (auto* s = dynamic_cast<const UseStmt*>(stmt)) {
        Value grain = loadGrain(s->path, s->line);
        env->define(s->alias, grain);

    } else if (auto* s = dynamic_cast<const ExportStmt*>(stmt)) {
        auto exports = std::make_shared<PraiaMap>();
        for (auto& name : s->names) {
            exports->entries[name] = env->get(name, s->line);
        }
        throw ExportSignal{exports};
    }
}

// ── Expression evaluation ────────────────────────────────────

Value Interpreter::evaluate(const Expr* expr) {
    // ── Literals ──

    if (auto* e = dynamic_cast<const NumberExpr*>(expr))
        return e->isInt ? Value(e->intValue) : Value(e->floatValue);

    if (auto* e = dynamic_cast<const StringExpr*>(expr))
        return Value(e->value);

    if (auto* e = dynamic_cast<const BoolExpr*>(expr))
        return Value(e->value);

    if (dynamic_cast<const NilExpr*>(expr))
        return Value();

    // ── Variables ──

    if (auto* e = dynamic_cast<const IdentifierExpr*>(expr))
        return env->get(e->name, e->line);

    if (auto* e = dynamic_cast<const ThisExpr*>(expr))
        return env->get("this", e->line);

    if (auto* e = dynamic_cast<const SuperExpr*>(expr)) {
        // Get the instance
        Value thisVal = env->get("this", e->line);
        if (!thisVal.isInstance())
            throw RuntimeError("'super' used outside of a method", e->line);
        auto instance = thisVal.asInstance();

        // Get the superclass from the defining class (not the instance's class)
        Value superVal = env->get("__super__", e->line);
        if (!superVal.isCallable())
            throw RuntimeError("'super' used in a class with no superclass", e->line);
        auto super = std::dynamic_pointer_cast<PraiaClass>(superVal.asCallable());
        if (!super)
            throw RuntimeError("Class has no superclass", e->line);

        // Look up the method on the superclass
        auto* methodDecl = super->findMethod(e->method);
        if (!methodDecl)
            throw RuntimeError("Superclass has no method '" + e->method + "'", e->line);

        // Bind it to the current instance, with the super's class as defining class
        auto bound = std::make_shared<PraiaMethod>();
        bound->methodName = e->method;
        bound->params = methodDecl->params;
        bound->decl = methodDecl;
        bound->closure = super->closure;
        bound->instance = instance;
        // Find which class actually defines this method (for correct super chaining)
        bound->definingClass = super;
        auto* check = super->methods.count(e->method) ? super.get() : nullptr;
        if (!check && super->superclass) {
            // The method is inherited further up — find the actual defining class
            auto walk = super;
            while (walk && !walk->methods.count(e->method))
                walk = walk->superclass;
            if (walk) bound->definingClass = walk;
        }
        return Value(std::static_pointer_cast<Callable>(bound));
    }

    if (auto* e = dynamic_cast<const AssignExpr*>(expr)) {
        Value val = evaluate(e->value.get());
        env->set(e->name, val, e->line);
        return val;
    }

    // ── Unary ──

    if (auto* e = dynamic_cast<const UnaryExpr*>(expr)) {
        Value operand = evaluate(e->operand.get());
        if (e->op == TokenType::MINUS) {
            if (operand.isInstance()) {
                auto [found, result] = callDunder(*this, operand.asInstance(), "__neg", {});
                if (found) return result;
            }
            if (!operand.isNumber())
                throw RuntimeError("Operand of '-' must be a number", e->line);
            if (operand.isInt()) return Value(-operand.asInt());
            return Value(-operand.asNumber());
        }
        if (e->op == TokenType::NOT)
            return Value(!operand.isTruthy());
        if (e->op == TokenType::BIT_NOT) {
            if (!operand.isNumber())
                throw RuntimeError("Operand of '~' must be a number", e->line);
            return Value(~static_cast<int64_t>(operand.asNumber()));
        }
        throw RuntimeError("Unknown unary operator", e->line);
    }

    // ── Postfix (i++, i--) ──

    if (auto* e = dynamic_cast<const PostfixExpr*>(expr)) {
        auto* ident = dynamic_cast<const IdentifierExpr*>(e->operand.get());
        if (!ident)
            throw RuntimeError("Postfix operator requires a variable", e->line);

        Value cur = env->get(ident->name, e->line);
        if (!cur.isNumber())
            throw RuntimeError("Postfix operator requires a number", e->line);

        if (cur.isInt()) {
            int64_t old = cur.asInt();
            int64_t next = (e->op == TokenType::INCREMENT) ? old + 1 : old - 1;
            env->set(ident->name, Value(next), e->line);
            return Value(old);
        }
        double old = cur.asNumber();
        double next = (e->op == TokenType::INCREMENT) ? old + 1 : old - 1;
        env->set(ident->name, Value(next), e->line);
        return Value(old);
    }

    // ── Binary ──

    if (auto* e = dynamic_cast<const BinaryExpr*>(expr)) {
        if (e->op == TokenType::OR) {
            Value left = evaluate(e->left.get());
            return left.isTruthy() ? left : evaluate(e->right.get());
        }
        if (e->op == TokenType::AND) {
            Value left = evaluate(e->left.get());
            return !left.isTruthy() ? left : evaluate(e->right.get());
        }

        Value left  = evaluate(e->left.get());
        Value right = evaluate(e->right.get());

        // Operator overloading: check for dunder methods on instances
        if (left.isInstance()) {
            std::string dunder = binaryDunder(e->op);
            if (!dunder.empty()) {
                auto [found, result] = callDunder(*this, left.asInstance(), dunder, {right});
                if (found) {
                    // NEQ, LTE, GTE negate the result of __eq, __gt, __lt
                    if (e->op == TokenType::NEQ || e->op == TokenType::LTE || e->op == TokenType::GTE)
                        return Value(!result.isTruthy());
                    return result;
                }
            }
        }

        switch (e->op) {
        case TokenType::PLUS:
            if (left.isInt() && right.isInt())
                return Value(left.asInt() + right.asInt());
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() + right.asNumber());
            if (left.isArray() && right.isArray()) {
                auto result = std::make_shared<PraiaArray>();
                for (auto& el : left.asArray()->elements) result->elements.push_back(el);
                for (auto& el : right.asArray()->elements) result->elements.push_back(el);
                return Value(result);
            }
            if (left.isMap() && right.isMap()) {
                auto result = std::make_shared<PraiaMap>();
                for (auto& [k, v] : left.asMap()->entries) result->entries[k] = v;
                for (auto& [k, v] : right.asMap()->entries) result->entries[k] = v;
                return Value(result);
            }
            if (left.isString() || right.isString())
                return Value(left.toString() + right.toString());
            throw RuntimeError("Operands of '+' must be numbers, strings, or arrays", e->line);
        case TokenType::MINUS:
            if (left.isInt() && right.isInt())
                return Value(left.asInt() - right.asInt());
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() - right.asNumber());
            throw RuntimeError("Operands of '-' must be numbers", e->line);
        case TokenType::STAR:
            if (left.isInt() && right.isInt())
                return Value(left.asInt() * right.asInt());
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() * right.asNumber());
            throw RuntimeError("Operands of '*' must be numbers", e->line);
        case TokenType::SLASH:
            // Division always returns double (like Python 3)
            if (left.isNumber() && right.isNumber()) {
                if (right.asNumber() == 0)
                    throw RuntimeError("Division by zero", e->line);
                return Value(left.asNumber() / right.asNumber());
            }
            throw RuntimeError("Operands of '/' must be numbers", e->line);
        case TokenType::PERCENT:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() == 0)
                    throw RuntimeError("Modulo by zero", e->line);
                return Value(left.asInt() % right.asInt());
            }
            if (left.isNumber() && right.isNumber()) {
                if (right.asNumber() == 0)
                    throw RuntimeError("Modulo by zero", e->line);
                return Value(std::fmod(left.asNumber(), right.asNumber()));
            }
            throw RuntimeError("Operands of '%' must be numbers", e->line);
        case TokenType::LT:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() < right.asNumber());
            throw RuntimeError("Operands of '<' must be numbers", e->line);
        case TokenType::GT:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() > right.asNumber());
            throw RuntimeError("Operands of '>' must be numbers", e->line);
        case TokenType::LTE:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() <= right.asNumber());
            throw RuntimeError("Operands of '<=' must be numbers", e->line);
        case TokenType::GTE:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() >= right.asNumber());
            throw RuntimeError("Operands of '>=' must be numbers", e->line);
        case TokenType::EQ:  return Value(left == right);
        case TokenType::NEQ: return Value(left != right);

        case TokenType::BIT_AND:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<int64_t>(left.asNumber()) & static_cast<int64_t>(right.asNumber()));
            throw RuntimeError("Operands of '&' must be numbers", e->line);
        case TokenType::BIT_OR:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<int64_t>(left.asNumber()) | static_cast<int64_t>(right.asNumber()));
            throw RuntimeError("Operands of '|' must be numbers", e->line);
        case TokenType::BIT_XOR:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<int64_t>(left.asNumber()) ^ static_cast<int64_t>(right.asNumber()));
            throw RuntimeError("Operands of '^' must be numbers", e->line);
        case TokenType::SHL:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<int64_t>(left.asNumber()) << static_cast<int64_t>(right.asNumber()));
            throw RuntimeError("Operands of '<<' must be numbers", e->line);
        case TokenType::SHR:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<int64_t>(left.asNumber()) >> static_cast<int64_t>(right.asNumber()));
            throw RuntimeError("Operands of '>>' must be numbers", e->line);

        default:
            throw RuntimeError("Unknown binary operator", e->line);
        }
    }

    // ── Function call ──

    if (auto* e = dynamic_cast<const CallExpr*>(expr)) {
        Value callee = evaluate(e->callee.get());
        if (!callee.isCallable())
            throw RuntimeError("Can only call functions", e->line);

        std::vector<Value> args;
        for (const auto& arg : e->args)
            args.push_back(evaluate(arg.get()));

        // Reorder named arguments if present
        bool hasNamed = false;
        for (auto& n : e->argNames) { if (!n.empty()) { hasNamed = true; break; } }
        if (hasNamed)
            args = reorderNamedArgs(callee.asCallable(), args, e->argNames, e->line);

        return callWithContext(*this, callee.asCallable(), args, e->line);
    }

    // ── Array literal ──

    if (auto* e = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
        auto arr = std::make_shared<PraiaArray>();
        for (const auto& elem : e->elements) {
            if (auto* spread = dynamic_cast<const SpreadExpr*>(elem.get())) {
                Value val = evaluate(spread->expr.get());
                if (!val.isArray())
                    throw RuntimeError("Spread requires an array", spread->line);
                for (auto& item : val.asArray()->elements)
                    arr->elements.push_back(item);
            } else {
                arr->elements.push_back(evaluate(elem.get()));
            }
        }
        return Value(arr);
    }

    // ── Ternary ──

    if (auto* e = dynamic_cast<const TernaryExpr*>(expr)) {
        Value cond = evaluate(e->condition.get());
        return cond.isTruthy() ? evaluate(e->thenExpr.get()) : evaluate(e->elseExpr.get());
    }

    // ── Pipe ──

    if (auto* e = dynamic_cast<const PipeExpr*>(expr)) {
        Value leftVal = evaluate(e->left.get());

        // If right side is a call: f(x, y) → f(leftVal, x, y)
        if (auto* call = dynamic_cast<const CallExpr*>(e->right.get())) {
            Value callee = evaluate(call->callee.get());
            if (!callee.isCallable())
                throw RuntimeError("Pipe target must be a function", e->line);

            std::vector<Value> args;
            args.push_back(leftVal);
            for (const auto& arg : call->args)
                args.push_back(evaluate(arg.get()));

            return callWithContext(*this, callee.asCallable(), args, e->line);
        }

        // Right side is just a function name: f → f(leftVal)
        Value callee = evaluate(e->right.get());
        if (!callee.isCallable())
            throw RuntimeError("Pipe target must be a function", e->line);

        return callWithContext(*this, callee.asCallable(), {leftVal}, e->line);
    }

    // ── Async / Await ──

    if (auto* e = dynamic_cast<const AsyncExpr*>(expr)) {
        // The inner expression should be a function call
        auto* call = dynamic_cast<const CallExpr*>(e->expr.get());
        if (!call)
            throw RuntimeError("async requires a function call", e->line);

        // Evaluate callee and args on the current thread
        Value callee = evaluate(call->callee.get());
        if (!callee.isCallable())
            throw RuntimeError("Can only call functions", e->line);

        std::vector<Value> args;
        for (const auto& arg : call->args)
            args.push_back(evaluate(arg.get()));

        // Reorder named arguments if present
        bool hasNamed = false;
        for (auto& n : call->argNames) { if (!n.empty()) { hasNamed = true; break; } }
        if (hasNamed)
            args = reorderNamedArgs(callee.asCallable(), args, call->argNames, e->line);

        auto callable = callee.asCallable();
        int arity = callable->arity();
        bool isNative = dynamic_cast<NativeFunction*>(callable.get()) != nullptr;
        if (isNative) {
            // Natives have no defaults — require exact arity
            if (arity != -1 && static_cast<int>(args.size()) != arity)
                throw RuntimeError(callable->name() + "() expected " + std::to_string(arity) +
                    " argument(s) but got " + std::to_string(args.size()), e->line);
        } else {
            // Praia functions: reject too many args, fewer is fine (defaults/nil fill the rest)
            if (arity != -1 && static_cast<int>(args.size()) > arity)
                throw RuntimeError(callable->name() + "() expected at most " + std::to_string(arity) +
                    " argument(s) but got " + std::to_string(args.size()), e->line);
        }

        // Spawn the call in a background thread.
        // Native functions run directly — they don't touch interpreter state.
        // Praia functions get a task-local Interpreter with shared globals
        // so PraiaFunction::call's env swaps don't corrupt foreground state.
        Interpreter* self = this;
        auto sharedGlobals = globals;
        auto sharedFuture = std::async(std::launch::async,
            [callable, args, self, sharedGlobals]() -> Value {
                if (dynamic_cast<NativeFunction*>(callable.get())) {
                    return callable->call(*self, args);
                }
                Interpreter taskInterp(sharedGlobals);
                return callable->call(taskInterp, args);
            }).share();

        auto fut = std::make_shared<PraiaFuture>();
        fut->future = sharedFuture;
        return Value(fut);
    }

    if (auto* e = dynamic_cast<const AwaitExpr*>(expr)) {
        Value val = evaluate(e->expr.get());
        if (!val.isFuture())
            throw RuntimeError("Can only await a future", e->line);

        try {
            return val.asFuture()->future.get();
        } catch (const RuntimeError&) {
            throw;
        } catch (const std::exception& ex) {
            throw RuntimeError(std::string("Async task failed: ") + ex.what(), e->line);
        }
    }

    // ── Lambda ──

    if (auto* e = dynamic_cast<const LambdaExpr*>(expr)) {
        auto lam = std::make_shared<PraiaLambda>();
        lam->params = e->params;
        lam->expr = e;
        lam->closure = env;
        return Value(std::static_pointer_cast<Callable>(lam));
    }

    // ── Map literal ──

    if (auto* e = dynamic_cast<const MapLiteralExpr*>(expr)) {
        auto map = std::make_shared<PraiaMap>();
        for (size_t i = 0; i < e->keys.size(); i++) {
            if (e->keys[i].empty() && dynamic_cast<const SpreadExpr*>(e->values[i].get())) {
                auto* spread = dynamic_cast<const SpreadExpr*>(e->values[i].get());
                Value val = evaluate(spread->expr.get());
                if (!val.isMap())
                    throw RuntimeError("Spread in map requires a map", spread->line);
                for (auto& [k, v] : val.asMap()->entries)
                    map->entries[k] = v;
            } else {
                map->entries[e->keys[i]] = evaluate(e->values[i].get());
            }
        }
        return Value(map);
    }

    // ── Index access ──

    if (auto* e = dynamic_cast<const IndexExpr*>(expr)) {
        Value obj = evaluate(e->object.get());
        Value idx = evaluate(e->index.get());
        if (obj.isArray()) {
            if (!idx.isNumber())
                throw RuntimeError("Array index must be a number", e->line);
            auto& elems = obj.asArray()->elements;
            int i = static_cast<int>(idx.asNumber());
            if (i < 0) i += static_cast<int>(elems.size());
            if (i < 0 || i >= static_cast<int>(elems.size()))
                throw RuntimeError("Array index out of bounds", e->line);
            return elems[i];
        }
        if (obj.isString()) {
            if (!idx.isNumber())
                throw RuntimeError("String index must be a number", e->line);
            auto& str = obj.asString();
            int i = static_cast<int>(idx.asNumber());
            if (i < 0) i += static_cast<int>(str.size());
            if (i < 0 || i >= static_cast<int>(str.size()))
                throw RuntimeError("String index out of bounds", e->line);
            return Value(std::string(1, str[i]));
        }
        if (obj.isMap()) {
            if (!idx.isString())
                throw RuntimeError("Map key must be a string", e->line);
            auto& entries = obj.asMap()->entries;
            auto it = entries.find(idx.asString());
            if (it == entries.end())
                throw RuntimeError("Map has no key '" + idx.asString() + "'", e->line);
            return it->second;
        }
        if (obj.isInstance()) {
            auto [found, result] = callDunder(*this, obj.asInstance(), "__index", {idx});
            if (found) return result;
        }
        throw RuntimeError("Can only index into arrays, strings, and maps", e->line);
    }

    // ── Index assignment ──

    if (auto* e = dynamic_cast<const IndexAssignExpr*>(expr)) {
        Value obj = evaluate(e->object.get());
        Value idx = evaluate(e->index.get());
        Value val = evaluate(e->value.get());
        if (obj.isArray()) {
            if (!idx.isNumber())
                throw RuntimeError("Array index must be a number", e->line);
            auto& elems = obj.asArray()->elements;
            int i = static_cast<int>(idx.asNumber());
            if (i < 0) i += static_cast<int>(elems.size());
            if (i < 0 || i >= static_cast<int>(elems.size()))
                throw RuntimeError("Array index out of bounds", e->line);
            elems[i] = val;
            return val;
        }
        if (obj.isMap()) {
            if (!idx.isString())
                throw RuntimeError("Map key must be a string", e->line);
            obj.asMap()->entries[idx.asString()] = val;
            return val;
        }
        if (obj.isInstance()) {
            auto [found, result] = callDunder(*this, obj.asInstance(), "__indexSet", {idx, val});
            if (found) return result;
        }
        if (obj.isString())
            throw RuntimeError("Strings are immutable — cannot assign to index", e->line);
        throw RuntimeError("Can only assign to array or map indices", e->line);
    }

    // ── Dot access ──

    if (auto* e = dynamic_cast<const DotExpr*>(expr)) {
        Value obj = evaluate(e->object.get());

        // Universal methods — work on any type
        if (e->field == "toString") {
            Value captured = obj;
            return Value(makeNative("toString", 0,
                [captured](const std::vector<Value>&) -> Value {
                    return Value(captured.toString());
                }));
        }
        if (e->field == "toNum") {
            Value captured = obj;
            return Value(makeNative("toNum", 0,
                [captured](const std::vector<Value>&) -> Value {
                    if (captured.isNumber()) return captured;
                    if (captured.isBool()) return Value(captured.asBool() ? 1.0 : 0.0);
                    if (captured.isString()) {
                        auto& s = captured.asString();
                        // Case-insensitive bool strings
                        std::string lower = s;
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                        if (lower == "true") return Value(1.0);
                        if (lower == "false") return Value(0.0);
                        // Try numeric parse
                        try {
                            size_t pos = 0;
                            double d = std::stod(s, &pos);
                            if (pos == s.size()) return Value(d);
                        } catch (...) {}
                        throw RuntimeError("toNum: cannot parse \"" + s + "\" as a number", 0);
                    }
                    throw RuntimeError("toNum: cannot convert " + captured.toString() +
                                       " to a number", 0);
                }));
        }

        if (obj.isInstance()) {
            auto inst = obj.asInstance();
            // Fields first
            auto fit = inst->fields.find(e->field);
            if (fit != inst->fields.end()) return fit->second;
            // Then methods
            auto* methodDecl = inst->klass->findMethod(e->field);
            if (methodDecl) {
                auto bound = std::make_shared<PraiaMethod>();
                bound->methodName = e->field;
                bound->params = methodDecl->params;
                bound->decl = methodDecl;
                bound->instance = inst;
                // Find which class in the hierarchy defines this method
                auto walk = inst->klass;
                while (walk && !walk->methods.count(e->field))
                    walk = walk->superclass;
                bound->definingClass = walk ? walk : inst->klass;
                bound->closure = bound->definingClass->closure;
                return Value(std::static_pointer_cast<Callable>(bound));
            }
            throw RuntimeError("Instance has no property '" + e->field + "'", e->line);
        }
        if (obj.isMap()) {
            auto& entries = obj.asMap()->entries;
            auto it = entries.find(e->field);
            if (it != entries.end()) return it->second;
            throw RuntimeError("Map has no field '" + e->field + "'", e->line);
        }
        if (obj.isString())
            return getStringMethod(obj.asString(), e->field, e->line);
        if (obj.isArray())
            return getArrayMethod(obj.asArray(), e->field, e->line, this);

        throw RuntimeError("Cannot access field '" + e->field + "' on this type", e->line);
    }

    // ── Dot assignment ──

    if (auto* e = dynamic_cast<const DotAssignExpr*>(expr)) {
        Value obj = evaluate(e->object.get());
        Value val = evaluate(e->value.get());
        if (obj.isInstance()) {
            obj.asInstance()->fields[e->field] = val;
            return val;
        }
        if (obj.isMap()) {
            obj.asMap()->entries[e->field] = val;
            return val;
        }
        throw RuntimeError("Can only set fields on instances and maps", e->line);
    }

    // ── String interpolation ──

    if (auto* e = dynamic_cast<const InterpolatedStringExpr*>(expr)) {
        std::string result;
        for (const auto& part : e->parts)
            result += evaluate(part.get()).toString();
        return Value(std::move(result));
    }

    throw RuntimeError("Unknown expression type", expr->line);
}
