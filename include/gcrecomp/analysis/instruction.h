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

struct Instruction {
    u32 address;
    u32 raw;
    std::string mnemonic;
    std::string operands;
    InstructionType type;
    u32 branchTarget = 0; // If applicable
    bool isBranch = false;
    bool isRelative = false;
    bool isLink = false; // bl or bclr
};

} // namespace gcrecomp
