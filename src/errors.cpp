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
    // String-forwarding compat shims. Praia code has, since the
    // pre-hierarchy days, treated the catch variable as a string
    // and called `.contains` / `.startsWith` on it. Keeping those
    // idioms working avoids a big-bang migration of hundreds of
    // existing call sites; each shim delegates to the raw `.message`
    // (NOT `str(this)`) so the substring space matches the legacy
    // `err.what()`-only semantics — a caller checking
    // `err.contains("Error")` shouldn't get a false positive from
    // the `"Error: "` prefix that `str(this)` prepends.
    // Subclasses inherit these unchanged. We deliberately do NOT
    // implement `__add` for `err + "suffix"` — adding an operator
    // overload makes `hasOperatorOverloads` true on every Error
    // instance and routes every arithmetic/string op through the
    // slow class-walk dispatch, which showed up as a real slowdown
    // in the http-stream test suite. Users concat with `str(err)`.
    func contains(sub) {
        return this.message.contains(sub)
    }
    func startsWith(prefix) {
        return this.message.startsWith(prefix)
    }
    func endsWith(suffix) {
        return this.message.endsWith(suffix)
    }
}

class TypeError extends Error {
    func init(message = "") {
        super.init(message)
        this.type = "TypeError"
    }
}

class ValueError extends Error {
    func init(message = "") {
        super.init(message)
        this.type = "ValueError"
    }
}

class NameError extends Error {
    func init(message = "") {
        super.init(message)
        this.type = "NameError"
    }
}

class IndexError extends Error {
    func init(message = "", index = nil) {
        super.init(message)
        this.type = "IndexError"
        this.index = index
    }
}

class KeyError extends Error {
    func init(message = "", key = nil) {
        super.init(message)
        this.type = "KeyError"
        this.key = key
    }
}

class AssertionError extends Error {
    func init(message = "") {
        super.init(message)
        this.type = "AssertionError"
    }
}

class IOError extends Error {
    func init(message = "", path = nil, errno = nil) {
        super.init(message)
        this.type = "IOError"
        this.path = path
        this.errno = errno
    }
}

class NetworkError extends IOError {
    func init(message = "", host = nil, port = nil, errno = nil) {
        super.init(message, nil, errno)
        this.type = "NetworkError"
        this.host = host
        this.port = port
    }
}

class HTTPError extends IOError {
    func init(message = "", status = nil, url = nil, body = nil) {
        super.init(message, nil, nil)
        this.type = "HTTPError"
        this.status = status
        this.url = url
        this.body = body
    }
}

class TimeoutError extends IOError {
    func init(message = "") {
        super.init(message)
        this.type = "TimeoutError"
    }
}

class ParseError extends Error {
    func init(message = "", line = 0, column = 0, source = nil) {
        super.init(message)
        this.type = "ParseError"
        this.line = line
        this.column = column
        this.source = source
    }
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
                        int line, int column) {
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
    return Value(inst);
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

Value wrapRuntimeErrorForInterpreter(Interpreter& interp,
                                     const RuntimeError& re) {
    Value klassVal = lookupErrorClass(interp, "Error");
    return makeErrorInstance(klassVal, "Error", re.what(), re.line, re.column);
}

Value wrapRuntimeErrorForVm(VM& vm, const RuntimeError& re) {
    Value klassVal = lookupErrorClass(vm, "Error");
    return makeErrorInstance(klassVal, "Error", re.what(), re.line, re.column);
}

} // namespace praia
