#pragma once

#include "../value.h"
#include "chunk.h"
#include "compiler.h"
#include <memory>
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

    // Closures created during execution (for cleanup)
    std::vector<ObjClosure*> allClosures;
    std::vector<ObjUpvalue*> allUpvalues;

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
