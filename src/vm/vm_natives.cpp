#include "vm.h"
#include "../interpreter.h"
#include "../environment.h"
#include <set>

// Reuse ALL builtins from the tree-walker by creating an Interpreter
// and copying its global environment into the VM.
void vmRegisterNatives(VM& vm) {
    // The Interpreter constructor registers all builtins:
    // print, len, push, pop, type, str, num, fromCharCode,
    // Lock, sort, filter, map, each, keys, values,
    // sys, http, json, yaml, base64, path, url, net, bytes, crypto,
    // random, time, math, sqlite
    Interpreter interp;

    auto globals = interp.getGlobals();

    std::vector<std::string> globalNames = {
        "print", "len", "push", "pop", "type", "str", "num", "fromCharCode",
        "Lock", "sort", "filter", "map", "each", "keys", "values",
        "sys", "http", "json", "yaml", "base64", "path", "url", "net",
        "bytes", "crypto", "random", "time", "math",
#ifdef HAVE_SQLITE
        "sqlite",
#endif
    };

    for (auto& name : globalNames) {
        try {
            Value val = globals->get(name, 0);
            vm.defineNative(name, val);
        } catch (...) {}
    }

    // Internal helpers for destructuring rest patterns
    auto makeNat = [](const std::string& name, int arity,
                      std::function<Value(const std::vector<Value>&)> fn) -> Value {
        auto f = std::make_shared<NativeFunction>();
        f->funcName = name;
        f->numArgs = arity;
        f->fn = std::move(fn);
        return Value(std::static_pointer_cast<Callable>(f));
    };

    vm.defineNative("__arraySlice", makeNat("__arraySlice", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray() || !args[1].isNumber()) return Value();
            auto& elems = args[0].asArray()->elements;
            int start = static_cast<int>(args[1].asNumber());
            auto rest = std::make_shared<PraiaArray>();
            for (int i = start; i < static_cast<int>(elems.size()); i++)
                rest->elements.push_back(elems[i]);
            return Value(rest);
        }));

    vm.defineNative("__mapRest", makeNat("__mapRest", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isMap() || !args[1].isArray()) return Value();
            auto& entries = args[0].asMap()->entries;
            auto& excludeArr = args[1].asArray()->elements;
            std::set<std::string> exclude;
            for (auto& e : excludeArr) {
                if (e.isString()) exclude.insert(e.asString());
            }
            auto rest = std::make_shared<PraiaMap>();
            for (auto& [k, v] : entries) {
                if (!exclude.count(k)) rest->entries[k] = v;
            }
            return Value(rest);
        }));
}
