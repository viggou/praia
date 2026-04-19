#include "vm.h"
#include "../builtins.h"
#include "../interpreter.h"
#include "../lexer.h"
#include "../parser.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

// VMClosureCallable::call — allows native functions (filter, map, etc.)
// to call VM closures through the tree-walker's Callable interface
Value VMClosureCallable::call(Interpreter&, const std::vector<Value>& args) {
    if (!vm) return Value();

    int savedFrameCount = vm->frameCount;

    // Push a placeholder for the closure slot, then args
    vm->push(Value()); // slot for the closure itself
    for (auto& arg : args) vm->push(arg);

    if (!vm->callClosure(closure, static_cast<int>(args.size()), 0)) return Value();

    // Execute until this closure's frame is done
    auto result = vm->execute(savedFrameCount);
    if (result != VM::Result::OK) return Value();

    return vm->pop();
}

VM::VM() {}

VM::~VM() {
    for (auto* c : allClosures) delete c;
    for (auto* u : allUpvalues) delete u;
}

void VM::defineNative(const std::string& name, Value value) {
    globals[name] = std::move(value);
}

void VM::push(Value value) {
    if (stackTop >= STACK_MAX) {
        std::cerr << "Stack overflow" << std::endl;
        return;
    }
    stack[stackTop++] = std::move(value);
}

Value VM::pop() { return std::move(stack[--stackTop]); }
Value& VM::peek(int distance) { return stack[stackTop - 1 - distance]; }
void VM::resetStack() { stackTop = 0; frameCount = 0; }

uint8_t VM::readByte() { return *frames[frameCount - 1].ip++; }

uint16_t VM::readU16() {
    auto& frame = frames[frameCount - 1];
    uint16_t val = (frame.ip[0] << 8) | frame.ip[1];
    frame.ip += 2;
    return val;
}

Value VM::readConstant() {
    uint16_t idx = readU16();
    auto& frame = frames[frameCount - 1];
    return frame.chunk().constants[idx];
}

std::string VM::readString() { return readConstant().asString(); }

// ── Upvalue management ──────────────────────────────────────

ObjUpvalue* VM::captureUpvalue(Value* local) {
    ObjUpvalue* prevUpvalue = nullptr;
    ObjUpvalue* upvalue = openUpvalues;

    while (upvalue && upvalue->location > local) {
        prevUpvalue = upvalue;
        upvalue = upvalue->next;
    }

    if (upvalue && upvalue->location == local) return upvalue;

    auto* created = new ObjUpvalue(local);
    allUpvalues.push_back(created);
    created->next = upvalue;

    if (prevUpvalue) prevUpvalue->next = created;
    else openUpvalues = created;

    return created;
}

void VM::closeUpvalues(Value* last) {
    while (openUpvalues && openUpvalues->location >= last) {
        ObjUpvalue* upvalue = openUpvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        openUpvalues = upvalue->next;
    }
}

// ── Function calls ──────────────────────────────────────────

bool VM::callClosure(ObjClosure* closure, int argCount, int line) {
    auto& fn = closure->function;

    // Allow fewer args (defaults to nil) but not more
    if (argCount > fn->arity) {
        runtimeError(fn->name + "() expected at most " + std::to_string(fn->arity) +
            " argument(s) but got " + std::to_string(argCount), line);
        return false;
    }

    // Pad missing args with nil
    while (argCount < fn->arity) {
        push(Value());
        argCount++;
    }

    if (frameCount >= FRAMES_MAX) {
        runtimeError("Stack overflow (too many nested calls)", line);
        return false;
    }

    auto& frame = frames[frameCount++];
    frame.closure = closure;
    frame.function = nullptr;
    frame.ip = fn->chunk.code.data();
    frame.baseSlot = stackTop - argCount - 1; // -1 for the closure itself on stack
    return true;
}

