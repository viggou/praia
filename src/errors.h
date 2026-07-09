#pragma once

// Builtin Error class hierarchy + catch-coercion helpers.
//
// The hierarchy itself is defined in Praia (see `kErrorClassesSource`
// in errors.cpp) and installed as builtin globals during engine
// setup. That lets `throw TypeError("bad type")` and
// `catch (e) { e is Error }` work without any `use "..."` line.
//
// The two engines diverge in how classes carry their method bodies
// — tree-walker fills `PraiaClass::methods` from AST nodes, VM fills
// `PraiaClass::vmMethods` from compiled bytecode — so the bootstrap
// runs once per engine at construction time, populating whichever
// map the running engine needs. See `bootstrapErrorClasses` overloads
// below.
//
// The C++ side of the story is `wrapRuntimeErrorForInterpreter` /
// `wrapRuntimeErrorForVm`, which sit at every catch coercion site
// that used to do `Value(std::string(err.what()))`. They look up the
// `Error` class from globals and construct a `PraiaInstance` with
// message/type/line/column fields populated directly — bypassing the
// class's Praia-side init because at catch time we might not have
// access to a live method-dispatch path in the VM.

#include "value.h"

#include <memory>
#include <string>
#include <unordered_map>

class Interpreter;
class Environment;
class VM;

// RuntimeError variant carrying the target Praia Error subclass name
// plus its structured field payload. Constructed by the praia::throw*
// helpers below and unpacked by wrapRuntimeErrorFor{Interpreter,Vm}
// via dynamic_cast — bare `throw RuntimeError(msg, line)` continues
// to wrap as the base Error, so per-site migration is fully
// incremental. Piggybacks on the existing catch pipeline (interpreter
// try/catch at src/interpreter.cpp:829 + VM OP_THROW) — no new
// exception type on the C++ side.
struct TypedRuntimeError : RuntimeError {
    std::string className;
    std::unordered_map<std::string, Value> fields;
    TypedRuntimeError(std::string cls,
                      const std::string& msg,
                      std::unordered_map<std::string, Value> f,
                      int line, int column)
        : RuntimeError(msg, line, column),
          className(std::move(cls)),
          fields(std::move(f)) {}
};

namespace praia {

// The Praia source that defines Error + all subclasses. Kept as a
// string constant so both engines can lex/parse/execute it via their
// normal machinery. `const char* const` (pointer itself is const)
// so nothing can reassign the symbol to point at another buffer.
extern const char* const kErrorClassesSource;

// Run the bootstrap source once at engine construction. Each overload
// is a no-op if the source fails to compile (defensive — an engine
// with no Error class still works, catch just falls back to strings).
void bootstrapErrorClasses(Interpreter& interp);
void bootstrapErrorClasses(VM& vm);

// Construct a PraiaInstance of the named class carrying the given
// message + location. Fields are populated directly (message, type,
// line, column) without calling the class's Praia-side init — that
// keeps the helper cheap and callable from any C++ throw site.
//
// `className` typically matches the class registered by the
// bootstrap (Error, TypeError, IOError, …). If the class isn't in
// globals — e.g. the bootstrap failed — the helper falls back to a
// bare string Value so the catch handler still sees *something*.
Value makeErrorInstance(const Value& classValue,
                        const std::string& className,
                        const std::string& message,
                        int line, int column);

// Extended overload: overlays `extras` on top of the default
// nil-populated subclass fields (index, key, path, errno, host, port,
// status, url, body, source). Used by the wrap functions when they
// unpack a TypedRuntimeError, and by the throw* helpers indirectly.
// `message`, `type`, `line`, `column` are guarded against overwrite —
// callers can't accidentally clobber the base four via `extras`.
Value makeErrorInstance(const Value& classValue,
                        const std::string& className,
                        const std::string& message,
                        int line, int column,
                        const std::unordered_map<std::string, Value>& extras);

// ── Typed-throw helpers ───────────────────────────────────────────
//
// Each helper throws a TypedRuntimeError; the interpreter's outer
// try/catch (src/interpreter.cpp:829) and the VM's OP_THROW handler
// unpack it into the corresponding Praia Error subclass instance.
// `[[noreturn]]` so callers drop `throw` at the call site and the
// compiler still tracks control flow.
//
// The C++ arg name `errnoVal` avoids the POSIX `errno` macro; the
// Praia-side field name is `"errno"` (matches `kErrorClassesSource`).
[[noreturn]] void throwTypeError     (const std::string& msg, int line = 0, int column = 0);
[[noreturn]] void throwValueError    (const std::string& msg, int line = 0, int column = 0);
[[noreturn]] void throwNameError     (const std::string& msg, int line = 0, int column = 0);
[[noreturn]] void throwAssertionError(const std::string& msg, int line = 0, int column = 0);
[[noreturn]] void throwIndexError    (const std::string& msg, Value index = Value(),
                                      int line = 0, int column = 0);
[[noreturn]] void throwKeyError      (const std::string& msg, Value key = Value(),
                                      int line = 0, int column = 0);
[[noreturn]] void throwIOError       (const std::string& msg,
                                      const std::string& path = "",
                                      int errnoVal = 0,
                                      int line = 0, int column = 0);
[[noreturn]] void throwNetworkError  (const std::string& msg,
                                      const std::string& host = "",
                                      int port = 0,
                                      int errnoVal = 0,
                                      int line = 0, int column = 0);
[[noreturn]] void throwHTTPError     (const std::string& msg,
                                      int status = 0,
                                      const std::string& url = "",
                                      const std::string& body = "",
                                      int line = 0, int column = 0);
[[noreturn]] void throwTimeoutError  (const std::string& msg, int line = 0, int column = 0);
[[noreturn]] void throwParseError    (const std::string& msg,
                                      int srcLine = 0, int srcColumn = 0,
                                      const std::string& source = "",
                                      int line = 0, int column = 0);

// Escape hatch for classes not covered above (e.g. user-registered
// subclasses via grains). Any keys in `extras` land as instance fields
// after the nil defaults, with the base four (message/type/line/column)
// guarded against overwrite. If the class isn't registered the wrapper
// falls back to base `Error` at catch time.
[[noreturn]] void throwPraiaError    (const std::string& className,
                                      const std::string& msg,
                                      std::unordered_map<std::string, Value> extras = {},
                                      int line = 0, int column = 0);

// Engine-specific class lookups. Return the class Value if found in
// globals; nil Value otherwise (caller falls back to string coercion).
Value lookupErrorClass(Interpreter& interp, const std::string& className);
Value lookupErrorClass(VM& vm, const std::string& className);

// Shared catch-coercion helpers used at every catch site that used
// to do `Value(std::string(err.what()))`. Returns the caught-error
// value that gets bound to the user's catch variable.
Value wrapRuntimeErrorForInterpreter(Interpreter& interp,
                                     const RuntimeError& re);
Value wrapRuntimeErrorForVm(VM& vm, const RuntimeError& re);

} // namespace praia
