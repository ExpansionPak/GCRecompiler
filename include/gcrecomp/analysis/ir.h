#pragma once

#include <gcrecomp/types.h>
#include <vector>
#include <string>
#include <memory>

namespace gcrecomp {

enum class IROp {
    None,
    // Arithmetic
    Add, Sub, Mul, Div,
    And, Or, Xor, Nor, Shl, Shr, Sar,
    Rol, Mask, // For rlwinm etc.
    // Memory
    Load8, Load16, Load32, LoadFloat,
    Store8, Store16, Store32, StoreFloat,
    // Control Flow
    Branch, BranchCond, Call, Return,
    // Comparison
    Cmp, Cmpl,
    // Floating Point
    FAdd, FSub, FMul, FDiv,
    // Special
    SetReg, GetReg, // Access physical registers
    SetImm,         // Load immediate to virtual reg
};

enum class IROperandType {
    None,
    Register,   // 0-31 for GPRs, etc.
    Immediate,
    Address,    // Memory address
    Label,      // Branch target (address or block ID)
};

struct IROperand {
    IROperandType type = IROperandType::None;
    u32 value = 0;
    std::string name; // For debugging/labels

    static IROperand Reg(u32 r) { return { IROperandType::Register, r }; }
    static IROperand Imm(u32 i) { return { IROperandType::Immediate, i }; }
    static IROperand Addr(u32 a) { return { IROperandType::Address, a }; }
};

struct IRInstruction {
    IROp op = IROp::None;
    std::vector<IROperand> operands;

    IRInstruction(IROp o) : op(o) {}
    IRInstruction(IROp o, const std::vector<IROperand>& ops) : op(o), operands(ops) {}
};

struct IRBlock {
    u32 startAddr;
    std::vector<IRInstruction> instructions;
};

} // namespace gcrecomp
