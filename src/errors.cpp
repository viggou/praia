#include "errors.h"

#include "environment.h"
#include "gc_heap.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "vm/compiler.h"
#include "vm/vm.h"

#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace praia {

// ── Bootstrap source ──────────────────────────────────────────────
//
// The Praia source below is executed by whichever engine is running
// at construction time. Each class populates the appropriate method
// map (tree-walker: `methods`; VM: `vmMethods`) via the engine's
// normal class-definition path. See src/errors.h for the design.
//
// Design notes:
// - `.message`, `.type`, `.line`, `.column` are set by the base
//   Error init and inherited by every subclass; subclasses override
//   `.type` and add their own fields (path, errno, host, status, …).
// - `__str()` renders `"<type>: <message>"` so `print(e)` and
//   string interpolation display the class name plus the message
//   without callers having to concatenate the two themselves.
// - `.contains` / `.startsWith` / `.endsWith` on Error forward to
//   the message string so pre-hierarchy tests that string-op the
//   caught RuntimeError (there are ~40 across the suite) keep working
//   without a sweeping migration. Delegate to `.message` — not
//   `str(this)` — so `"Error: "` prefix doesn't leak into substring
//   checks.
// - `errno` / `line` / `column` are just Praia identifiers here —
//   they don't conflict with C++ macros because this is Praia source.
const char* const kErrorClassesSource = R"PRAIA(
class Error {
    func init(message = "") {
        this.message = str(message)
        this.type = "Error"
        this.line = 0
        this.column = 0
    }
    func __str() {
        return this.type + ": " + this.message
    }
    func contains(needle) { return this.message.contains(needle) }
    func startsWith(prefix) { return this.message.startsWith(prefix) }
    func endsWith(suffix) { return this.message.endsWith(suffix) }
}

class TypeError extends Error {
    func init(message = "") { super.init(message); this.type = "TypeError" }
}
class ValueError extends Error {
    func init(message = "") { super.init(message); this.type = "ValueError" }
}
class NameError extends Error {
    func init(message = "") { super.init(message); this.type = "NameError" }
}
class IndexError extends Error {
    func init(message = "", index = nil) { super.init(message); this.type = "IndexError"; this.index = index }
}
class KeyError extends Error {
    func init(message = "", key = nil) { super.init(message); this.type = "KeyError"; this.key = key }
}
class AssertionError extends Error {
    func init(message = "") { super.init(message); this.type = "AssertionError" }
}
class IOError extends Error {
    func init(message = "", path = nil, errno = nil) { super.init(message); this.type = "IOError"; this.path = path; this.errno = errno }
}
class NetworkError extends IOError {
    func init(message = "", host = nil, port = nil, errno = nil) { super.init(message, nil, errno); this.type = "NetworkError"; this.host = host; this.port = port }
}
class HTTPError extends IOError {
    func init(message = "", status = nil, url = nil, body = nil) { super.init(message, nil, nil); this.type = "HTTPError"; this.status = status; this.url = url; this.body = body }
}
class TimeoutError extends IOError {
    func init(message = "") { super.init(message); this.type = "TimeoutError" }
}
class ParseError extends Error {
    func init(message = "", line = 0, column = 0, source = nil) { super.init(message); this.type = "ParseError"; this.line = line; this.column = column; this.source = source }
}
)PRAIA";

// ── Bootstrap execution ───────────────────────────────────────────

// Parse the bootstrap source into an AST held by a shared_ptr. The
// tree-walker's class-registration path stores raw `const ClassMethod*`
// pointers into this AST inside `PraiaClass::methods`; if the AST
// vector is destroyed those pointers dangle. Anchoring the parsed
// program in a static (process-lifetime) container keeps the class
// methods live for the whole run without threading ownership through
// the interpreter's per-call state.
using BootstrapProgram = std::shared_ptr<std::vector<StmtPtr>>;

static BootstrapProgram parseBootstrapSource() {
    Lexer lexer(kErrorClassesSource);
    auto tokens = lexer.tokenize();
    if (lexer.hasError()) {
        std::cerr << "internal: Error-class bootstrap failed to lex" << std::endl;
        return nullptr;
    }
    Parser parser(tokens);
    auto program = parser.parse();
    if (parser.hasError()) {
        std::cerr << "internal: Error-class bootstrap failed to parse" << std::endl;
        return nullptr;
    }
    return std::make_shared<std::vector<StmtPtr>>(std::move(program));
}

