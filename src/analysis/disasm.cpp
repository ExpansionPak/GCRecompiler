#include <gcrecomp/analysis/disasm.h>
#include <gcrecomp/log.h>
#include <iomanip>
#include <sstream>

namespace gcrecomp {

namespace {

inline s32 signExtend(u32 value, u32 width) {
    const u32 shift = 32 - width;
    return static_cast<s32>(value << shift) >> shift;
}

inline bool isUnconditionalBranch(u32 bo) {
    return (bo & 0x14) == 0x14;
}

} // namespace

bool Disassembler::disassemble(const Binary& binary, u32 addr, Instruction& out) {
    if (!binary.read32(addr, out.raw)) {
        return false;
    }

    out.address = addr;
    out.mnemonic = "unknown";
    out.operands = "";
    out.isBranch = false;
    out.branchTarget = 0;
    out.bo = 0;
    out.bi = 0;
    out.isRelative = false;
    out.isLink = false;
    out.isInterruptReturn = false;
    out.branchRegisterTarget = BranchRegisterTarget::None;
    out.type = InstructionType::Unknown;

    u32 op = out.raw >> 26;
    
    // Branch (b, bl, ba, bla)
    if (op == 18) {
        out.isBranch = true;
        bool aa = (out.raw >> 1) & 1;
        bool lk = out.raw & 1;
        s32 disp = signExtend(out.raw & 0x03FFFFFC, 26);

        out.branchTarget = aa ? static_cast<u32>(disp) : static_cast<u32>(static_cast<s32>(addr) + disp);
        out.mnemonic = lk ? "bl" : "b";
        out.isRelative = !aa;
        out.isLink = lk;
        out.type = lk ? InstructionType::Call : InstructionType::Branch;
        
        std::stringstream ss;
        ss << "0x" << std::hex << out.branchTarget;
        out.operands = ss.str();
        return true;
    }

    // Branch Conditional (bc, bcl, bca, bcla)
    if (op == 16) {
        out.isBranch = true;
        out.bo = (out.raw >> 21) & 0x1F;
        out.bi = (out.raw >> 16) & 0x1F;
        bool aa = (out.raw >> 1) & 1;
        bool lk = out.raw & 1;
        s32 disp = signExtend(out.raw & 0x0000FFFC, 16);
        
        out.branchTarget = aa ? static_cast<u32>(disp) : static_cast<u32>(static_cast<s32>(addr) + disp);
        out.mnemonic = lk ? "bcl" : "bc";
        out.isRelative = !aa;
        out.isLink = lk;
        out.type = isUnconditionalBranch(out.bo)
            ? (lk ? InstructionType::Call : InstructionType::Branch)
            : InstructionType::ConditionalBranch;
        
        std::stringstream ss;
        ss << "bo=" << std::dec << out.bo << ", bi=" << out.bi
           << ", 0x" << std::hex << out.branchTarget;
        out.operands = ss.str();
        return true;
    }

    // Branch to LR / CTR
    if (op == 19) {
        u32 xop = (out.raw >> 1) & 0x3FF;
        bool lk = out.raw & 1;
        out.bo = (out.raw >> 21) & 0x1F;
        out.bi = (out.raw >> 16) & 0x1F;
        const bool unconditional = isUnconditionalBranch(out.bo);

        if (xop == 50 && !lk) { // rfi
            out.isBranch = true;
            out.isInterruptReturn = true;
            out.mnemonic = "rfi";
            out.type = InstructionType::Return;
            out.operands.clear();
            return true;
        }
        
        if (xop == 16) { // bclr
            out.isBranch = true;
            out.isLink = lk;
            out.branchRegisterTarget = BranchRegisterTarget::LinkRegister;
            if (!lk && unconditional && out.bi == 0) {
                out.mnemonic = "blr";
                out.type = InstructionType::Return;
                out.operands.clear();
                return true;
            }

            out.mnemonic = lk ? "bclrl" : "bclr";
            out.type = unconditional
                ? (lk ? InstructionType::Call : InstructionType::Branch)
                : InstructionType::ConditionalBranch;
            std::stringstream ss;
            ss << "bo=" << std::dec << out.bo << ", bi=" << out.bi;
            out.operands = ss.str();
            return true;
        }
        if (xop == 528) { // bcctr
            out.isBranch = true;
            out.isLink = lk;
            out.branchRegisterTarget = BranchRegisterTarget::CountRegister;
            out.mnemonic = lk ? "bcctrl" : "bcctr";
            out.type = unconditional
                ? (lk ? InstructionType::Call : InstructionType::Branch)
                : InstructionType::ConditionalBranch;
            std::stringstream ss;
            ss << "bo=" << std::dec << out.bo << ", bi=" << out.bi;
            out.operands = ss.str();
            return true;
        }
    }

    // Not a branch we care about for CFG (for now)
    return true;
}

} // namespace gcrecomp
