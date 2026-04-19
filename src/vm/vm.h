#pragma once

#include "../value.h"
#include "chunk.h"
#include "compiler.h"
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Upvalue: heap-allocated box for captured variables
struct ObjUpvalue {
    Value* location;      // points into the stack while variable is live
    Value closed;          // holds the value after the variable goes out of scope
    ObjUpvalue* next;      // linked list for open upvalues

    ObjUpvalue(Value* slot) : location(slot), next(nullptr) {}
};

// Closure: a compiled function + its captured upvalues
struct ObjClosure {
    std::shared_ptr<CompiledFunction> function;
    std::vector<ObjUpvalue*> upvalues;
    int upvalueCount;

    ObjClosure(std::shared_ptr<CompiledFunction> fn) {
        function = std::move(fn);
        upvalueCount = function->upvalueCount;
        upvalues.resize(upvalueCount, nullptr);
    }
};

// Wrapper to store ObjClosure as a Callable in Value
struct VMClosureCallable : Callable {
    ObjClosure* closure;
    VMClosureCallable(ObjClosure* c) : closure(c) {}
    Value call(Interpreter&, const std::vector<Value>&) override { return Value(); } // unused, VM handles directly
    int arity() const override { return closure->function->arity; }
    std::string name() const override { return closure->function->name; }
};

// A method bound to an instance — when called, slot 0 = this
struct VMBoundMethod : Callable {
    Value receiver;
    ObjClosure* method;
    VMBoundMethod(Value recv, ObjClosure* m) : receiver(std::move(recv)), method(m) {}
    Value call(Interpreter&, const std::vector<Value>&) override { return Value(); } // unused
    int arity() const override { return method->function->arity; }
    std::string name() const override { return method->function->name; }
};

struct VMCallFrame {
    ObjClosure* closure;   // the closure being executed (null for script w/o closure)
    std::shared_ptr<CompiledFunction> function; // fallback for script frame
    const uint8_t* ip;
    int baseSlot;

    Chunk& chunk() const {
        return closure ? closure->function->chunk : function->chunk;
    }
    const std::string& name() const {
        return closure ? closure->function->name : function->name;
    }
};

class VM {
public:
    VM();
    ~VM();

    enum class Result { OK, COMPILE_ERROR, RUNTIME_ERROR };

    Result run(std::shared_ptr<CompiledFunction> script);

    // Setup
    void defineNative(const std::string& name, Value value);
    void setCurrentFile(const std::string& path) { currentFile = path; }

private:
    Result execute();

    // Stack
    static constexpr int STACK_MAX = 16384;
    Value stack[STACK_MAX];
    int stackTop = 0;

    void push(Value value);
    Value pop();
    Value& peek(int distance = 0);
    void resetStack();

    // Call frames
    static constexpr int FRAMES_MAX = 256;
    VMCallFrame frames[FRAMES_MAX];
    int frameCount = 0;

    // Globals
    std::unordered_map<std::string, Value> globals;

    // Open upvalues (linked list, ordered by stack slot desc)
    ObjUpvalue* openUpvalues = nullptr;
    ObjUpvalue* captureUpvalue(Value* local);
    void closeUpvalues(Value* last);

    // Exception handlers
    struct ExceptionHandler {
        const uint8_t* catchIp;  // where to jump on exception
        int frameIndex;          // which call frame installed this
        int stackTop;            // stack depth to restore
    };
    std::vector<ExceptionHandler> exceptionHandlers;

    // Closures created during execution (for cleanup)
    std::vector<ObjClosure*> allClosures;
    std::vector<ObjUpvalue*> allUpvalues;

    // Module system
    std::string currentFile;
    std::unordered_map<std::string, Value> grainCache;
    std::set<std::string> importedInCurrentFile;
    std::vector<std::vector<StmtPtr>> grainAsts;

    Value loadGrain(const std::string& path, int line);
    std::string resolveGrainPath(const std::string& path, int line);

    // Helpers
    uint8_t readByte();
    uint16_t readU16();
    Value readConstant();
    std::string readString();
    bool callValue(Value callee, int argCount, int line);
    bool callClosure(ObjClosure* closure, int argCount, int line);

    void runtimeError(const std::string& msg, int line);
    std::string formatStackTrace() const;
};