// Single process-lifetime BootstrapProgram shared across every
// engine an embedder constructs. Parsed at most once (lazily, on
// first bootstrap call) and kept alive forever so the tree-walker's
// `PraiaClass::methods` — which stores raw `const ClassMethod*`
// pointers into this AST — remains valid for as long as any engine
// holds those classes. The mutex serialises the one-shot parse
// under concurrent engine construction.
static std::mutex& sharedBootstrapMutex() {
    static std::mutex m;
    return m;
}

// Returns the shared AST. First caller parses; subsequent callers
// get the same shared_ptr. Returns nullptr only if the initial parse
// itself failed.
static const BootstrapProgram& sharedBootstrapProgram() {
    static BootstrapProgram cached;
    static bool initialised = false;
    std::lock_guard<std::mutex> lock(sharedBootstrapMutex());
    if (!initialised) {
        cached = parseBootstrapSource();
        initialised = true;
    }
    return cached;
}

void bootstrapErrorClasses(Interpreter& interp) {
    const auto& program = sharedBootstrapProgram();
    if (!program) return;
    try {
        interp.interpret(*program);
    } catch (const std::exception& e) {
        std::cerr << "internal: Error-class bootstrap failed to run: "
                  << e.what() << std::endl;
    }
}

void bootstrapErrorClasses(VM& vm) {
    const auto& program = sharedBootstrapProgram();
    if (!program) return;
    // The VM lowers the AST to bytecode inside
    // `compiler.compile(*program)`; the resulting PraiaClass
    // entries hold their methods in `vmMethods` (bytecode-based)
    // and don't reference the AST nodes. Sharing the parsed AST
    // across VMs is still fine — compile() reads it as const.
    Compiler compiler;
    auto script = compiler.compile(*program);
    if (!script) {
        std::cerr << "internal: Error-class bootstrap failed to compile for VM"
                  << std::endl;
        return;
    }
    try {
        vm.run(script);
    } catch (const std::exception& e) {
        std::cerr << "internal: Error-class bootstrap failed to run in VM: "
                  << e.what() << std::endl;
    }
}

// ── Instance construction ─────────────────────────────────────────

Value makeErrorInstance(const Value& classValue,
                        const std::string& className,
                        const std::string& message,
                        int line, int column,
                        const std::unordered_map<std::string, Value>& extras) {
    if (!classValue.isCallable()) {
        // Class wasn't loaded — fall back to the pre-hierarchy
        // string shape so the catch handler still receives *something*
        // usable and the test suite doesn't hard-crash if the
        // bootstrap ever regresses.
        return Value(message);
    }
    auto klass = std::dynamic_pointer_cast<PraiaClass>(classValue.asCallable());
    if (!klass) return Value(message);

    auto inst = gcNew<PraiaInstance>();
    inst->klass = klass;
    inst->fields["message"] = Value(message);
    inst->fields["type"] = Value(className);
    inst->fields["line"] = Value(static_cast<int64_t>(line));
    inst->fields["column"] = Value(static_cast<int64_t>(column));
    // Populate every subclass-declared field with nil so a caller
    // that dispatches on the exact class (`e is IOError`) can safely
    // read `e.path` / `e.errno` / `e.status` / … without getting a
    // "no such property" runtime error. C++ callers of this helper
    // bypass the class's Praia-side init (which is where those
    // defaults would normally be set), so we mirror the field shape
    // here. Keep in sync with the class definitions in
    // `kErrorClassesSource`.
    inst->fields["index"]  = Value();  // IndexError
    inst->fields["key"]    = Value();  // KeyError
    inst->fields["path"]   = Value();  // IOError (+ inherited)
    inst->fields["errno"]  = Value();  // IOError, NetworkError
    inst->fields["host"]   = Value();  // NetworkError
    inst->fields["port"]   = Value();  // NetworkError
    inst->fields["status"] = Value();  // HTTPError
    inst->fields["url"]    = Value();  // HTTPError
    inst->fields["body"]   = Value();  // HTTPError
    inst->fields["source"] = Value();  // ParseError
    // Overlay caller-provided extras on top of the nil defaults so
    // typed throw helpers can populate the subclass fields directly.
    // Guard the base four (message/type/line/column) from overwrite
    // — those are set from the dedicated positional args and letting
    // extras clobber them would break `is` / str() rendering.
    for (const auto& kv : extras) {
        if (kv.first == "message" || kv.first == "type" ||
            kv.first == "line"    || kv.first == "column") {
            continue;
        }
        inst->fields[kv.first] = kv.second;
    }
    return Value(inst);
}

