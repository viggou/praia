#include "interpreter.h"

// ── PraiaClass / PraiaMethod ──────────────────────────────────

const ClassMethod* PraiaClass::findMethod(const std::string& name) const {
    auto it = methods.find(name);
    if (it != methods.end()) return it->second;
    if (superclass) return superclass->findMethod(name);
    return nullptr;
}

int PraiaClass::arity() const {
    auto* init = findMethod("init");
    return init ? static_cast<int>(init->params.size()) : 0;
}

Value PraiaClass::call(Interpreter& interp, const std::vector<Value>& args) {
    auto instance = std::make_shared<PraiaInstance>();
    instance->klass = shared_from_this();

    auto* init = findMethod("init");
    if (init) {
        // Bind and call the init method
        auto method = std::make_shared<PraiaMethod>();
        method->methodName = "init";
        method->params = init->params;
        method->decl = init;
        method->closure = closure;
        method->instance = instance;
        // Find which class in the hierarchy defines init
        auto self = shared_from_this();
        while (self && !self->methods.count("init"))
            self = self->superclass;
        method->definingClass = self ? self : shared_from_this();
        method->call(interp, args);
    }

    return Value(instance);
}

Value PraiaMethod::call(Interpreter& interp, const std::vector<Value>& args) {
    auto methodEnv = std::make_shared<Environment>(closure);
    methodEnv->define("this", Value(instance));

    // Store the defining class so super resolves correctly in multi-level inheritance
    if (definingClass && definingClass->superclass) {
        methodEnv->define("__super__", Value(
            std::static_pointer_cast<Callable>(definingClass->superclass)));
    }

    auto prevEnv = interp.env;
    interp.env = methodEnv;
    for (size_t i = 0; i < params.size(); i++) {
        if (i < args.size() && !args[i].isNil()) {
            methodEnv->define(params[i], args[i]);
        } else if (decl && i < decl->defaults.size() && decl->defaults[i]) {
            methodEnv->define(params[i], interp.evaluate(decl->defaults[i].get()));
        } else if (i < args.size()) {
            methodEnv->define(params[i], args[i]);
        } else {
            methodEnv->define(params[i], Value());
        }
    }
    try {
        for (const auto& stmt : decl->body)
            interp.execute(stmt.get());
    } catch (const ReturnSignal& ret) {
        interp.env = prevEnv;
        return ret.value;
    } catch (...) {
        interp.env = prevEnv;
        throw;
    }
    interp.env = prevEnv;
    return Value();
}

// ── PraiaFunction / PraiaLambda ───────────────────────────────

Value PraiaLambda::call(Interpreter& interp, const std::vector<Value>& args) {
    auto lambdaEnv = std::make_shared<Environment>(closure);

    auto prevEnv = interp.env;
    interp.env = lambdaEnv;
    for (size_t i = 0; i < params.size(); i++) {
        if (i < args.size() && !args[i].isNil()) {
            lambdaEnv->define(params[i], args[i]);
        } else if (i < expr->defaults.size() && expr->defaults[i]) {
            lambdaEnv->define(params[i], interp.evaluate(expr->defaults[i].get()));
        } else if (i < args.size()) {
            lambdaEnv->define(params[i], args[i]);
        } else {
            lambdaEnv->define(params[i], Value());
        }
    }

    try {
        for (const auto& stmt : expr->body)
            interp.execute(stmt.get());
    } catch (const ReturnSignal& ret) {
        interp.env = prevEnv;
        return ret.value;
    } catch (...) {
        interp.env = prevEnv;
        throw;
    }
    interp.env = prevEnv;
    return Value(); // implicit nil
}

Value PraiaFunction::call(Interpreter& interp, const std::vector<Value>& args) {
    auto funcEnv = std::make_shared<Environment>(closure);

    // Switch to function env before evaluating defaults so that:
    // 1. Defaults resolve in the definition scope (closure), not the caller scope
    // 2. Earlier params are visible to later defaults (e.g. f(a=1, b=a+1))
    auto prevEnv = interp.env;
    interp.env = funcEnv;
    for (size_t i = 0; i < params.size(); i++) {
        if (i < args.size() && !args[i].isNil()) {
            funcEnv->define(params[i], args[i]);
        } else if (defaults && i < defaults->size() && (*defaults)[i]) {
            funcEnv->define(params[i], interp.evaluate((*defaults)[i].get()));
        } else if (i < args.size()) {
            funcEnv->define(params[i], args[i]);
        } else {
            funcEnv->define(params[i], Value());
        }
    }

    try {
        for (const auto& stmt : body->statements)
            interp.execute(stmt.get());
    } catch (const ReturnSignal& ret) {
        interp.env = prevEnv;
        return ret.value;
    } catch (...) {
        interp.env = prevEnv;
        throw;
    }
    interp.env = prevEnv;
    return Value(); // implicit nil
}

