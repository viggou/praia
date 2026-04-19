#include "vm.h"
#include "../interpreter.h"
#include "../environment.h"

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
}
