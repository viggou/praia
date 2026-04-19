#pragma once

#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

// Forward declarations
struct Callable;
struct PraiaArray;
struct PraiaMap;
struct PraiaInstance;
struct PraiaFuture;
class Interpreter;

// Runtime error with line info
struct RuntimeError : std::runtime_error {
    int line;
    RuntimeError(const std::string& msg, int line)
        : std::runtime_error(msg), line(line) {}
};

// The universal Praia value type
struct Value {
    using Data = std::variant<
        std::nullptr_t,
        bool,
        int64_t,
        double,
        std::string,
        std::shared_ptr<Callable>,
        std::shared_ptr<PraiaArray>,
        std::shared_ptr<PraiaMap>,
        std::shared_ptr<PraiaInstance>,
        std::shared_ptr<PraiaFuture>
    >;

    Data data;

    Value() : data(nullptr) {}
    Value(std::nullptr_t) : data(nullptr) {}
    Value(bool b) : data(b) {}
    Value(int64_t i) : data(i) {}
    Value(int i) : data(static_cast<int64_t>(i)) {}
    Value(double d) : data(d) {}
    Value(const std::string& s) : data(s) {}
    Value(std::string&& s) : data(std::move(s)) {}
    Value(const char* s) : data(std::string(s)) {}
    Value(std::shared_ptr<Callable> c) : data(std::move(c)) {}
    Value(std::shared_ptr<PraiaArray> a) : data(std::move(a)) {}
    Value(std::shared_ptr<PraiaMap> m) : data(std::move(m)) {}
    Value(std::shared_ptr<PraiaInstance> i) : data(std::move(i)) {}
    Value(std::shared_ptr<PraiaFuture> f) : data(std::move(f)) {}

    bool isNil()      const { return std::holds_alternative<std::nullptr_t>(data); }
    bool isBool()     const { return std::holds_alternative<bool>(data); }
    bool isInt()      const { return std::holds_alternative<int64_t>(data); }
    bool isDouble()   const { return std::holds_alternative<double>(data); }
    bool isNumber()   const { return isInt() || isDouble(); }
    bool isString()   const { return std::holds_alternative<std::string>(data); }
    bool isCallable() const { return std::holds_alternative<std::shared_ptr<Callable>>(data); }
    bool isArray()    const { return std::holds_alternative<std::shared_ptr<PraiaArray>>(data); }
    bool isMap()      const { return std::holds_alternative<std::shared_ptr<PraiaMap>>(data); }
    bool isInstance() const { return std::holds_alternative<std::shared_ptr<PraiaInstance>>(data); }
    bool isFuture()   const { return std::holds_alternative<std::shared_ptr<PraiaFuture>>(data); }

    bool                        asBool()     const { return std::get<bool>(data); }
    int64_t                     asInt()      const { return std::get<int64_t>(data); }
    // asNumber() returns double regardless of int/double storage
    double                      asNumber()   const {
        if (isInt()) return static_cast<double>(std::get<int64_t>(data));
        return std::get<double>(data);
    }
    const std::string&          asString()   const { return std::get<std::string>(data); }
    std::shared_ptr<Callable>   asCallable() const { return std::get<std::shared_ptr<Callable>>(data); }
    std::shared_ptr<PraiaArray> asArray()    const { return std::get<std::shared_ptr<PraiaArray>>(data); }
    std::shared_ptr<PraiaMap>      asMap()      const { return std::get<std::shared_ptr<PraiaMap>>(data); }
    std::shared_ptr<PraiaInstance> asInstance() const { return std::get<std::shared_ptr<PraiaInstance>>(data); }
    std::shared_ptr<PraiaFuture>   asFuture()   const { return std::get<std::shared_ptr<PraiaFuture>>(data); }

    bool isTruthy() const {
        if (isNil()) return false;
        if (isBool()) return asBool();
        if (isInt()) return asInt() != 0;
        if (isDouble()) return asNumber() != 0;
        return true;
    }

    // Declared here, defined after PraiaArray/PraiaMap (needs complete types)
    std::string toString() const;
    bool operator==(const Value& o) const;
    bool operator!=(const Value& o) const { return !(*this == o); }
};

// Concrete types — defined after Value so they can hold Values
struct PraiaArray {
    std::vector<Value> elements;
};

struct PraiaMap {
    std::unordered_map<std::string, Value> entries;
};

struct PraiaFuture {
    std::shared_future<Value> future;
};

struct PraiaClass;  // defined in interpreter.h (it's a Callable)

struct PraiaInstance {
    std::shared_ptr<PraiaClass> klass;
    std::unordered_map<std::string, Value> fields;
};

// ── Value method definitions (need complete types) ──

inline std::string Value::toString() const {
    if (isNil())    return "nil";
    if (isBool())   return asBool() ? "true" : "false";
    if (isInt())    return std::to_string(asInt());
    if (isDouble()) { std::ostringstream o; o << asNumber(); return o.str(); }
    if (isString()) return asString();
    if (isCallable()) return "<function>";
    if (isInstance()) return "<instance>";
    if (isFuture()) return "<future>";
    if (isArray()) {
        std::ostringstream o;
        o << "[";
        auto& elems = asArray()->elements;
        for (size_t i = 0; i < elems.size(); i++) {
            if (i > 0) o << ", ";
            if (elems[i].isString()) o << "\"" << elems[i].toString() << "\"";
            else o << elems[i].toString();
        }
        o << "]";
        return o.str();
    }
    if (isMap()) {
        std::ostringstream o;
        o << "{";
        bool first = true;
        for (auto& [k, v] : asMap()->entries) {
            if (!first) o << ", ";
            first = false;
            o << k << ": ";
            if (v.isString()) o << "\"" << v.toString() << "\"";
            else o << v.toString();
        }
        o << "}";
        return o.str();
    }
    return "<unknown>";
}

inline bool Value::operator==(const Value& o) const {
    if (isNil()    && o.isNil())    return true;
    if (isNil()    || o.isNil())    return false;
    if (isBool()   && o.isBool())   return asBool()   == o.asBool();
    if (isNumber() && o.isNumber()) return asNumber()  == o.asNumber();
    if (isString() && o.isString()) return asString()  == o.asString();
    if (isArray()  && o.isArray()) {
        auto& a = asArray()->elements;
        auto& b = o.asArray()->elements;
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (a[i] != b[i]) return false;
        return true;
    }
    if (isMap() && o.isMap()) {
        auto& a = asMap()->entries;
        auto& b = o.asMap()->entries;
        if (a.size() != b.size()) return false;
        for (auto& [k, v] : a) {
            auto it = b.find(k);
            if (it == b.end() || it->second != v) return false;
        }
        return true;
    }
    // Instances: reference equality
    if (isInstance() && o.isInstance())
        return asInstance() == o.asInstance();
    return false;
}

// Interface for anything that can be called (user functions + built-ins)
struct Callable {
    virtual ~Callable() = default;
    virtual Value call(Interpreter& interp, const std::vector<Value>& args) = 0;
    virtual int arity() const = 0;   // -1 = variadic
    virtual std::string name() const = 0;
};
