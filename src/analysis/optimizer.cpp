#include <gcrecomp/analysis/optimizer.h>
#include <gcrecomp/log.h>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace gcrecomp {

namespace {

bool isGprRegister(const IROperand& operand) {
    return operand.type == IROperandType::Register && operand.regClass == IRRegisterClass::GPR;
}

bool writesAnyGprValue(const IRInstruction& instr) {
    switch (instr.op) {
        case IROp::SetImm:
        case IROp::SetReg:
        case IROp::Add:
        case IROp::Sub:
        case IROp::Mul:
        case IROp::DivS:
        case IROp::DivU:
        case IROp::And:
        case IROp::Or:
        case IROp::Xor:
        case IROp::Nor:
        case IROp::Andc:
        case IROp::Orc:
        case IROp::Nand:
        case IROp::Addic:
        case IROp::Shl:
        case IROp::Shr:
        case IROp::Sar:
        case IROp::Rol:
        case IROp::Mask:
        case IROp::Rlwimi:
        case IROp::Extsb:
        case IROp::Extsh:
        case IROp::Cntlzw:
        case IROp::Load8:
        case IROp::Load8u:
        case IROp::Load16:
        case IROp::Load16u:
        case IROp::Load16a:
        case IROp::Load32:
        case IROp::Load32u:
        case IROp::Mfcr:
        case IROp::Mfspr:
        case IROp::Mfmsr:
        case IROp::ReservationLoad:
            return !instr.operands.empty() && isGprRegister(instr.operands[0]);
        default:
            return false;
    }
}

bool writesPureGprValue(const IRInstruction& instr) {
    switch (instr.op) {
        case IROp::SetImm:
        case IROp::SetReg:
        case IROp::Add:
        case IROp::Sub:
        case IROp::Mul:
        case IROp::DivS:
        case IROp::DivU:
        case IROp::And:
        case IROp::Or:
        case IROp::Xor:
        case IROp::Nor:
        case IROp::Andc:
        case IROp::Orc:
        case IROp::Nand:
        case IROp::Addic:
        case IROp::Shl:
        case IROp::Shr:
        case IROp::Sar:
        case IROp::Rol:
        case IROp::Mask:
        case IROp::Rlwimi:
        case IROp::Extsb:
        case IROp::Extsh:
        case IROp::Cntlzw:
            return !instr.operands.empty() && isGprRegister(instr.operands[0]);
        default:
            return false;
    }
}

std::optional<u32> writtenGpr(const IRInstruction& instr) {
    if (writesAnyGprValue(instr)) {
        return instr.operands[0].value;
    }
    return std::nullopt;
}

std::optional<u32> writtenPureGpr(const IRInstruction& instr) {
    if (writesPureGprValue(instr)) {
        return instr.operands[0].value;
    }
    return std::nullopt;
}

bool operandIsRead(const IRInstruction& instr, size_t index) {
    switch (instr.op) {
        case IROp::SetImm:
        case IROp::Mfcr:
        case IROp::Mfspr:
        case IROp::Mfmsr:
        case IROp::Return:
        case IROp::Rfi:
        case IROp::Sync:
        case IROp::Isync:
        case IROp::Syscall:
            return false;

        case IROp::Rlwimi:
            return index <= 4;

        case IROp::Store8:
        case IROp::Store16:
        case IROp::Store32:
        case IROp::StoreFloat:
        case IROp::StoreDouble:
        case IROp::ReservationStore:
            return index <= 2;

        case IROp::Lmw:
        case IROp::Stmw:
            return index == 1;

        case IROp::Cmp:
        case IROp::Cmpl:
            return index == 1 || index == 2;

        case IROp::CrAnd:
        case IROp::CrOr:
        case IROp::CrXor:
        case IROp::CrNor:
        case IROp::Trap:
            return false;

        case IROp::Mtcrf:
        case IROp::Mtspr:
            return index == 1;

        case IROp::Mtmsr:
            return index == 0;

        case IROp::Branch:
        case IROp::Call:
        case IROp::BranchCond:
        case IROp::BranchIndirect:
        case IROp::CallIndirect:
            return false;

        case IROp::BranchTable:
            return index == 0;

        default:
            return index > 0;
    }
}

void collectReadGprs(const IRInstruction& instr, std::set<u32>& out) {
    for (size_t i = 0; i < instr.operands.size(); ++i) {
        if (!operandIsRead(instr, i)) {
            continue;
        }
        if (isGprRegister(instr.operands[i])) {
            out.insert(instr.operands[i].value);
        }
    }
}

void propagateKnownConstants(IRInstruction& instr, const std::map<u32, u32>& regValues) {
    for (size_t i = 0; i < instr.operands.size(); ++i) {
        if (!operandIsRead(instr, i)) {
            continue;
        }

        // Read/modify/write ops such as rlwimi use operand 0 as both the
        // destination register and an input value. Keep the destination typed
        // as a register so the emitter can still write back to the correct GPR.
        if (i == 0 && writesAnyGprValue(instr)) {
            continue;
        }

        auto& operand = instr.operands[i];
        if (!isGprRegister(operand)) {
            continue;
        }

        auto it = regValues.find(operand.value);
        if (it == regValues.end()) {
            continue;
        }

        operand.type = IROperandType::Immediate;
        operand.value = it->second;
        operand.regClass = IRRegisterClass::None;
    }
}

void foldSimpleConstants(IRInstruction& instr) {
    if (instr.op == IROp::Add && instr.operands.size() == 3 &&
        instr.operands[1].type == IROperandType::Immediate &&
        instr.operands[2].type == IROperandType::Immediate) {
        instr.op = IROp::SetImm;
        instr.operands = { IROperand::Reg(instr.operands[0].value), IROperand::Imm(instr.operands[1].value + instr.operands[2].value) };
        return;
    }

    if (instr.op == IROp::SetReg && instr.operands.size() == 2 &&
        instr.operands[0].regClass == IRRegisterClass::GPR &&
        instr.operands[1].type == IROperandType::Immediate) {
        instr.op = IROp::SetImm;
        instr.operands = { IROperand::Reg(instr.operands[0].value), IROperand::Imm(instr.operands[1].value) };
    }
}

} // namespace

