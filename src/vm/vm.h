#pragma once

#include "../value.h"
#include "chunk.h"
#include "compiler.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct VMCallFrame {
    std::shared_ptr<CompiledFunction> function;
    const uint8_t* ip;
    int baseSlot;
};

class VM {
public:
    VM();

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

    // Helpers
    uint8_t readByte();
    uint16_t readU16();
    Value readConstant();
    std::string readString();
    bool callValue(Value callee, int argCount, int line);

    void runtimeError(const std::string& msg, int line);
    std::string formatStackTrace() const;
};
