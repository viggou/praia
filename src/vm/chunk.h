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

    // Run-length encoded column info
    struct ColumnEntry { int column; int count; };
    std::vector<ColumnEntry> columns;

    void write(uint8_t byte, int line, int column = 0);
    void write(OpCode op, int line, int column = 0);
    void writeU16(uint16_t value, int line, int column = 0);
    uint16_t addConstant(Value value);
    int getLine(int offset) const;
    int getColumn(int offset) const;
    void patchU16(int offset, uint16_t value);
    int size() const { return static_cast<int>(code.size()); }
};
