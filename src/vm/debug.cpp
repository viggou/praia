#include "debug.h"
#include "opcode.h"
#include <iomanip>
#include <iostream>

static int simpleInstruction(const char* name, int offset) {
    std::cout << name << "\n";
    return offset + 1;
}

static int constantInstruction(const char* name, const Chunk& chunk, int offset) {
    uint16_t idx = (chunk.code[offset + 1] << 8) | chunk.code[offset + 2];
    std::cout << std::left << std::setw(20) << name << " " << idx
              << " '" << chunk.constants[idx].toString() << "'\n";
    return offset + 3;
}

static int byteInstruction(const char* name, const Chunk& chunk, int offset) {
    uint8_t val = chunk.code[offset + 1];
    std::cout << std::left << std::setw(20) << name << " " << (int)val << "\n";
    return offset + 2;
}

static int u16Instruction(const char* name, const Chunk& chunk, int offset) {
    uint16_t val = (chunk.code[offset + 1] << 8) | chunk.code[offset + 2];
    std::cout << std::left << std::setw(20) << name << " " << val << "\n";
    return offset + 3;
}

static int jumpInstruction(const char* name, int sign, const Chunk& chunk, int offset) {
    uint16_t jump = (chunk.code[offset + 1] << 8) | chunk.code[offset + 2];
    std::cout << std::left << std::setw(20) << name << " " << offset
              << " -> " << (offset + 3 + sign * jump) << "\n";
    return offset + 3;
}

void disassembleChunk(const Chunk& chunk, const std::string& name) {
    std::cout << "== " << name << " ==\n";
    for (int offset = 0; offset < chunk.size();) {
        offset = disassembleInstruction(chunk, offset);
    }
}

int disassembleInstruction(const Chunk& chunk, int offset) {
    std::cout << std::setfill('0') << std::setw(4) << offset << " ";

    int line = chunk.getLine(offset);
    if (offset > 0 && line == chunk.getLine(offset - 1))
        std::cout << "   | ";
    else
        std::cout << std::setfill(' ') << std::setw(4) << line << " ";

    uint8_t instruction = chunk.code[offset];
    switch (static_cast<OpCode>(instruction)) {
        case OpCode::OP_CONSTANT:       return constantInstruction("OP_CONSTANT", chunk, offset);
        case OpCode::OP_NIL:            return simpleInstruction("OP_NIL", offset);
        case OpCode::OP_TRUE:           return simpleInstruction("OP_TRUE", offset);
        case OpCode::OP_FALSE:          return simpleInstruction("OP_FALSE", offset);
        case OpCode::OP_POP:            return simpleInstruction("OP_POP", offset);
        case OpCode::OP_POPN:           return byteInstruction("OP_POPN", chunk, offset);
        case OpCode::OP_ADD:            return simpleInstruction("OP_ADD", offset);
        case OpCode::OP_SUBTRACT:       return simpleInstruction("OP_SUBTRACT", offset);
        case OpCode::OP_MULTIPLY:       return simpleInstruction("OP_MULTIPLY", offset);
        case OpCode::OP_DIVIDE:         return simpleInstruction("OP_DIVIDE", offset);
        case OpCode::OP_MODULO:         return simpleInstruction("OP_MODULO", offset);
        case OpCode::OP_NEGATE:         return simpleInstruction("OP_NEGATE", offset);
        case OpCode::OP_BIT_AND:        return simpleInstruction("OP_BIT_AND", offset);
        case OpCode::OP_BIT_OR:         return simpleInstruction("OP_BIT_OR", offset);
        case OpCode::OP_BIT_XOR:        return simpleInstruction("OP_BIT_XOR", offset);
        case OpCode::OP_BIT_NOT:        return simpleInstruction("OP_BIT_NOT", offset);
        case OpCode::OP_SHL:            return simpleInstruction("OP_SHL", offset);
        case OpCode::OP_SHR:            return simpleInstruction("OP_SHR", offset);
        case OpCode::OP_EQUAL:          return simpleInstruction("OP_EQUAL", offset);
        case OpCode::OP_NOT_EQUAL:      return simpleInstruction("OP_NOT_EQUAL", offset);
        case OpCode::OP_LESS:           return simpleInstruction("OP_LESS", offset);
        case OpCode::OP_GREATER:        return simpleInstruction("OP_GREATER", offset);
        case OpCode::OP_LESS_EQUAL:     return simpleInstruction("OP_LESS_EQUAL", offset);
        case OpCode::OP_GREATER_EQUAL:  return simpleInstruction("OP_GREATER_EQUAL", offset);
        case OpCode::OP_NOT:            return simpleInstruction("OP_NOT", offset);
        case OpCode::OP_DEFINE_GLOBAL:  return constantInstruction("OP_DEFINE_GLOBAL", chunk, offset);
        case OpCode::OP_GET_GLOBAL:     return constantInstruction("OP_GET_GLOBAL", chunk, offset);
        case OpCode::OP_SET_GLOBAL:     return constantInstruction("OP_SET_GLOBAL", chunk, offset);
        case OpCode::OP_GET_LOCAL:      return u16Instruction("OP_GET_LOCAL", chunk, offset);
        case OpCode::OP_SET_LOCAL:      return u16Instruction("OP_SET_LOCAL", chunk, offset);
        case OpCode::OP_POST_INC_LOCAL: return u16Instruction("OP_POST_INC_LOCAL", chunk, offset);
        case OpCode::OP_POST_DEC_LOCAL: return u16Instruction("OP_POST_DEC_LOCAL", chunk, offset);
        case OpCode::OP_POST_INC_GLOBAL:return constantInstruction("OP_POST_INC_GLOBAL", chunk, offset);
        case OpCode::OP_POST_DEC_GLOBAL:return constantInstruction("OP_POST_DEC_GLOBAL", chunk, offset);
        case OpCode::OP_JUMP:           return jumpInstruction("OP_JUMP", 1, chunk, offset);
        case OpCode::OP_JUMP_BACK:      return jumpInstruction("OP_JUMP_BACK", -1, chunk, offset);
        case OpCode::OP_JUMP_IF_FALSE:  return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
        case OpCode::OP_JUMP_IF_TRUE:   return jumpInstruction("OP_JUMP_IF_TRUE", 1, chunk, offset);
        case OpCode::OP_POP_JUMP_IF_FALSE: return jumpInstruction("OP_POP_JUMP_IF_FALSE", 1, chunk, offset);
        case OpCode::OP_CALL:           return byteInstruction("OP_CALL", chunk, offset);
        case OpCode::OP_RETURN:         return simpleInstruction("OP_RETURN", offset);
        default:
            std::cout << "Unknown opcode " << (int)instruction << "\n";
            return offset + 1;
    }
}