// Backwards-compatible 4-arg overload — thin forwarder to the 5-arg
// version with an empty extras map. Every catch site that predates
// the typed-throw work calls this shape.
Value makeErrorInstance(const Value& classValue,
                        const std::string& className,
                        const std::string& message,
                        int line, int column) {
    static const std::unordered_map<std::string, Value> kEmpty;
    return makeErrorInstance(classValue, className, message, line, column, kEmpty);
}

// ── Class lookups ─────────────────────────────────────────────────

Value lookupErrorClass(Interpreter& interp, const std::string& className) {
    auto globals = interp.getGlobals();
    if (!globals) return Value();
    // Environment::get throws on miss; catch and return nil so the
    // caller can fall back gracefully.
    try {
        return globals->get(className, /*line=*/0);
    } catch (const RuntimeError&) {
        return Value();
    }
}

Value lookupErrorClass(VM& vm, const std::string& className) {
    return vm.lookupGlobal(className);
}

// ── Catch coercion ────────────────────────────────────────────────

// A resolved class name must be a real PraiaClass — not just any
// Callable — before we can instantiate an error via it. User code (or
// a plugin) could shadow "IOError" with a plain lambda; that lambda is
// callable but not instantiable, and makeErrorInstance would silently
// fall back to Value(message), stripping the typed-throw's structured
// payload. This helper lets both wrap functions reject non-class
// callables early and fall back to the base Error class.
static bool isPraiaClassValue(const Value& v) {
    if (!v.isCallable()) return false;
    return std::dynamic_pointer_cast<PraiaClass>(v.asCallable()) != nullptr;
}

Value wrapRuntimeErrorForInterpreter(Interpreter& interp,
                                     const RuntimeError& re) {
    // A typed throw carries the target class + structured payload.
    // Look up the specific class; if the bootstrap didn't register
    // it (typo in throwPraiaError, engine constructed with
    // NoErrorBootstrap, etc.) — or if user code shadowed the name
    // with a non-class callable — fall back to base `Error`, and use
    // "Error" for the .type / str(e) rendering too so the reported
    // class name never lies about the actual klass on the instance.
    if (auto* tre = dynamic_cast<const TypedRuntimeError*>(&re)) {
        std::string cls = tre->className;
        Value klass = lookupErrorClass(interp, cls);
        if (!isPraiaClassValue(klass)) {
            cls = "Error";
            klass = lookupErrorClass(interp, cls);
        }
        return makeErrorInstance(klass, cls, tre->what(),
                                 tre->line, tre->column, tre->fields);
    }
    Value klassVal = lookupErrorClass(interp, "Error");
    return makeErrorInstance(klassVal, "Error", re.what(), re.line, re.column);
}

Value wrapRuntimeErrorForVm(VM& vm, const RuntimeError& re) {
    if (auto* tre = dynamic_cast<const TypedRuntimeError*>(&re)) {
        std::string cls = tre->className;
        Value klass = lookupErrorClass(vm, cls);
        if (!isPraiaClassValue(klass)) {
            cls = "Error";
            klass = lookupErrorClass(vm, cls);
        }
        return makeErrorInstance(klass, cls, tre->what(),
                                 tre->line, tre->column, tre->fields);
    }
    Value klassVal = lookupErrorClass(vm, "Error");
    return makeErrorInstance(klassVal, "Error", re.what(), re.line, re.column);
}

