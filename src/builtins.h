#pragma once

#include "interpreter.h"
#include "value.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

// Factory for native (C++) functions exposed as Praia callables.
inline std::shared_ptr<NativeFunction> makeNative(
    const std::string& name, int arity,
    std::function<Value(const std::vector<Value>&)> fn) {
    auto f = std::make_shared<NativeFunction>();
    f->funcName = name;
    f->numArgs = arity;
    f->fn = std::move(fn);
    return f;
}

// ── HTTP (builtins_http.cpp) ─────────────────────────────────
Value doHttpRequest(const std::string& method, const std::string& url,
                    const std::string& body,
                    const std::unordered_map<std::string, std::string>& extraHeaders);
void httpServerListen(int port, std::shared_ptr<Callable> handler, Interpreter& interp);

// ── JSON (builtins_json.cpp) ─────────────────────────────────
Value jsonParse(const std::string& src);
std::string jsonStringify(const Value& val, int indent = 0, int depth = 0);

// ── YAML (builtins_yaml.cpp) ─────────────────────────────────
Value yamlParse(const std::string& src);
std::string yamlStringify(const Value& val, int depth = 0);

// ── String / Array dot-method dispatch (builtins_methods.cpp) ─
Value getStringMethod(const std::string& str, const std::string& name, int line);
Value getArrayMethod(std::shared_ptr<PraiaArray> arr, const std::string& name, int line,
                     Interpreter* interp = nullptr);
