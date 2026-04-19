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

    for (size_t i = 0; i < params.size(); i++) {
        if (i < args.size()) {
            methodEnv->define(params[i], args[i]);
        } else if (decl && i < decl->defaults.size() && decl->defaults[i]) {
            methodEnv->define(params[i], interp.evaluate(decl->defaults[i].get()));
        } else {
            methodEnv->define(params[i], Value());
        }
    }

    auto prevEnv = interp.env;
    interp.env = methodEnv;
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
    for (size_t i = 0; i < params.size(); i++) {
        if (i < args.size()) {
            lambdaEnv->define(params[i], args[i]);
        } else if (i < expr->defaults.size() && expr->defaults[i]) {
            lambdaEnv->define(params[i], interp.evaluate(expr->defaults[i].get()));
        } else {
            lambdaEnv->define(params[i], Value());
        }
    }

    auto prevEnv = interp.env;
    interp.env = lambdaEnv;
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
    for (size_t i = 0; i < params.size(); i++) {
        if (i < args.size()) {
            funcEnv->define(params[i], args[i]);
        } else if (defaults && i < defaults->size() && (*defaults)[i]) {
            funcEnv->define(params[i], interp.evaluate((*defaults)[i].get()));
        } else {
            funcEnv->define(params[i], Value());
        }
    }

    try {
        interp.executeBlock(body, funcEnv);
    } catch (const ReturnSignal& ret) {
        return ret.value;
    }
    return Value(); // implicit nil
}
