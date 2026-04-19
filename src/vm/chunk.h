#pragma once

#include "../value.h"
#include "opcode.h"
#include <cstdint>
#include <string>
#include <vector>

struct Chunk {
    std::vector<uint8_t> code;
    std::vector<Value> constants;

    // Run-length encoded line info
    struct LineEntry { int line; int count; };
    std::vector<LineEntry> lines;

    void write(uint8_t byte, int line);
    void write(OpCode op, int line);
    void writeU16(uint16_t value, int line);
    uint16_t addConstant(Value value);
    int getLine(int offset) const;
    void patchU16(int offset, uint16_t value);
    int size() const { return static_cast<int>(code.size()); }
};
