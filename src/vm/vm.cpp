#include "vm.h"
#include "../interpreter.h"
#include <cmath>
#include <iostream>

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
        // Check if it's a VM closure
        auto callable = callee.asCallable();
        auto* vmClosure = dynamic_cast<VMClosureCallable*>(callable.get());
        if (vmClosure) {
            return callClosure(vmClosure->closure, argCount, line);
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
            } catch (const RuntimeError& err) {
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

VM::Result VM::run(std::shared_ptr<CompiledFunction> script) {
    // Create a closure for the top-level script
    auto* scriptClosure = new ObjClosure(script);
    allClosures.push_back(scriptClosure);

    auto wrapper = std::make_shared<VMClosureCallable>(scriptClosure);
    push(Value(std::static_pointer_cast<Callable>(wrapper)));

    frames[0].closure = scriptClosure;
    frames[0].function = script;
    frames[0].ip = script->chunk.code.data();
    frames[0].baseSlot = 0;
    frameCount = 1;

    return execute();
}

VM::Result VM::execute() {
    #define FRAME (frames[frameCount - 1])
    #define READ_BYTE() (*FRAME.ip++)
    #define READ_U16() (FRAME.ip += 2, static_cast<uint16_t>((FRAME.ip[-2] << 8) | FRAME.ip[-1]))
    #define READ_CONSTANT() (FRAME.chunk().constants[READ_U16()])
    #define READ_STRING() (READ_CONSTANT().asString())
    #define CURRENT_LINE() (FRAME.chunk().getLine(static_cast<int>(FRAME.ip - FRAME.chunk().code.data())))

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
            if (a.isString() || b.isString()) { push(Value(a.toString() + b.toString())); break; }
            runtimeError("Operands of '+' must be numbers, strings, or arrays", CURRENT_LINE());
            return Result::RUNTIME_ERROR;
        }
        case OpCode::OP_SUBTRACT: {
            Value b = pop(), a = pop();
            if (a.isInt() && b.isInt()) { push(Value(a.asInt() - b.asInt())); break; }
            if (a.isNumber() && b.isNumber()) { push(Value(a.asNumber() - b.asNumber())); break; }
            runtimeError("Operands of '-' must be numbers", CURRENT_LINE()); return Result::RUNTIME_ERROR;
        }
        case OpCode::OP_MULTIPLY: {
            Value b = pop(), a = pop();
            if (a.isInt() && b.isInt()) { push(Value(a.asInt() * b.asInt())); break; }
            if (a.isNumber() && b.isNumber()) { push(Value(a.asNumber() * b.asNumber())); break; }
            runtimeError("Operands of '*' must be numbers", CURRENT_LINE()); return Result::RUNTIME_ERROR;
        }
        case OpCode::OP_DIVIDE: {
            Value b = pop(), a = pop();
            if (a.isNumber() && b.isNumber()) {
                if (b.asNumber() == 0) { runtimeError("Division by zero", CURRENT_LINE()); return Result::RUNTIME_ERROR; }
                push(Value(a.asNumber() / b.asNumber())); break;
            }
            runtimeError("Operands of '/' must be numbers", CURRENT_LINE()); return Result::RUNTIME_ERROR;
        }
        case OpCode::OP_MODULO: {
            Value b = pop(), a = pop();
            if (a.isInt() && b.isInt()) {
                if (b.asInt() == 0) { runtimeError("Modulo by zero", CURRENT_LINE()); return Result::RUNTIME_ERROR; }
                push(Value(a.asInt() % b.asInt())); break;
            }
            if (a.isNumber() && b.isNumber()) {
                if (b.asNumber() == 0) { runtimeError("Modulo by zero", CURRENT_LINE()); return Result::RUNTIME_ERROR; }
                push(Value(std::fmod(a.asNumber(), b.asNumber()))); break;
            }
            runtimeError("Operands of '%' must be numbers", CURRENT_LINE()); return Result::RUNTIME_ERROR;
        }
        case OpCode::OP_NEGATE: {
            if (!peek().isNumber()) { runtimeError("Operand of '-' must be a number", CURRENT_LINE()); return Result::RUNTIME_ERROR; }
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
            if (it == globals.end()) { runtimeError("Undefined variable '" + n + "'", CURRENT_LINE()); return Result::RUNTIME_ERROR; }
            push(it->second); break;
        }
        case OpCode::OP_SET_GLOBAL: {
            std::string n = READ_STRING();
            auto it = globals.find(n);
            if (it == globals.end()) { runtimeError("Undefined variable '" + n + "'", CURRENT_LINE()); return Result::RUNTIME_ERROR; }
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
            if (it == globals.end()) { runtimeError("Undefined variable '" + n + "'", CURRENT_LINE()); return Result::RUNTIME_ERROR; }
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
            if (frameCount == 0) {
                pop();
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
                runtimeError("Internal error: OP_CLOSURE constant is not a function", CURRENT_LINE());
                return Result::RUNTIME_ERROR;
            }
            auto* vmcc = dynamic_cast<VMClosureCallable*>(fnVal.asCallable().get());
            if (!vmcc) {
                runtimeError("Internal error: OP_CLOSURE constant is not a VM closure", CURRENT_LINE());
                return Result::RUNTIME_ERROR;
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

        // ── Stubs for later phases ──
        case OpCode::OP_CLASS:
        case OpCode::OP_METHOD:
        case OpCode::OP_INHERIT:
        case OpCode::OP_GET_PROPERTY:
        case OpCode::OP_SET_PROPERTY:
        case OpCode::OP_GET_THIS:
        case OpCode::OP_GET_SUPER:
        case OpCode::OP_INVOKE:
        case OpCode::OP_SUPER_INVOKE:
        case OpCode::OP_BUILD_ARRAY:
        case OpCode::OP_BUILD_MAP:
        case OpCode::OP_INDEX_GET:
        case OpCode::OP_INDEX_SET:
        case OpCode::OP_UNPACK_SPREAD:
        case OpCode::OP_BUILD_STRING:
        case OpCode::OP_TRY_BEGIN:
        case OpCode::OP_TRY_END:
        case OpCode::OP_THROW:
        case OpCode::OP_ASYNC:
        case OpCode::OP_AWAIT:
        case OpCode::OP_IMPORT:
        case OpCode::OP_EXPORT:
            runtimeError("Opcode not yet implemented in VM", CURRENT_LINE());
            return Result::RUNTIME_ERROR;
        }
    }

    #undef FRAME
    #undef READ_BYTE
    #undef READ_U16
    #undef READ_CONSTANT
    #undef READ_STRING
    #undef CURRENT_LINE
}
