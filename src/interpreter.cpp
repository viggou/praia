#include "builtins.h"
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
#include <sstream>

namespace fs = std::filesystem;

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

    // 2. grains/ directory (project-level)
    if (!currentFile.empty()) {
        // Walk up from the current file to find a grains/ directory
        fs::path dir = fs::path(currentFile).parent_path();
        for (int i = 0; i < 10; i++) { // limit depth
            auto candidate = dir / "grains" / (path + ".praia");
            if (fs::exists(candidate)) return fs::canonical(candidate).string();
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
    }

    // 3. grains/ relative to cwd
    {
        auto candidate = fs::current_path() / "grains" / (path + ".praia");
        if (fs::exists(candidate)) return fs::canonical(candidate).string();
    }

    // 4. ~/.praia/grains/ (global, for future package manager)
    {
        const char* home = std::getenv("HOME");
        if (home) {
            auto candidate = fs::path(home) / ".praia" / "grains" / (path + ".praia");
            if (fs::exists(candidate)) return fs::canonical(candidate).string();
        }
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

void Interpreter::interpret(const std::vector<StmtPtr>& program) {
    std::lock_guard<std::recursive_mutex> lock(interpMutex);
    try {
        for (const auto& stmt : program)
            execute(stmt.get());
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

void Interpreter::interpretRepl(const std::vector<StmtPtr>& program) {
    std::lock_guard<std::recursive_mutex> lock(interpMutex);
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
        Value val;
        if (s->initializer) val = evaluate(s->initializer.get());
        env->define(s->name, std::move(val));

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

        double from = startVal.asNumber();
        double to   = endVal.asNumber();
        auto* bodyBlock = static_cast<const BlockStmt*>(s->body.get());

        try {
            for (double i = from; i < to; i++) {
                auto iterEnv = std::make_shared<Environment>(env);
                iterEnv->define(s->varName, Value(i));
                try { executeBlock(bodyBlock, iterEnv); }
                catch (const ContinueSignal&) { /* skip to next iteration */ }
            }
        } catch (const BreakSignal&) { /* exit loop */ }

    } else if (auto* s = dynamic_cast<const ForInStmt*>(stmt)) {
        Value iterable = evaluate(s->iterable.get());
        if (!iterable.isArray())
            throw RuntimeError("for-in requires an array", s->line);
        auto* bodyBlock = static_cast<const BlockStmt*>(s->body.get());

        try {
            for (const auto& elem : iterable.asArray()->elements) {
                auto iterEnv = std::make_shared<Environment>(env);
                iterEnv->define(s->varName, elem);
                try { executeBlock(bodyBlock, iterEnv); }
                catch (const ContinueSignal&) { /* skip to next iteration */ }
            }
        } catch (const BreakSignal&) { /* exit loop */ }

    } else if (auto* s = dynamic_cast<const FuncStmt*>(stmt)) {
        auto func = std::make_shared<PraiaFunction>();
        func->funcName = s->name;
        func->params = s->params;
        func->body = static_cast<const BlockStmt*>(s->body.get());
        func->closure = env;
        env->define(s->name, Value(func));

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
        try {
            execute(s->tryBody.get());
        } catch (const ThrowSignal& ts) {
            auto catchEnv = std::make_shared<Environment>(env);
            catchEnv->define(s->errorVar, ts.value);
            executeBlock(static_cast<const BlockStmt*>(s->catchBody.get()), catchEnv);
        } catch (const RuntimeError& re) {
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
        return Value(e->value);

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
            if (!operand.isNumber())
                throw RuntimeError("Operand of '-' must be a number", e->line);
            return Value(-operand.asNumber());
        }
        if (e->op == TokenType::NOT)
            return Value(!operand.isTruthy());
        if (e->op == TokenType::BIT_NOT) {
            if (!operand.isNumber())
                throw RuntimeError("Operand of '~' must be a number", e->line);
            return Value(static_cast<double>(~static_cast<int64_t>(operand.asNumber())));
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

        switch (e->op) {
        case TokenType::PLUS:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() + right.asNumber());
            if (left.isString() || right.isString())
                return Value(left.toString() + right.toString());
            throw RuntimeError("Operands of '+' must be numbers or strings", e->line);
        case TokenType::MINUS:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() - right.asNumber());
            throw RuntimeError("Operands of '-' must be numbers", e->line);
        case TokenType::STAR:
            if (left.isNumber() && right.isNumber())
                return Value(left.asNumber() * right.asNumber());
            throw RuntimeError("Operands of '*' must be numbers", e->line);
        case TokenType::SLASH:
            if (left.isNumber() && right.isNumber()) {
                if (right.asNumber() == 0)
                    throw RuntimeError("Division by zero", e->line);
                return Value(left.asNumber() / right.asNumber());
            }
            throw RuntimeError("Operands of '/' must be numbers", e->line);
        case TokenType::PERCENT:
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
                return Value(static_cast<double>(static_cast<int64_t>(left.asNumber()) & static_cast<int64_t>(right.asNumber())));
            throw RuntimeError("Operands of '&' must be numbers", e->line);
        case TokenType::BIT_OR:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<double>(static_cast<int64_t>(left.asNumber()) | static_cast<int64_t>(right.asNumber())));
            throw RuntimeError("Operands of '|' must be numbers", e->line);
        case TokenType::BIT_XOR:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<double>(static_cast<int64_t>(left.asNumber()) ^ static_cast<int64_t>(right.asNumber())));
            throw RuntimeError("Operands of '^' must be numbers", e->line);
        case TokenType::SHL:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<double>(static_cast<int64_t>(left.asNumber()) << static_cast<int64_t>(right.asNumber())));
            throw RuntimeError("Operands of '<<' must be numbers", e->line);
        case TokenType::SHR:
            if (left.isNumber() && right.isNumber())
                return Value(static_cast<double>(static_cast<int64_t>(left.asNumber()) >> static_cast<int64_t>(right.asNumber())));
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

        return callWithContext(*this, callee.asCallable(), args, e->line);
    }

    // ── Array literal ──

    if (auto* e = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
        auto arr = std::make_shared<PraiaArray>();
        for (const auto& elem : e->elements)
            arr->elements.push_back(evaluate(elem.get()));
        return Value(arr);
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

        auto callable = callee.asCallable();
        int arity = callable->arity();
        if (arity != -1 && static_cast<int>(args.size()) != arity)
            throw RuntimeError(callable->name() + "() expected " + std::to_string(arity) +
                " argument(s) but got " + std::to_string(args.size()), e->line);

        // Spawn the call in a background thread
        // Native functions (http.get, sys.exec, etc.) run without the lock = true parallelism
        // Praia functions need the lock (only one runs at a time, like Python's GIL)
        Interpreter* self = this;
        auto sharedFuture = std::async(std::launch::async,
            [callable, args, self]() -> Value {
                if (dynamic_cast<NativeFunction*>(callable.get())) {
                    return callable->call(*self, args);
                }
                std::lock_guard<std::recursive_mutex> lock(self->interpMutex);
                return callable->call(*self, args);
            }).share();

        auto fut = std::make_shared<PraiaFuture>();
        fut->future = sharedFuture;
        return Value(fut);
    }

    if (auto* e = dynamic_cast<const AwaitExpr*>(expr)) {
        Value val = evaluate(e->expr.get());
        if (!val.isFuture())
            throw RuntimeError("Can only await a future", e->line);

        auto& future = val.asFuture()->future;

        // Release the interpreter lock while waiting so other async tasks can run.
        // RAII guard ensures the mutex is re-locked even if future.get() throws.
        struct MutexRelocker {
            std::recursive_mutex& m;
            ~MutexRelocker() { m.lock(); }
        } relocker{interpMutex};
        interpMutex.unlock();
        Value result = future.get();
        return result;
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
        for (size_t i = 0; i < e->keys.size(); i++)
            map->entries[e->keys[i]] = evaluate(e->values[i].get());
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