bool VM::callValue(Value callee, int argCount, int line) {
    if (callee.isCallable()) {
        auto callable = callee.asCallable();

        // VM closure
        auto* vmClosure = dynamic_cast<VMClosureCallable*>(callable.get());
        if (vmClosure) {
            return callClosure(vmClosure->closure, argCount, line);
        }

        // Bound method (instance.method())
        auto* bound = dynamic_cast<VMBoundMethod*>(callable.get());
        if (bound) {
            // Replace the callee slot with the receiver (this)
            stack[stackTop - argCount - 1] = bound->receiver;
            return callClosure(bound->method, argCount, line);
        }

        // PraiaClass (instantiation)
        auto* klass = dynamic_cast<PraiaClass*>(callable.get());
        if (klass) {
            auto instance = std::make_shared<PraiaInstance>();
            instance->klass = std::dynamic_pointer_cast<PraiaClass>(callable);

            // Replace callee with the new instance
            stack[stackTop - argCount - 1] = Value(instance);

            // Call init if it exists
            auto it = klass->vmMethods.find("init");
            if (it != klass->vmMethods.end() && it->second.isCallable()) {
                auto* initVmcc = dynamic_cast<VMClosureCallable*>(it->second.asCallable().get());
                if (initVmcc) {
                    return callClosure(initVmcc->closure, argCount, line);
                }
            } else if (argCount > 0) {
                runtimeError(klass->className + "() takes no arguments (no init method)", line);
                return false;
            }
            return true;
        }

        // Native function
        auto* native = dynamic_cast<NativeFunction*>(callable.get());
        if (native) {
            int arity = native->arity();
            if (arity != -1 && argCount != arity) {
                runtimeError(native->name() + "() expected " + std::to_string(arity) +
                    " argument(s) but got " + std::to_string(argCount), line);
                return false;
            }
            std::vector<Value> args(argCount);
            for (int i = argCount - 1; i >= 0; i--) args[i] = pop();
            pop(); // the callable
            try {
                Value result = native->fn(args);
                push(std::move(result));
            } catch (const ExitSignal&) {
                throw; // propagate sys.exit() to main
            } catch (const RuntimeError& err) {
                if (tryHandleError(Value(std::string(err.what())))) return true;
                runtimeError(err.what(), err.line > 0 ? err.line : line);
                return false;
            }
            return true;
        }
    }

    runtimeError("Can only call functions", line);
    return false;
}

void VM::runtimeError(const std::string& msg, int line) {
    std::cerr << "[line " << line << "] Runtime error: " << msg << std::endl;
    std::cerr << formatStackTrace();
    resetStack();
}

bool VM::tryHandleError(Value error) {
    if (!exceptionHandlers.empty()) {
        auto handler = exceptionHandlers.back();
        exceptionHandlers.pop_back();

        while (frameCount - 1 > handler.frameIndex) {
            closeUpvalues(&stack[frames[frameCount - 1].baseSlot]);
            frameCount--;
        }

        stackTop = handler.stackTop;
        push(error);
        frames[frameCount - 1].ip = handler.catchIp;
        return true; // caught
    }
    return false; // uncaught
}

std::string VM::formatStackTrace() const {
    std::string trace;
    for (int i = frameCount - 1; i >= 0; i--) {
        auto& frame = frames[i];
        int offset = static_cast<int>(frame.ip - frame.chunk().code.data());
        int line = frame.chunk().getLine(offset);
        trace += "  at " + frame.name() + "() line " + std::to_string(line) + "\n";
    }
    return trace;
}

// ── Module loading ──────────────────────────────────────────