void Optimizer::optimizeBlock(IRBlock& block) {
    constantPropagation(block);
}

void Optimizer::constantPropagation(IRBlock& block) {
    std::map<u32, u32> regValues;
    std::vector<IRInstruction> optimized;

    for (auto instr : block.instructions) {
        propagateKnownConstants(instr, regValues);
        foldSimpleConstants(instr);

        if (instr.op == IROp::SetImm && instr.operands.size() == 2 && isGprRegister(instr.operands[0])) {
            regValues[instr.operands[0].value] = instr.operands[1].value;
        } else if (auto written = writtenGpr(instr)) {
            regValues.erase(*written);
        }

        optimized.push_back(std::move(instr));
    }

    std::vector<IRInstruction> finalInstructions;
    std::map<u32, size_t> lastPureWrite;

    for (const auto& instr : optimized) {
        std::set<u32> reads;
        collectReadGprs(instr, reads);
        for (u32 reg : reads) {
            lastPureWrite.erase(reg);
        }

        if (auto written = writtenGpr(instr)) {
            auto existing = lastPureWrite.find(*written);
            if (existing != lastPureWrite.end()) {
                finalInstructions[existing->second].op = IROp::None;
                lastPureWrite.erase(existing);
            }
        }

        if (auto written = writtenPureGpr(instr)) {
            lastPureWrite[*written] = finalInstructions.size();
        }

        finalInstructions.push_back(instr);
    }

    block.instructions.clear();
    for (const auto& instr : finalInstructions) {
        if (instr.op != IROp::None) {
            block.instructions.push_back(instr);
        }
    }
}

} // namespace gcrecomp
