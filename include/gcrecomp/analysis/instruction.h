#pragma once

#include <gcrecomp/types.h>
#include <string>
#include <vector>

namespace gcrecomp {

enum class InstructionType {
    Unknown,
    Branch,
    BranchLink,
    ConditionalBranch,
    Return,
    Call,
    System,
    Memory,
    Compute
};

enum class BranchRegisterTarget {
    None,
    LinkRegister,
    CountRegister,
};

struct Instruction {
    u32 address;
    u32 raw;
    std::string mnemonic;
    std::string operands;
    InstructionType type;
    u32 branchTarget = 0; // If applicable
    u32 bo = 0;
    u32 bi = 0;
    bool isBranch = false;
    bool isRelative = false;
    bool isLink = false; // bl or bclr
    bool isInterruptReturn = false;
    BranchRegisterTarget branchRegisterTarget = BranchRegisterTarget::None;
};

} // namespace gcrecomp
