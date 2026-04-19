#include "vm.h"
#include "../interpreter.h" // for NativeFunction, Callable
#include <cmath>
#include <iostream>

VM::VM() {}

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

Value VM::pop() {
    return std::move(stack[--stackTop]);
}

Value& VM::peek(int distance) {
    return stack[stackTop - 1 - distance];
}

void VM::resetStack() {
    stackTop = 0;
    frameCount = 0;
}

uint8_t VM::readByte() {
    return *frames[frameCount - 1].ip++;
}

uint16_t VM::readU16() {
    auto& frame = frames[frameCount - 1];
    uint16_t val = (frame.ip[0] << 8) | frame.ip[1];
    frame.ip += 2;
    return val;
}

Value VM::readConstant() {
    uint16_t idx = readU16();
    return frames[frameCount - 1].function->chunk.constants[idx];
}

std::string VM::readString() {
    return readConstant().asString();
}

bool VM::callValue(Value callee, int argCount, int line) {
    if (callee.isCallable()) {
        auto callable = callee.asCallable();

        // Check if it's a NativeFunction
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
            pop(); // the callable itself
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
        int offset = static_cast<int>(frame.ip - frame.function->chunk.code.data());
        int line = frame.function->chunk.getLine(offset);
        trace += "  at " + frame.function->name + "() line " + std::to_string(line) + "\n";
    }
    return trace;
}

VM::Result VM::run(std::shared_ptr<CompiledFunction> script) {
    // Set up the initial call frame
    frames[0].function = script;
    frames[0].ip = script->chunk.code.data();
    frames[0].baseSlot = 0;
    frameCount = 1;

    push(Value()); // slot 0 for the script

    return execute();
}