// ── Generator callables ──────────────────────────────────────
// Calling a generator function returns a PraiaGenerator object.
// The body runs on a dedicated thread, synchronized via condvars.

// Declared as friend in interpreter.h via PraiaGeneratorFunction/Lambda
// which call this helper. Access to private Interpreter members is needed
// for the generator thread to set env and call execute().
Value makeGeneratorFromEnv(
    std::shared_ptr<Environment> funcEnv,
    std::shared_ptr<Environment> sharedGlobals,
    const std::vector<StmtPtr>& bodyStmts,
    std::shared_ptr<PraiaGenerator> gen) {

    gen->isVM = false;
    gen->state = PraiaGenerator::State::CREATED;

    // Capture pointer to the AST statements (AST is kept alive by grainAsts/program)
    const auto* stmtsPtr = &bodyStmts;
    gen->thread = std::thread([gen, funcEnv, stmtsPtr, sharedGlobals]() {
        Interpreter genInterp(sharedGlobals);
        genInterp.env = funcEnv;

        // Wait for first next() call
        {
            std::unique_lock<std::mutex> lock(gen->mtx);
            gen->callerCV.wait(lock, [&] { return gen->resumed; });
            gen->resumed = false;
            if (gen->done) return;
        }

        gen->state = PraiaGenerator::State::RUNNING;

        try {
            for (const auto& stmt : *stmtsPtr)
                genInterp.execute(stmt.get());
        } catch (const ReturnSignal& ret) {
            std::lock_guard<std::mutex> lock(gen->mtx);
            gen->lastYielded = ret.value;
            gen->done = true;
            gen->hasValue = true;
            gen->state = PraiaGenerator::State::COMPLETED;
            gen->genCV.notify_one();
            return;
        } catch (const ThrowSignal& ts) {
            std::lock_guard<std::mutex> lock(gen->mtx);
            gen->errorMessage = ts.value.toString();
            gen->done = true;
            gen->hasValue = true;
            gen->state = PraiaGenerator::State::COMPLETED;
            gen->genCV.notify_one();
            return;
        } catch (const RuntimeError& err) {
            std::lock_guard<std::mutex> lock(gen->mtx);
            gen->errorMessage = err.what();
            gen->done = true;
            gen->hasValue = true;
            gen->state = PraiaGenerator::State::COMPLETED;
            gen->genCV.notify_one();
            return;
        } catch (...) {
            std::lock_guard<std::mutex> lock(gen->mtx);
            gen->errorMessage = "Generator failed";
            gen->done = true;
            gen->hasValue = true;
            gen->state = PraiaGenerator::State::COMPLETED;
            gen->genCV.notify_one();
            return;
        }

        // Body finished without return
        std::lock_guard<std::mutex> lock(gen->mtx);
        gen->lastYielded = Value();
        gen->done = true;
        gen->hasValue = true;
        gen->state = PraiaGenerator::State::COMPLETED;
        gen->genCV.notify_one();
    });

    return Value(gen);
}

Value PraiaGeneratorFunction::call(Interpreter& interp, const std::vector<Value>& args) {
    auto funcEnv = std::make_shared<Environment>(closure);
    auto gen = std::make_shared<PraiaGenerator>();

    auto prevEnv = interp.env;
    interp.env = funcEnv;
    for (size_t i = 0; i < params.size(); i++) {
        if (i < args.size() && !args[i].isNil()) {
            funcEnv->define(params[i], args[i]);
        } else if (defaults && i < defaults->size() && (*defaults)[i]) {
            funcEnv->define(params[i], interp.evaluate((*defaults)[i].get()));
        } else if (i < args.size()) {
            funcEnv->define(params[i], args[i]);
        } else {
            funcEnv->define(params[i], Value());
        }
    }
    interp.env = prevEnv;

    funcEnv->define("__gen__", Value(gen));
    return makeGeneratorFromEnv(funcEnv, interp.globals, body->statements, gen);
}

Value PraiaGeneratorLambda::call(Interpreter& interp, const std::vector<Value>& args) {
    auto funcEnv = std::make_shared<Environment>(closure);
    auto gen = std::make_shared<PraiaGenerator>();

    auto prevEnv = interp.env;
    interp.env = funcEnv;
    for (size_t i = 0; i < params.size(); i++) {
        if (i < args.size() && !args[i].isNil()) {
            funcEnv->define(params[i], args[i]);
        } else if (i < expr->defaults.size() && expr->defaults[i]) {
            funcEnv->define(params[i], interp.evaluate(expr->defaults[i].get()));
        } else if (i < args.size()) {
            funcEnv->define(params[i], args[i]);
        } else {
            funcEnv->define(params[i], Value());
        }
    }
    interp.env = prevEnv;

    funcEnv->define("__gen__", Value(gen));
    return makeGeneratorFromEnv(funcEnv, interp.globals, expr->body, gen);
}
