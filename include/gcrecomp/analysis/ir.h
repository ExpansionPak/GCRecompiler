#pragma once

#include <gcrecomp/types.h>
#include <vector>
#include <string>
#include <memory>

namespace gcrecomp {

enum class IROp {
    None,
    // Arithmetic
    And, Or, Xor, Nor, Andc, Orc, Nand, Shl, Shr, Sar,
    Rol, Mask, // For rlwinm etc.
    Rlwimi,    // For rlwimi
    Add, Sub, Mul, DivS, DivU, Addic,
    // Bit Manipulation
    Extsb, Extsh, Cntlzw,
    // Memory
    Load8, Load8u, Load16, Load16u, Load16a, Load32, Load32u, LoadFloat, LoadDouble,
    Store8, Store8u, Store16, Store16u, Store32, Store32u, StoreFloat, StoreDouble,
    Lmw, Stmw,
    // Control Flow
    Branch, BranchCond, BranchIndirect, BranchTable, Call, CallIndirect, Return, Rfi,
    // Comparison
    Cmp, Cmpl,
    CrAnd, CrOr, CrXor, CrNor,
    // Floating Point
    FAdd, FSub, FMul, FDiv, FMadd, FMsub, FSel, Fctiw, Frsp,
    // System / Special
    SetReg, GetReg, // Access physical registers
    SetImm,         // Load immediate to virtual reg
    Mfspr, Mtspr,
    Mfcr, Mtcrf,
    Mfmsr, Mtmsr,
    Sync, Isync,
    Trap, Syscall,
    ReservationLoad, ReservationStore,
};

enum class IROperandType {
    None,
    Register,   // 0-31 for GPRs, etc.
    Immediate,
    Address,    // Memory address
    Label,      // Branch target (address or block ID)
    SpecialRegister,
};

enum class IRRegisterClass {
    None,
    GPR,
    FPR,
};

enum class IRSpecialRegister : u32 {
    None = 0,
    LinkRegister = 1,
    CountRegister = 2,
};

struct IROperand {
    IROperandType type = IROperandType::None;
    u32 value = 0;
    IRRegisterClass regClass = IRRegisterClass::None;
    std::string name; // For debugging/labels

    static IROperand Reg(u32 r) {
        IROperand op;
        op.type = IROperandType::Register;
        op.value = r;
        op.regClass = IRRegisterClass::GPR;
        return op;
    }

    static IROperand FReg(u32 r) {
        IROperand op;
        op.type = IROperandType::Register;
        op.value = r;
        op.regClass = IRRegisterClass::FPR;
        return op;
    }

    static IROperand Imm(u32 i) {
        IROperand op;
        op.type = IROperandType::Immediate;
        op.value = i;
        return op;
    }

    static IROperand Addr(u32 a) {
        IROperand op;
        op.type = IROperandType::Address;
        op.value = a;
        return op;
    }

    static IROperand Special(IRSpecialRegister reg) {
        IROperand op;
        op.type = IROperandType::SpecialRegister;
        op.value = static_cast<u32>(reg);
        return op;
    }
};

struct IRInstruction {
    IROp op = IROp::None;
    u32 address = 0;
    std::vector<IROperand> operands;

    IRInstruction(IROp o) : op(o) {}
    IRInstruction(IROp o, const std::vector<IROperand>& ops) : op(o), operands(ops) {}
};

struct IRBlock {
    u32 startAddr;
    std::vector<IRInstruction> instructions;
};

} // namespace gcrecomp