std::string VM::resolveGrainPath(const std::string& path, int line) {
    // Same logic as tree-walker (interpreter.cpp)
    if (path.rfind("./", 0) == 0 || path.rfind("../", 0) == 0) {
        std::string base = currentFile.empty() ? fs::current_path().string()
                                                : fs::path(currentFile).parent_path().string();
        std::string resolved = (fs::path(base) / (path + ".praia")).string();
        if (fs::exists(resolved)) return fs::canonical(resolved).string();
        runtimeError("Grain not found: " + path, line);
        return "";
    }

    if (!currentFile.empty()) {
        fs::path dir = fs::path(currentFile).parent_path();
        for (int i = 0; i < 10; i++) {
            auto candidate = dir / "grains" / (path + ".praia");
            if (fs::exists(candidate)) return fs::canonical(candidate).string();
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
    }

    {
        auto candidate = fs::current_path() / "grains" / (path + ".praia");
        if (fs::exists(candidate)) return fs::canonical(candidate).string();
    }

    {
        const char* home = std::getenv("HOME");
        if (home) {
            auto candidate = fs::path(home) / ".praia" / "grains" / (path + ".praia");
            if (fs::exists(candidate)) return fs::canonical(candidate).string();
        }
    }

    runtimeError("Grain not found: " + path, line);
    return "";
}

Value VM::loadGrain(const std::string& importPath, int line) {
    std::string resolved = resolveGrainPath(importPath, line);
    if (resolved.empty()) return Value();

    if (importedInCurrentFile.count(resolved)) {
        runtimeError("Grain '" + importPath + "' is already imported in this file", line);
        return Value();
    }
    importedInCurrentFile.insert(resolved);

    auto cached = grainCache.find(resolved);
    if (cached != grainCache.end()) return cached->second;

    // Read source
    std::ifstream f(resolved);
    if (!f.is_open()) { runtimeError("Cannot read grain: " + resolved, line); return Value(); }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string source = ss.str();

    // Lex + parse
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    if (lexer.hasError()) { runtimeError("Syntax error in grain: " + importPath, line); return Value(); }

    Parser parser(tokens);
    auto program = parser.parse();
    if (parser.hasError()) { runtimeError("Parse error in grain: " + importPath, line); return Value(); }

    // Compile
    Compiler compiler;
    auto script = compiler.compile(program);
    if (!script) { runtimeError("Compile error in grain: " + importPath, line); return Value(); }

    // Execute in a fresh VM with shared globals for builtins
    VM grainVm;
    // Copy builtins (but not user globals)
    grainVm.globals = globals; // copy all globals (includes builtins)
    grainVm.currentFile = resolved;

    auto* grainClosure = new ObjClosure(script);
    grainVm.allClosures.push_back(grainClosure);

    auto wrapper = std::make_shared<VMClosureCallable>(grainClosure);
    grainVm.push(Value(std::static_pointer_cast<Callable>(wrapper)));

    grainVm.frames[0].closure = grainClosure;
    grainVm.frames[0].function = script;
    grainVm.frames[0].ip = script->chunk.code.data();
    grainVm.frames[0].baseSlot = 0;
    grainVm.frameCount = 1;

    auto result = grainVm.execute();

    // The grain should have ended with OP_EXPORT which pushes an exports map
    // and returns. Check if there's a result.
    Value exports;
    if (result == Result::OK && grainVm.stackTop > 0) {
        exports = grainVm.stack[grainVm.stackTop - 1];
    } else {
        exports = Value(std::make_shared<PraiaMap>()); // empty exports
    }

    // Keep the grain's ASTs, closures, and upvalues alive
    grainAsts.push_back(std::move(program));

    // Transfer ownership of closures and upvalues to the parent VM
    for (auto* c : grainVm.allClosures) allClosures.push_back(c);
    grainVm.allClosures.clear();
    for (auto* u : grainVm.allUpvalues) allUpvalues.push_back(u);
    grainVm.allUpvalues.clear();

    // Copy grain's globals to parent VM so exported closures can access
    // their module-level variables (which were compiled as globals)
    for (auto& [k, v] : grainVm.globals) {
        // Don't overwrite existing builtins or user globals
        if (globals.find(k) == globals.end()) {
            globals[k] = v;
        }
    }

    grainCache[resolved] = exports;
    return exports;
}

VM::Result VM::run(std::shared_ptr<CompiledFunction> script) {
    // Create a closure for the top-level script
    auto* scriptClosure = new ObjClosure(script);
    allClosures.push_back(scriptClosure);

    auto wrapper = std::make_shared<VMClosureCallable>(scriptClosure);
    wrapper->vm = this;
    push(Value(std::static_pointer_cast<Callable>(wrapper)));

    frames[0].closure = scriptClosure;
    frames[0].function = script;
    frames[0].ip = script->chunk.code.data();
    frames[0].baseSlot = 0;
    frameCount = 1;

    return execute();
}

VM::Result VM::execute(int baseFrameCount_) {
    #define FRAME (frames[frameCount - 1])
    #define READ_BYTE() (*FRAME.ip++)
    #define READ_U16() (FRAME.ip += 2, static_cast<uint16_t>((FRAME.ip[-2] << 8) | FRAME.ip[-1]))
    #define READ_CONSTANT() (FRAME.chunk().constants[READ_U16()])
    #define READ_STRING() (READ_CONSTANT().asString())
    #define CURRENT_LINE() (FRAME.chunk().getLine(static_cast<int>(FRAME.ip - FRAME.chunk().code.data())))
    #define RUNTIME_ERR(msg) do { \
        std::string _msg = (msg); int _line = CURRENT_LINE(); \
        if (tryHandleError(Value(_msg))) break; \
        runtimeError(_msg, _line); return Result::RUNTIME_ERROR; \
    } while(0)

    for (;;) {
        uint8_t instruction = READ_BYTE();

        switch (static_cast<OpCode>(instruction)) {

        case OpCode::OP_CONSTANT: push(READ_CONSTANT()); break;
        case OpCode::OP_NIL: push(Value()); break;
        case OpCode::OP_TRUE: push(Value(true)); break;
        case OpCode::OP_FALSE: push(Value(false)); break;
        case OpCode::OP_POP: pop(); break;
        case OpCode::OP_POPN: { uint8_t n = READ_BYTE(); stackTop -= n; break; }

        // ── Arithmetic ──
        case OpCode::OP_ADD: {
            Value b = pop(), a = pop();
            if (a.isInt() && b.isInt()) { push(Value(a.asInt() + b.asInt())); break; }
            if (a.isNumber() && b.isNumber()) { push(Value(a.asNumber() + b.asNumber())); break; }
            if (a.isArray() && b.isArray()) {
                auto r = std::make_shared<PraiaArray>();
                for (auto& el : a.asArray()->elements) r->elements.push_back(el);
                for (auto& el : b.asArray()->elements) r->elements.push_back(el);
                push(Value(r)); break;
            }
            if (a.isMap() && b.isMap()) {
                auto r = std::make_shared<PraiaMap>();
                for (auto& [k, v] : a.asMap()->entries) r->entries[k] = v;
                for (auto& [k, v] : b.asMap()->entries) r->entries[k] = v; // b overrides a
                push(Value(r)); break;
            }
            if (a.isString() || b.isString()) { push(Value(a.toString() + b.toString())); break; }
            RUNTIME_ERR("Operands of '+' must be numbers, strings, or arrays");
        }
        case OpCode::OP_SUBTRACT: {
            Value b = pop(), a = pop();
            if (a.isInt() && b.isInt()) { push(Value(a.asInt() - b.asInt())); break; }
            if (a.isNumber() && b.isNumber()) { push(Value(a.asNumber() - b.asNumber())); break; }
            RUNTIME_ERR("Operands of '-' must be numbers");
        }
        case OpCode::OP_MULTIPLY: {
            Value b = pop(), a = pop();
            if (a.isInt() && b.isInt()) { push(Value(a.asInt() * b.asInt())); break; }
            if (a.isNumber() && b.isNumber()) { push(Value(a.asNumber() * b.asNumber())); break; }
            RUNTIME_ERR("Operands of '*' must be numbers");
        }
        case OpCode::OP_DIVIDE: {
            Value b = pop(), a = pop();
            if (a.isNumber() && b.isNumber()) {
                if (b.asNumber() == 0) { RUNTIME_ERR("Division by zero"); }
                push(Value(a.asNumber() / b.asNumber())); break;
            }
            RUNTIME_ERR("Operands of '/' must be numbers");
        }
        case OpCode::OP_MODULO: {
            Value b = pop(), a = pop();
            if (a.isInt() && b.isInt()) {
                if (b.asInt() == 0) { RUNTIME_ERR("Modulo by zero"); }
                push(Value(a.asInt() % b.asInt())); break;
            }
            if (a.isNumber() && b.isNumber()) {
                if (b.asNumber() == 0) { RUNTIME_ERR("Modulo by zero"); }
                push(Value(std::fmod(a.asNumber(), b.asNumber()))); break;
            }
            RUNTIME_ERR("Operands of '%' must be numbers");
        }
        case OpCode::OP_NEGATE: {
            if (!peek().isNumber()) { RUNTIME_ERR("Operand of '-' must be a number"); }
            if (peek().isInt()) push(Value(-pop().asInt()));
            else push(Value(-pop().asNumber()));
            break;
        }

        // ── Bitwise ──
        case OpCode::OP_BIT_AND: { Value b=pop(),a=pop(); push(Value(static_cast<int64_t>(a.asNumber())&static_cast<int64_t>(b.asNumber()))); break; }
        case OpCode::OP_BIT_OR:  { Value b=pop(),a=pop(); push(Value(static_cast<int64_t>(a.asNumber())|static_cast<int64_t>(b.asNumber()))); break; }
        case OpCode::OP_BIT_XOR: { Value b=pop(),a=pop(); push(Value(static_cast<int64_t>(a.asNumber())^static_cast<int64_t>(b.asNumber()))); break; }
        case OpCode::OP_BIT_NOT: { push(Value(~static_cast<int64_t>(pop().asNumber()))); break; }
        case OpCode::OP_SHL: { Value b=pop(),a=pop(); push(Value(static_cast<int64_t>(a.asNumber())<<static_cast<int64_t>(b.asNumber()))); break; }
        case OpCode::OP_SHR: { Value b=pop(),a=pop(); push(Value(static_cast<int64_t>(a.asNumber())>>static_cast<int64_t>(b.asNumber()))); break; }

        // ── Comparison ──
        case OpCode::OP_EQUAL:         { Value b=pop(),a=pop(); push(Value(a==b)); break; }
        case OpCode::OP_NOT_EQUAL:     { Value b=pop(),a=pop(); push(Value(a!=b)); break; }
        case OpCode::OP_LESS:          { Value b=pop(),a=pop(); push(Value(a.asNumber()<b.asNumber())); break; }
        case OpCode::OP_GREATER:       { Value b=pop(),a=pop(); push(Value(a.asNumber()>b.asNumber())); break; }
        case OpCode::OP_LESS_EQUAL:    { Value b=pop(),a=pop(); push(Value(a.asNumber()<=b.asNumber())); break; }
        case OpCode::OP_GREATER_EQUAL: { Value b=pop(),a=pop(); push(Value(a.asNumber()>=b.asNumber())); break; }
        case OpCode::OP_NOT: { push(Value(!pop().isTruthy())); break; }

        // ── Variables ──
        case OpCode::OP_DEFINE_GLOBAL: { std::string n = READ_STRING(); globals[n] = pop(); break; }
        case OpCode::OP_GET_GLOBAL: {
            std::string n = READ_STRING();
            auto it = globals.find(n);
            if (it == globals.end()) { RUNTIME_ERR("Undefined variable '" + n + "'"); }
            push(it->second); break;
        }
        case OpCode::OP_SET_GLOBAL: {
            std::string n = READ_STRING();
            auto it = globals.find(n);
            if (it == globals.end()) { RUNTIME_ERR("Undefined variable '" + n + "'"); }
            it->second = peek(); break;
        }
        case OpCode::OP_GET_LOCAL: { uint16_t slot = READ_U16(); push(stack[FRAME.baseSlot + slot]); break; }
        case OpCode::OP_SET_LOCAL: { uint16_t slot = READ_U16(); stack[FRAME.baseSlot + slot] = peek(); break; }

        // ── Postfix ──
        case OpCode::OP_POST_INC_LOCAL: {
            uint16_t slot = READ_U16();
            Value& val = stack[FRAME.baseSlot + slot];
            push(val);
            if (val.isInt()) val = Value(val.asInt() + 1); else val = Value(val.asNumber() + 1);
            break;
        }
        case OpCode::OP_POST_DEC_LOCAL: {
            uint16_t slot = READ_U16();
            Value& val = stack[FRAME.baseSlot + slot];
            push(val);
            if (val.isInt()) val = Value(val.asInt() - 1); else val = Value(val.asNumber() - 1);
            break;
        }
        case OpCode::OP_POST_INC_GLOBAL:
        case OpCode::OP_POST_DEC_GLOBAL: {
            std::string n = READ_STRING();
            auto it = globals.find(n);
            if (it == globals.end()) { RUNTIME_ERR("Undefined variable '" + n + "'"); }
            push(it->second);
            bool inc = static_cast<OpCode>(instruction) == OpCode::OP_POST_INC_GLOBAL;
            if (it->second.isInt()) it->second = Value(it->second.asInt() + (inc ? 1 : -1));
            else it->second = Value(it->second.asNumber() + (inc ? 1 : -1));
            break;
        }

        // ── Control flow ──
        case OpCode::OP_JUMP: { uint16_t off = READ_U16(); FRAME.ip += off; break; }
        case OpCode::OP_JUMP_BACK: { uint16_t off = READ_U16(); FRAME.ip -= off; break; }
        case OpCode::OP_JUMP_IF_FALSE: { uint16_t off = READ_U16(); if (!peek().isTruthy()) FRAME.ip += off; break; }
        case OpCode::OP_JUMP_IF_TRUE: { uint16_t off = READ_U16(); if (peek().isTruthy()) FRAME.ip += off; break; }
        case OpCode::OP_POP_JUMP_IF_FALSE: { uint16_t off = READ_U16(); if (!pop().isTruthy()) FRAME.ip += off; break; }

        // ── Functions ──
        case OpCode::OP_CALL: {
            uint8_t argc = READ_BYTE();
            Value callee = peek(argc);
            if (!callValue(callee, argc, CURRENT_LINE())) return Result::RUNTIME_ERROR;
            break;
        }

        case OpCode::OP_RETURN: {
            Value result = pop();
            int returnBase = FRAME.baseSlot; // save callee's base before popping frame
            closeUpvalues(&stack[returnBase]);
            frameCount--;
            if (frameCount <= baseFrameCount_) {
                if (frameCount == 0) pop();
                stackTop = returnBase;
                push(std::move(result));
                return Result::OK;
            }
            stackTop = returnBase;
            push(std::move(result));
            break;
        }

        case OpCode::OP_CLOSURE: {
            auto fnVal = READ_CONSTANT();
            // The constant should be a CompiledFunction wrapped in a Value
            // We stored it as a Callable (VMClosureCallable) during compilation... no.
            // Actually, the compiler stores the CompiledFunction directly.
            // We need to extract it. Let's use a convention: closures store
            // the CompiledFunction as a special map with __fn field, or we
            // add a helper. Simplest: store CompiledFunction as a Callable wrapper.

            // For now: the compiler emits the function index, the constant is a
            // string marker. Let me redesign: the compiler should store the
            // CompiledFunction ptr directly in the constant pool.
            // Since Value can't hold CompiledFunction, I'll use a side channel.

            // Alternative approach: the compiler creates the closure value at compile time
            // and stores it as a Callable in the constant pool. The OP_CLOSURE opcode
            // just reads it and sets up upvalues.

            // Let me use the simplest working approach:
            // The compiler stores a VMClosureCallable in the constant pool.
            // OP_CLOSURE reads it, creates a new ObjClosure with upvalues.

            if (!fnVal.isCallable()) {
                RUNTIME_ERR("Internal error: OP_CLOSURE constant is not a function");
            }
            auto* vmcc = dynamic_cast<VMClosureCallable*>(fnVal.asCallable().get());
            if (!vmcc) {
                RUNTIME_ERR("Internal error: OP_CLOSURE constant is not a VM closure");
            }

            auto* closure = new ObjClosure(vmcc->closure->function);
            allClosures.push_back(closure);

            // Read upvalue descriptors
            for (int i = 0; i < closure->upvalueCount; i++) {
                uint8_t isLocal = READ_BYTE();
                uint16_t index = READ_U16();
                if (isLocal) {
                    closure->upvalues[i] = captureUpvalue(&stack[FRAME.baseSlot + index]);
                } else {
                    closure->upvalues[i] = FRAME.closure->upvalues[index];
                }
            }

            auto wrapper = std::make_shared<VMClosureCallable>(closure);
            wrapper->vm = this;
            push(Value(std::static_pointer_cast<Callable>(wrapper)));
            break;
        }

        case OpCode::OP_GET_UPVALUE: {
            uint8_t slot = READ_BYTE();
            push(*FRAME.closure->upvalues[slot]->location);
            break;
        }

        case OpCode::OP_SET_UPVALUE: {
            uint8_t slot = READ_BYTE();
            *FRAME.closure->upvalues[slot]->location = peek();
            break;
        }

        case OpCode::OP_CLOSE_UPVALUE: {
            closeUpvalues(&stack[stackTop - 1]);
            pop();
            break;
        }

        // ── Classes ──
        case OpCode::OP_CLASS: {
            std::string name = READ_STRING();
            auto klass = std::make_shared<PraiaClass>();
            klass->className = name;
            push(Value(std::static_pointer_cast<Callable>(klass)));
            break;
        }

        case OpCode::OP_METHOD: {
            std::string name = READ_STRING();
            Value method = pop(); // the closure
            Value& klass = peek(); // the class

            auto klassPtr = std::dynamic_pointer_cast<PraiaClass>(klass.asCallable());
            if (!klassPtr) { RUNTIME_ERR("OP_METHOD: not a class"); }

            // Store the closure as a method — the VM will bind it when accessed
            klassPtr->vmMethods[name] = method;
            break;
        }

        case OpCode::OP_INHERIT: {
            Value subclass = pop();
            Value superclass = peek(); // leave superclass on stack? no, pop both

            auto superPtr = std::dynamic_pointer_cast<PraiaClass>(superclass.asCallable());
            auto subPtr = std::dynamic_pointer_cast<PraiaClass>(subclass.asCallable());

            if (!superPtr) { RUNTIME_ERR("Superclass must be a class"); }
            if (!subPtr) { RUNTIME_ERR("Subclass must be a class"); }

            subPtr->superclass = superPtr;

            // Copy superclass methods (they can be overridden later by OP_METHOD)
            for (auto& [k, v] : superPtr->vmMethods) {
                if (subPtr->vmMethods.find(k) == subPtr->vmMethods.end()) {
                    subPtr->vmMethods[k] = v;
                }
            }

            pop(); // pop superclass
            break;
        }

        case OpCode::OP_GET_PROPERTY: {
            std::string name = READ_STRING();
            Value obj = pop();

            if (obj.isInstance()) {
                auto inst = obj.asInstance();
                // Fields first
                auto fit = inst->fields.find(name);
                if (fit != inst->fields.end()) { push(fit->second); break; }

                // Then methods — bind to instance
                auto it = inst->klass->vmMethods.find(name);
                if (it == inst->klass->vmMethods.end()) {
                    // Walk superclass chain
                    auto walk = inst->klass->superclass;
                    while (walk) {
                        auto sit = walk->vmMethods.find(name);
                        if (sit != walk->vmMethods.end()) { it = sit; break; }
                        walk = walk->superclass;
                    }
                }
                if (it != inst->klass->vmMethods.end()) {
                    // Bind: create a new closure-like value with "this" bound
                    // For simplicity, store instance+method as a bound method callable
                    auto boundMethod = it->second;
                    if (boundMethod.isCallable()) {
                        auto* vmcc = dynamic_cast<VMClosureCallable*>(boundMethod.asCallable().get());
                        if (vmcc) {
                            // Create a bound method: when called, slot 0 = this
                            auto bm = std::make_shared<VMBoundMethod>(obj, vmcc->closure);
                            push(Value(std::static_pointer_cast<Callable>(bm)));
                            break;
                        }
                    }
                    push(boundMethod);
                    break;
                }

                RUNTIME_ERR("Instance has no property '" + name + "'");
            }

            if (obj.isMap()) {
                auto& entries = obj.asMap()->entries;
                auto it = entries.find(name);
                if (it != entries.end()) { push(it->second); break; }
                RUNTIME_ERR("Map has no field '" + name + "'");
            }

            // String/array methods
            if (obj.isString()) {
                try {
                    push(getStringMethod(obj.asString(), name, CURRENT_LINE()));
                } catch (const RuntimeError& err) {
                    if (tryHandleError(Value(std::string(err.what())))) break;
                    runtimeError(err.what(), CURRENT_LINE());
                    return Result::RUNTIME_ERROR;
                }
                break;
            }
            if (obj.isArray()) {
                try {
                    push(getArrayMethod(obj.asArray(), name, CURRENT_LINE(), nullptr));
                } catch (const RuntimeError& err) {
                    if (tryHandleError(Value(std::string(err.what())))) break;
                    runtimeError(err.what(), CURRENT_LINE());
                    return Result::RUNTIME_ERROR;
                }
                break;
            }

            RUNTIME_ERR("Cannot access property '" + name + "' on this type");
        }

        case OpCode::OP_SET_PROPERTY: {
            std::string name = READ_STRING();
            Value val = pop();
            Value obj = pop();

            if (obj.isInstance()) {
                obj.asInstance()->fields[name] = val;
                push(val);
                break;
            }
            if (obj.isMap()) {
                obj.asMap()->entries[name] = val;
                push(val);
                break;
            }

            RUNTIME_ERR("Can only set properties on instances and maps");
        }

        case OpCode::OP_GET_THIS: {
            push(stack[FRAME.baseSlot]);
            break;
        }

        case OpCode::OP_GET_SUPER: {
            std::string name = READ_STRING();
            Value instance = pop();

            if (!instance.isInstance()) {
                RUNTIME_ERR("'super' used outside of a method");
            }
            auto inst = instance.asInstance();
            auto super = inst->klass->superclass;
            if (!super) { RUNTIME_ERR("Class has no superclass"); }

            auto it = super->vmMethods.find(name);
            if (it == super->vmMethods.end()) {
                RUNTIME_ERR("Superclass has no method '" + name + "'");
            }

            auto* vmcc = dynamic_cast<VMClosureCallable*>(it->second.asCallable().get());
            if (vmcc) {
                auto bm = std::make_shared<VMBoundMethod>(instance, vmcc->closure);
                push(Value(std::static_pointer_cast<Callable>(bm)));
            } else {
                push(it->second);
            }
            break;
        }

        case OpCode::OP_INVOKE:
        case OpCode::OP_SUPER_INVOKE: {
            // Not emitted by the compiler (uses GET_PROPERTY + CALL instead)
            // Reserved for future optimization
            RUNTIME_ERR("OP_INVOKE not yet implemented");
        }

        case OpCode::OP_BUILD_ARRAY: {
            uint16_t count = READ_U16();
            auto arr = std::make_shared<PraiaArray>();
            if (count == 0xFFFF) {
                // Dynamic count (with spreads) — not yet supported, use fixed count
                // For now, this shouldn't happen since we fall back to fixed count
                RUNTIME_ERR("Dynamic array builds not yet supported in VM");
            }
            arr->elements.resize(count);
            for (int i = count - 1; i >= 0; i--) arr->elements[i] = pop();
            push(Value(arr));
            break;
        }

        case OpCode::OP_BUILD_MAP: {
            uint16_t count = READ_U16();
            auto map = std::make_shared<PraiaMap>();
            // Stack has count pairs: key, value, key, value, ...
            // Pop in reverse
            std::vector<std::pair<std::string, Value>> pairs(count);
            for (int i = count - 1; i >= 0; i--) {
                Value val = pop();
                Value key = pop();
                pairs[i] = {key.asString(), std::move(val)};
            }
            for (auto& [k, v] : pairs) map->entries[k] = std::move(v);
            push(Value(map));
            break;
        }

        case OpCode::OP_INDEX_GET: {
            Value idx = pop();
            Value obj = pop();
            if (obj.isArray()) {
                if (!idx.isNumber()) { RUNTIME_ERR("Array index must be a number"); }
                auto& elems = obj.asArray()->elements;
                int i = static_cast<int>(idx.asNumber());
                if (i < 0) i += static_cast<int>(elems.size());
                if (i < 0 || i >= static_cast<int>(elems.size())) { RUNTIME_ERR("Array index out of bounds"); }
                push(elems[i]);
            } else if (obj.isString()) {
                if (!idx.isNumber()) { RUNTIME_ERR("String index must be a number"); }
                auto& str = obj.asString();
                int i = static_cast<int>(idx.asNumber());
                if (i < 0) i += static_cast<int>(str.size());
                if (i < 0 || i >= static_cast<int>(str.size())) { RUNTIME_ERR("String index out of bounds"); }
                push(Value(std::string(1, str[i])));
            } else if (obj.isMap()) {
                if (!idx.isString()) { RUNTIME_ERR("Map key must be a string"); }
                auto& entries = obj.asMap()->entries;
                auto it = entries.find(idx.asString());
                if (it == entries.end()) { RUNTIME_ERR("Map has no key '" + idx.asString() + "'"); }
                push(it->second);
            } else {
                RUNTIME_ERR("Can only index into arrays, strings, and maps");
            }
            break;
        }

        case OpCode::OP_INDEX_SET: {
            Value val = pop();
            Value idx = pop();
            Value obj = pop();
            if (obj.isArray()) {
                auto& elems = obj.asArray()->elements;
                int i = static_cast<int>(idx.asNumber());
                if (i < 0) i += static_cast<int>(elems.size());
                if (i < 0 || i >= static_cast<int>(elems.size())) { RUNTIME_ERR("Array index out of bounds"); }
                elems[i] = val;
                push(val);
            } else if (obj.isMap()) {
                obj.asMap()->entries[idx.asString()] = val;
                push(val);
            } else {
                RUNTIME_ERR("Can only assign to array or map indices");
            }
            break;
        }

        case OpCode::OP_UNPACK_SPREAD: {
            // Not yet fully implemented for VM
            RUNTIME_ERR("Spread in VM not yet supported");
        }

        case OpCode::OP_BUILD_STRING: {
            uint16_t count = READ_U16();
            std::string result;
            // Collect all parts in order
            std::vector<Value> parts(count);
            for (int i = count - 1; i >= 0; i--) parts[i] = pop();
            for (auto& p : parts) result += p.toString();
            push(Value(std::move(result)));
            break;
        }
        case OpCode::OP_TRY_BEGIN: {
            uint16_t catchOffset = READ_U16();
            ExceptionHandler handler;
            handler.catchIp = FRAME.ip + catchOffset;
            handler.frameIndex = frameCount - 1;
            handler.stackTop = stackTop;
            exceptionHandlers.push_back(handler);
            break;
        }

        case OpCode::OP_TRY_END: {
            if (!exceptionHandlers.empty()) {
                exceptionHandlers.pop_back();
            }
            break;
        }

        case OpCode::OP_THROW: {
            Value error = pop();

            if (!exceptionHandlers.empty()) {
                auto handler = exceptionHandlers.back();
                exceptionHandlers.pop_back();

                // Unwind call frames back to the handler's frame
                while (frameCount - 1 > handler.frameIndex) {
                    closeUpvalues(&stack[FRAME.baseSlot]);
                    frameCount--;
                }

                // Restore stack and jump to catch
                stackTop = handler.stackTop;
                push(error); // push error value for catch block
                FRAME.ip = handler.catchIp;
                break; // continue execution from catch
            }

            // No handler — uncaught error
            int line = CURRENT_LINE();
            std::cerr << "[line " << line << "] Uncaught error: " << error.toString() << std::endl;
            std::cerr << formatStackTrace();
            resetStack();
            return Result::RUNTIME_ERROR;
        }
        case OpCode::OP_IMPORT: {
            std::string path = READ_STRING();
            std::string alias = READ_STRING();
            (void)alias; // alias is handled by the compiler (defines the variable)
            Value exports = loadGrain(path, CURRENT_LINE());
            if (exports.isNil() && grainCache.find(path) == grainCache.end()) {
                return Result::RUNTIME_ERROR;
            }
            push(exports);
            break;
        }

        case OpCode::OP_EXPORT: {
            READ_BYTE(); // unused count byte
            // The exports map is on top of the stack.
            // For a grain, this is the return value. Just return from execution.
            {
                Value result = pop();
                int returnBase = FRAME.baseSlot;
                closeUpvalues(&stack[returnBase]);
                frameCount--;
                if (frameCount == 0) {
                    push(std::move(result));
                    return Result::OK;
                }
                stackTop = returnBase;
                push(std::move(result));
            }
            break;
        }

        case OpCode::OP_ASYNC: {
            uint8_t argc = READ_BYTE();
            Value callee = peek(argc);

            if (!callee.isCallable()) {
                RUNTIME_ERR("async requires a callable");
            }

            auto callable = callee.asCallable();

            // Collect args
            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; i--) args[i] = pop();
            pop(); // callee

            // Spawn in background thread — native functions run truly in parallel
            auto* native = dynamic_cast<NativeFunction*>(callable.get());
            auto sharedFuture = std::async(std::launch::async,
                [callable, args, native]() -> Value {
                    if (native) {
                        return native->fn(args);
                    }
                    // For non-native: can't easily run Praia closures in threads
                    // without a second VM. For now, just call synchronously.
                    // (This matches the tree-walker's GIL behavior for Praia functions)
                    return Value(); // placeholder
                }).share();

            auto fut = std::make_shared<PraiaFuture>();
            fut->future = sharedFuture;
            push(Value(fut));
            break;
        }

        case OpCode::OP_AWAIT: {
            Value val = pop();
            if (!val.isFuture()) {
                RUNTIME_ERR("Can only await a future");
            }
            try {
                Value result = val.asFuture()->future.get();
                push(std::move(result));
            } catch (const RuntimeError& err) {
                RUNTIME_ERR(err.what());
            } catch (...) {
                RUNTIME_ERR("Async task failed");
            }
            break;
        }
        }
    }

    #undef FRAME
    #undef READ_BYTE
    #undef READ_U16
    #undef READ_CONSTANT
    #undef READ_STRING
    #undef CURRENT_LINE
}
