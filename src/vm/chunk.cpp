#include "chunk.h"

void Chunk::write(uint8_t byte, int line) {
    code.push_back(byte);
    if (!lines.empty() && lines.back().line == line) {
        lines.back().count++;
    } else {
        lines.push_back({line, 1});
    }
}

void Chunk::write(OpCode op, int line) {
    write(static_cast<uint8_t>(op), line);
}

void Chunk::writeU16(uint16_t value, int line) {
    write(static_cast<uint8_t>((value >> 8) & 0xFF), line);
    write(static_cast<uint8_t>(value & 0xFF), line);
}

uint16_t Chunk::addConstant(Value value) {
    if (constants.size() >= 65535)
        throw RuntimeError("Too many constants in one chunk (max 65535)", 0);
    constants.push_back(std::move(value));
    return static_cast<uint16_t>(constants.size() - 1);
}

int Chunk::getLine(int offset) const {
    int current = 0;
    for (auto& entry : lines) {
        current += entry.count;
        if (offset < current) return entry.line;
    }
    return lines.empty() ? 0 : lines.back().line;
}

void Chunk::patchU16(int offset, uint16_t value) {
    code[offset] = static_cast<uint8_t>((value >> 8) & 0xFF);
    code[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}
