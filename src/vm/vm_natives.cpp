#include "vm.h"
#include "../interpreter.h"
#include "../environment.h"
#include <algorithm>
#include <set>

// Call any Callable within the VM context.
// Handles VM closures (re-entrant execution), bound methods, and native functions.
Value callWithVM(VM& vm, std::shared_ptr<Callable> callable, const std::vector<Value>& args) {
    // VM closure
    auto* vmcc = dynamic_cast<VMClosureCallable*>(callable.get());
    if (vmcc && vmcc->vm) {
        int savedFrameCount = vm.frameCount;
        vm.push(Value()); // closure slot
        for (auto& arg : args) vm.push(arg);
        if (!vm.callClosure(vmcc->closure, static_cast<int>(args.size()), 0))
            return Value();
        auto result = vm.execute(savedFrameCount);
        if (result != VM::Result::OK) return Value();
        return vm.pop();
    }

    // Bound method
    auto* bound = dynamic_cast<VMBoundMethod*>(callable.get());
    if (bound) {
        int savedFrameCount = vm.frameCount;
        vm.push(bound->receiver); // slot 0 = this
        for (auto& arg : args) vm.push(arg);
        if (!vm.callClosure(bound->method, static_cast<int>(args.size()), 0))
            return Value();
        vm.frames[vm.frameCount - 1].definingClass = bound->definingClass;
        auto result = vm.execute(savedFrameCount);
        if (result != VM::Result::OK) return Value();
        return vm.pop();
    }

    // Native function
    auto* native = dynamic_cast<NativeFunction*>(callable.get());
    if (native) {
        return native->fn(args);
    }

    // Fallback
    Interpreter dummy;
    return callable->call(dummy, args);
}

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
        "Lock", "Channel", "futures", "sort", "filter", "map", "each", "keys", "values",
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

    // Override higher-order functions to work with VM closures
    // The tree-walker versions capture the Interpreter pointer, which doesn't
    // work for VM closures. These VM-aware versions call through callValue.

    // For filter/map/each: we can't easily call VM closures from a native function
    // because we don't have access to the VM's call stack. Instead, we override
    // these to work by calling the callable's call() method directly.
    // VM closures implement Callable::call() as a no-op, so we need to detect them
    // and handle them specially... This is a fundamental limitation.
    //
    // Pragmatic solution: override filter/map/each/sort with versions that
    // just call callable->call() with a dummy interpreter. For NativeFunctions
    // this works. For VM closures, call() is a no-op, so these won't work.
    //
    // The REAL fix: compile filter/map/each/sort as opcodes or have the VM
    // provide a callback mechanism. For now, users can use for-in loops + push
    // instead of filter/map in the VM. This is a known limitation.

    // Actually, a better approach: create VM-specific filter/map that use
    // the pipe operator pattern. Since pipe compiles to OP_CALL, it works.
    // But filter/map are global functions, not methods...
    //
    // OK, let me just leave the tree-walker versions for now. They work for
    // NativeFunction callables (e.g., built-in functions). For VM closures,
    // users should use for-in loops.

    // Override str() to call toString() on VM instances
    vm.defineNative("str", makeNat("str", 1,
        [&vm](const std::vector<Value>& args) -> Value {
            if (args[0].isInstance()) {
                auto inst = args[0].asInstance();
                if (inst->klass) {
                    // Look for toString in vmMethods
                    auto walk = inst->klass;
                    while (walk) {
                        auto it = walk->vmMethods.find("toString");
                        if (it != walk->vmMethods.end() && it->second.isCallable()) {
                            auto* vmcc = dynamic_cast<VMClosureCallable*>(it->second.asCallable().get());
                            if (vmcc && vmcc->vm) {
                                // Call toString() via re-entrant VM execution
                                vm.push(Value(inst)); // slot for 'this'
                                if (vm.callClosure(vmcc->closure, 0, 0)) {
                                    int saved = vm.frameCount;
                                    auto result = vm.execute(saved - 1);
                                    if (result == VM::Result::OK) {
                                        return vm.pop();
                                    }
                                }
                            }
                            break;
                        }
                        walk = walk->superclass;
                    }
                }
            }
            return Value(args[0].toString());
        }));

    // Override higher-order functions with VM-aware versions
    vm.defineNative("filter", makeNat("filter", 2,
        [&vm](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("filter() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("filter() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto result = std::make_shared<PraiaArray>();
            auto pred = args[1].asCallable();
            for (auto& elem : src) {
                Value test = callWithVM(vm, pred, {elem});
                if (test.isTruthy()) result->elements.push_back(elem);
            }
            return Value(result);
        }));

    vm.defineNative("map", makeNat("map", 2,
        [&vm](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("map() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("map() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto result = std::make_shared<PraiaArray>();
            auto transform = args[1].asCallable();
            for (auto& elem : src)
                result->elements.push_back(callWithVM(vm, transform, {elem}));
            return Value(result);
        }));

    vm.defineNative("each", makeNat("each", 2,
        [&vm](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("each() requires an array as first argument", 0);
            if (!args[1].isCallable())
                throw RuntimeError("each() requires a function as second argument", 0);
            auto& src = args[0].asArray()->elements;
            auto fn = args[1].asCallable();
            for (auto& elem : src)
                callWithVM(vm, fn, {elem});
            return args[0];
        }));

    vm.defineNative("sort", makeNat("sort", -1,
        [&vm](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isArray())
                throw RuntimeError("sort() requires an array", 0);
            auto sorted = std::make_shared<PraiaArray>();
            sorted->elements = args[0].asArray()->elements;
            auto& elems = sorted->elements;
            if (args.size() > 1 && args[1].isCallable()) {
                auto cmp = args[1].asCallable();
                std::sort(elems.begin(), elems.end(),
                    [&vm, &cmp](const Value& a, const Value& b) -> bool {
                        Value result = callWithVM(vm, cmp, {a, b});
                        if (result.isNumber()) return result.asNumber() < 0;
                        return result.isTruthy();
                    });
            } else {
                std::sort(elems.begin(), elems.end(),
                    [](const Value& a, const Value& b) {
                        if (a.isNumber() && b.isNumber()) return a.asNumber() < b.asNumber();
                        return a.toString() < b.toString();
                    });
            }
            return Value(sorted);
        }));

    // Convert any iterable to an array for for-in loops.
    // Arrays pass through, maps become [{key, value}, ...], strings become char arrays.
    vm.defineNative("__iterEntries", makeNat("__iterEntries", 1,
        [](const std::vector<Value>& args) -> Value {
            const Value& v = args[0];
            if (v.isArray()) return v;
            if (v.isMap()) {
                auto arr = std::make_shared<PraiaArray>();
                for (auto& [k, val] : v.asMap()->entries) {
                    auto entry = std::make_shared<PraiaMap>();
                    entry->entries["key"] = Value(k);
                    entry->entries["value"] = val;
                    arr->elements.push_back(Value(entry));
                }
                return Value(arr);
            }
            if (v.isString()) {
                auto arr = std::make_shared<PraiaArray>();
                for (char c : v.asString())
                    arr->elements.push_back(Value(std::string(1, c)));
                return Value(arr);
            }
            return Value(); // nil for non-iterables
        }));

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