// ── Typed-throw helpers ───────────────────────────────────────────
//
// Each helper builds a small `fields` map with only the arguments the
// caller actually provided (empty string / zero → omit the field so
// the default nil in `makeErrorInstance` shows through, distinguishing
// "not set" from "explicitly zero"). Then throws a TypedRuntimeError
// that the interpreter/VM catch handler unpacks into the corresponding
// Error subclass instance.
//
// Field name notes (must match kErrorClassesSource):
//   IOError:      path, errno
//   NetworkError: host, port, errno (+ inherited path)
//   HTTPError:    status, url, body (+ inherited path, errno)
//   ParseError:   line (source line, not throw location), column, source
//   IndexError:   index
//   KeyError:     key
// The C++ arg names use `errnoVal` (POSIX `errno` is a macro),
// `srcLine`/`srcColumn` (avoids shadowing the throw-location `line`/
// `column` on ParseError). Praia-side field names stay unshadowed.

void throwTypeError(const std::string& msg, int line, int column) {
    throw TypedRuntimeError("TypeError", msg, {}, line, column);
}

void throwValueError(const std::string& msg, int line, int column) {
    throw TypedRuntimeError("ValueError", msg, {}, line, column);
}

void throwNameError(const std::string& msg, int line, int column) {
    throw TypedRuntimeError("NameError", msg, {}, line, column);
}

void throwAssertionError(const std::string& msg, int line, int column) {
    throw TypedRuntimeError("AssertionError", msg, {}, line, column);
}

void throwIndexError(const std::string& msg, Value index, int line, int column) {
    std::unordered_map<std::string, Value> f;
    if (!index.isNil()) f["index"] = std::move(index);
    throw TypedRuntimeError("IndexError", msg, std::move(f), line, column);
}

void throwKeyError(const std::string& msg, Value key, int line, int column) {
    std::unordered_map<std::string, Value> f;
    if (!key.isNil()) f["key"] = std::move(key);
    throw TypedRuntimeError("KeyError", msg, std::move(f), line, column);
}

void throwIOError(const std::string& msg,
                  const std::string& path, int errnoVal,
                  int line, int column) {
    std::unordered_map<std::string, Value> f;
    if (!path.empty())  f["path"]  = Value(path);
    if (errnoVal != 0)  f["errno"] = Value(static_cast<int64_t>(errnoVal));
    throw TypedRuntimeError("IOError", msg, std::move(f), line, column);
}

void throwNetworkError(const std::string& msg,
                       const std::string& host, int port, int errnoVal,
                       int line, int column) {
    std::unordered_map<std::string, Value> f;
    if (!host.empty())  f["host"]  = Value(host);
    if (port != 0)      f["port"]  = Value(static_cast<int64_t>(port));
    if (errnoVal != 0)  f["errno"] = Value(static_cast<int64_t>(errnoVal));
    throw TypedRuntimeError("NetworkError", msg, std::move(f), line, column);
}

void throwHTTPError(const std::string& msg,
                    int status, const std::string& url, const std::string& body,
                    int line, int column) {
    std::unordered_map<std::string, Value> f;
    if (status != 0)   f["status"] = Value(static_cast<int64_t>(status));
    if (!url.empty())  f["url"]    = Value(url);
    if (!body.empty()) f["body"]   = Value(body);
    throw TypedRuntimeError("HTTPError", msg, std::move(f), line, column);
}

void throwTimeoutError(const std::string& msg, int line, int column) {
    throw TypedRuntimeError("TimeoutError", msg, {}, line, column);
}

void throwParseError(const std::string& msg,
                     int srcLine, int srcColumn, const std::string& source) {
    std::unordered_map<std::string, Value> f;
    if (!source.empty()) f["source"] = Value(source);
    // Route srcLine/srcColumn through the RuntimeError's own
    // line/column channel — that's what makeErrorInstance uses to
    // populate the instance's `.line` / `.column` fields. ParseError's
    // Praia-side init treats `.line` / `.column` as the *source*
    // location (not the C++ throw site), so this puts the source
    // position exactly where user code expects to read it.
    throw TypedRuntimeError("ParseError", msg, std::move(f), srcLine, srcColumn);
}

void throwPraiaError(const std::string& className,
                     const std::string& msg,
                     std::unordered_map<std::string, Value> extras,
                     int line, int column) {
    throw TypedRuntimeError(className, msg, std::move(extras), line, column);
}

} // namespace praia
