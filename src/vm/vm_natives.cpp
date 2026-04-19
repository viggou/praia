#include "vm.h"
#include "../interpreter.h" // for NativeFunction, makeNative equivalent
#include <iostream>
#include <sstream>

// Helper to create a NativeFunction value
static Value makeNat(const std::string& name, int arity,
                     std::function<Value(const std::vector<Value>&)> fn) {
    auto f = std::make_shared<NativeFunction>();
    f->funcName = name;
    f->numArgs = arity;
    f->fn = std::move(fn);
    return Value(std::static_pointer_cast<Callable>(f));
}

// Called from VM constructor or run() to populate globals
// For Phase 1, we just need print, len, type, str, num
void vmRegisterNatives(VM& vm) {
    vm.defineNative("print", makeNat("print", -1,
        [](const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].toString();
            }
            std::cout << "\n";
            return Value();
        }));

    vm.defineNative("len", makeNat("len", 1,
        [](const std::vector<Value>& args) -> Value {
            if (args[0].isArray()) return Value(static_cast<int64_t>(args[0].asArray()->elements.size()));
            if (args[0].isString()) return Value(static_cast<int64_t>(args[0].asString().size()));
            if (args[0].isMap()) return Value(static_cast<int64_t>(args[0].asMap()->entries.size()));
            throw RuntimeError("len() requires an array, string, or map", 0);
        }));

    vm.defineNative("type", makeNat("type", 1,
        [](const std::vector<Value>& args) -> Value {
            auto& v = args[0];
            if (v.isNil()) return Value("nil");
            if (v.isBool()) return Value("bool");
            if (v.isInt()) return Value("int");
            if (v.isDouble()) return Value("number");
            if (v.isString()) return Value("string");
            if (v.isArray()) return Value("array");
            if (v.isMap()) return Value("map");
            if (v.isInstance()) return Value("instance");
            if (v.isCallable()) return Value("function");
            return Value("unknown");
        }));

    vm.defineNative("str", makeNat("str", 1,
        [](const std::vector<Value>& args) -> Value {
            return Value(args[0].toString());
        }));

    vm.defineNative("num", makeNat("num", 1,
        [](const std::vector<Value>& args) -> Value {
            if (args[0].isNumber()) return args[0];
            if (args[0].isString()) {
                try { return Value(std::stod(args[0].asString())); }
                catch (...) { throw RuntimeError("Cannot convert string to number", 0); }
            }
            throw RuntimeError("Cannot convert to number", 0);
        }));
}

// Declare so vm.cpp can call it
void vmRegisterNatives(VM& vm);