VM::Result VM::execute() {
    auto& frame = frames[frameCount - 1];

    #define READ_BYTE() (*frame.ip++)
    #define READ_U16() (frame.ip += 2, static_cast<uint16_t>((frame.ip[-2] << 8) | frame.ip[-1]))
    #define READ_CONSTANT() (frame.function->chunk.constants[READ_U16()])
    #define READ_STRING() (READ_CONSTANT().asString())
    #define CURRENT_LINE() (frame.function->chunk.getLine(static_cast<int>(frame.ip - frame.function->chunk.code.data())))

    for (;;) {
        uint8_t instruction = READ_BYTE();

        switch (static_cast<OpCode>(instruction)) {

        case OpCode::OP_CONSTANT: {
            push(READ_CONSTANT());
            break;
        }
        case OpCode::OP_NIL: push(Value()); break;
        case OpCode::OP_TRUE: push(Value(true)); break;
        case OpCode::OP_FALSE: push(Value(false)); break;
        case OpCode::OP_POP: pop(); break;
        case OpCode::OP_POPN: {
            uint8_t n = READ_BYTE();
            stackTop -= n;
            break;
        }

        // ── Arithmetic ──
        case OpCode::OP_ADD: {
            Value b = pop(), a = pop();
            if (a.isInt() && b.isInt()) { push(Value(a.asInt() + b.asInt())); break; }
            if (a.isNumber() && b.isNumber()) { push(Value(a.asNumber() + b.asNumber())); break; }
            if (a.isArray() && b.isArray()) {
                auto result = std::make_shared<PraiaArray>();
                for (auto& el : a.asArray()->elements) result->elements.push_back(el);
                for (auto& el : b.asArray()->elements) result->elements.push_back(el);
                push(Value(result)); break;
            }
            if (a.isString() || b.isString()) { push(Value(a.toString() + b.toString())); break; }
            runtimeError("Operands of '+' must be numbers, strings, or arrays", CURRENT_LINE());
            return Result::RUNTIME_ERROR;
        }
        case OpCode::OP_SUBTRACT: {
            Value b = pop(), a = pop();
            if (a.isInt() && b.isInt()) { push(Value(a.asInt() - b.asInt())); break; }
            if (a.isNumber() && b.isNumber()) { push(Value(a.asNumber() - b.asNumber())); break; }
            runtimeError("Operands of '-' must be numbers", CURRENT_LINE());
            return Result::RUNTIME_ERROR;
        }
        case OpCode::OP_MULTIPLY: {
            Value b = pop(), a = pop();
            if (a.isInt() && b.isInt()) { push(Value(a.asInt() * b.asInt())); break; }
            if (a.isNumber() && b.isNumber()) { push(Value(a.asNumber() * b.asNumber())); break; }
            runtimeError("Operands of '*' must be numbers", CURRENT_LINE());
            return Result::RUNTIME_ERROR;
        }
        case OpCode::OP_DIVIDE: {
            Value b = pop(), a = pop();
            if (a.isNumber() && b.isNumber()) {
                if (b.asNumber() == 0) { runtimeError("Division by zero", CURRENT_LINE()); return Result::RUNTIME_ERROR; }
                push(Value(a.asNumber() / b.asNumber())); break;
            }
            runtimeError("Operands of '/' must be numbers", CURRENT_LINE());
            return Result::RUNTIME_ERROR;
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
            runtimeError("Operands of '%' must be numbers", CURRENT_LINE());
            return Result::RUNTIME_ERROR;
        }
        case OpCode::OP_NEGATE: {
            if (!peek().isNumber()) { runtimeError("Operand of '-' must be a number", CURRENT_LINE()); return Result::RUNTIME_ERROR; }
            if (peek().isInt()) { push(Value(-pop().asInt())); }
            else { push(Value(-pop().asNumber())); }
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
        case OpCode::OP_DEFINE_GLOBAL: {
            std::string name = READ_STRING();
            globals[name] = pop();
            break;
        }
        case OpCode::OP_GET_GLOBAL: {
            std::string name = READ_STRING();
            auto it = globals.find(name);
            if (it == globals.end()) {
                runtimeError("Undefined variable '" + name + "'", CURRENT_LINE());
                return Result::RUNTIME_ERROR;
            }
            push(it->second);
            break;
        }
        case OpCode::OP_SET_GLOBAL: {
            std::string name = READ_STRING();
            auto it = globals.find(name);
            if (it == globals.end()) {
                runtimeError("Undefined variable '" + name + "'", CURRENT_LINE());
                return Result::RUNTIME_ERROR;
            }
            it->second = peek();
            break;
        }
        case OpCode::OP_GET_LOCAL: {
            uint16_t slot = READ_U16();
            push(stack[frame.baseSlot + slot]);
            break;
        }
        case OpCode::OP_SET_LOCAL: {
            uint16_t slot = READ_U16();
            stack[frame.baseSlot + slot] = peek();
            break;
        }

        // ── Postfix ──
        case OpCode::OP_POST_INC_LOCAL: {
            uint16_t slot = READ_U16();
            Value& val = stack[frame.baseSlot + slot];
            push(val); // push old value
            if (val.isInt()) val = Value(val.asInt() + 1);
            else val = Value(val.asNumber() + 1);
            break;
        }
        case OpCode::OP_POST_DEC_LOCAL: {
            uint16_t slot = READ_U16();
            Value& val = stack[frame.baseSlot + slot];
            push(val);
            if (val.isInt()) val = Value(val.asInt() - 1);
            else val = Value(val.asNumber() - 1);
            break;
        }
        case OpCode::OP_POST_INC_GLOBAL:
        case OpCode::OP_POST_DEC_GLOBAL: {
            std::string name = READ_STRING();
            auto it = globals.find(name);
            if (it == globals.end()) { runtimeError("Undefined variable '" + name + "'", CURRENT_LINE()); return Result::RUNTIME_ERROR; }
            push(it->second);
            if (it->second.isInt())
                it->second = Value(it->second.asInt() + (static_cast<OpCode>(instruction) == OpCode::OP_POST_INC_GLOBAL ? 1 : -1));
            else
                it->second = Value(it->second.asNumber() + (static_cast<OpCode>(instruction) == OpCode::OP_POST_INC_GLOBAL ? 1 : -1));
            break;
        }

        // ── Control flow ──
        case OpCode::OP_JUMP: {
            uint16_t offset = READ_U16();
            frame.ip += offset;
            break;
        }
        case OpCode::OP_JUMP_BACK: {
            uint16_t offset = READ_U16();
            frame.ip -= offset;
            break;
        }
        case OpCode::OP_JUMP_IF_FALSE: {
            uint16_t offset = READ_U16();
            if (!peek().isTruthy()) frame.ip += offset;
            break;
        }
        case OpCode::OP_JUMP_IF_TRUE: {
            uint16_t offset = READ_U16();
            if (peek().isTruthy()) frame.ip += offset;
            break;
        }
        case OpCode::OP_POP_JUMP_IF_FALSE: {
            uint16_t offset = READ_U16();
            if (!pop().isTruthy()) frame.ip += offset;
            break;
        }

        // ── Functions ──
        case OpCode::OP_CALL: {
            uint8_t argc = READ_BYTE();
            Value callee = peek(argc);
            if (!callValue(callee, argc, CURRENT_LINE())) {
                return Result::RUNTIME_ERROR;
            }
            break;
        }
        case OpCode::OP_RETURN: {
            Value result = pop();
            frameCount--;
            if (frameCount == 0) {
                pop(); // script slot
                return Result::OK;
            }
            stackTop = frame.baseSlot;
            push(std::move(result));
            // Restore frame reference
            frame = frames[frameCount - 1];
            break;
        }

        // ── Stubs for later phases ──
        case OpCode::OP_CLOSURE:
        case OpCode::OP_GET_UPVALUE:
        case OpCode::OP_SET_UPVALUE:
        case OpCode::OP_CLOSE_UPVALUE:
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

    #undef READ_BYTE
    #undef READ_U16
    #undef READ_CONSTANT
    #undef READ_STRING
    #undef CURRENT_LINE
}
