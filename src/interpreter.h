#pragma once

#include "ast.h"
#include "environment.h"
#include "value.h"
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Thrown by return statements to unwind back to the enclosing function call
struct ReturnSignal {
    Value value;
};

// Thrown by break/continue to unwind to the enclosing loop
struct BreakSignal {};
struct ContinueSignal {};

// Thrown by throw statements, caught by try/catch
struct ThrowSignal {
    Value value;
    int line;
};

// A user-defined Praia function
struct PraiaFunction : Callable {
    std::string funcName;
    std::vector<std::string> params;
    const BlockStmt* body;
    std::shared_ptr<Environment> closure;

    Value call(Interpreter& interp, const std::vector<Value>& args) override;
    int arity() const override { return static_cast<int>(params.size()); }
    std::string name() const override { return funcName; }
};

// A lambda (anonymous function)
struct PraiaLambda : Callable {
    std::vector<std::string> params;
    const LambdaExpr* expr;  // points into AST (kept alive by grainAsts/astStore)
    std::shared_ptr<Environment> closure;

    Value call(Interpreter& interp, const std::vector<Value>& args) override;
    int arity() const override { return static_cast<int>(params.size()); }
    std::string name() const override { return "<lambda>"; }
};

// A built-in native function
struct NativeFunction : Callable {
    std::string funcName;
    int numArgs;  // -1 = variadic
    std::function<Value(const std::vector<Value>&)> fn;

    Value call(Interpreter&, const std::vector<Value>& args) override { return fn(args); }
    int arity() const override { return numArgs; }
    std::string name() const override { return funcName; }
};

// A method bound to an instance
struct PraiaMethod : Callable {
    std::string methodName;
    std::vector<std::string> params;
    const ClassMethod* decl;  // points into the ClassStmt AST
    std::shared_ptr<Environment> closure;
    std::shared_ptr<PraiaInstance> instance;
    std::shared_ptr<PraiaClass> definingClass;  // class where this method is defined

    Value call(Interpreter& interp, const std::vector<Value>& args) override;
    int arity() const override { return static_cast<int>(params.size()); }
    std::string name() const override { return methodName; }
};

// A class (callable to create instances)
struct PraiaClass : Callable, std::enable_shared_from_this<PraiaClass> {
    std::string className;
    std::shared_ptr<PraiaClass> superclass;
    std::unordered_map<std::string, const ClassMethod*> methods;  // name -> AST node
    std::shared_ptr<Environment> closure;

    Value call(Interpreter& interp, const std::vector<Value>& args) override;
    int arity() const override;
    std::string name() const override { return className; }

    const ClassMethod* findMethod(const std::string& name) const;
};

// Thrown by sys.exit() to terminate cleanly
struct ExitSignal {
    int code;
};

// Thrown by export statements to deliver exports back to the importer
struct ExportSignal {
    std::shared_ptr<PraiaMap> exports;
};

// Stack trace support
struct CallFrame {
    std::string name;
    int line;
};

class CallFrameGuard {
    std::vector<CallFrame>& stack_;
public:
    CallFrameGuard(std::vector<CallFrame>& s, const std::string& name, int line)
        : stack_(s) { stack_.push_back({name, line}); }
    ~CallFrameGuard() { if (!stack_.empty()) stack_.pop_back(); }
    CallFrameGuard(const CallFrameGuard&) = delete;
    CallFrameGuard& operator=(const CallFrameGuard&) = delete;
};

class Interpreter {
    friend struct PraiaLambda;
    friend struct PraiaMethod;
public:
    Interpreter();
    void interpret(const std::vector<StmtPtr>& program);
    void interpretRepl(const std::vector<StmtPtr>& program);
    void setArgs(const std::vector<std::string>& args);
    void setCurrentFile(const std::string& path);

    // Public so PraiaFunction::call can use it
    void executeBlock(const BlockStmt* block, std::shared_ptr<Environment> env);

private:
    Value evaluate(const Expr* expr);
    void execute(const Stmt* stmt);

    // Grain loading
    Value loadGrain(const std::string& path, int line);
    std::string resolveGrainPath(const std::string& path, int line);

    std::shared_ptr<Environment> globals;
    std::shared_ptr<Environment> env;  // current scope
    std::shared_ptr<PraiaMap> sysMap;

    // Module system state
    std::string currentFile;                              // path of currently executing file
    std::unordered_map<std::string, Value> grainCache;    // resolved path -> cached exports
    std::set<std::string> importedInCurrentFile;          // tracks per-file duplicate imports

    // AST storage to keep grain ASTs alive (function bodies are raw pointers)
    std::vector<std::vector<StmtPtr>> grainAsts;

    // Thread safety for async/await
    std::recursive_mutex interpMutex;

    // Call stack for error traces
public:
    std::vector<CallFrame> callStack;
    std::string formatStackTrace() const;
};
