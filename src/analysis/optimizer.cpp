#include <gcrecomp/analysis/optimizer.h>
#include <gcrecomp/log.h>
#include <map>
#include <vector>

namespace gcrecomp {

void Optimizer::optimizeBlock(IRBlock &block) { constantPropagation(block); }

void Optimizer::constantPropagation(IRBlock &block) {
  std::map<u32, u32> regValues; // register -> constant value
  std::vector<IRInstruction> optimized;

  for (auto &instr : block.instructions) {
    bool instructionSimplified = false;

    // Fold operands if they are known constants
    for (auto &op : instr.operands) {
      if (op.type == IROperandType::Register) {
        auto it = regValues.find(op.value);
        if (it != regValues.end()) {
          op.type = IROperandType::Immediate;
          op.value = it->second;
        }
      }
    }

    // Perform Constant Folding
    if (instr.op == IROp::Add && instr.operands.size() == 3) {
      if (instr.operands[1].type == IROperandType::Immediate &&
          instr.operands[2].type == IROperandType::Immediate) {
        u32 result = instr.operands[1].value + instr.operands[2].value;
        u32 destReg = instr.operands[0].value;

        // Replace with SetImm
        instr.op = IROp::SetImm;
        instr.operands = {IROperand::Reg(destReg), IROperand::Imm(result)};
      }
    }

    // Track known constants
    if (instr.op == IROp::SetImm) {
      u32 destReg = instr.operands[0].value;
      u32 value = instr.operands[1].value;
      regValues[destReg] = value;
    } else {
      // Any other op writing to a register kills its constant state
      // (Note: Simplified for local propagation within a single block)
      if (!instr.operands.empty() &&
          instr.operands[0].type == IROperandType::Register) {
        regValues.erase(instr.operands[0].value);
      }
    }

    optimized.push_back(instr);
  }

  block.instructions = optimized;

  // 2nd Pass: Redundant Store Elimination (local to block)
  std::vector<IRInstruction> finalInstructions;
  std::map<u32, size_t> lastWrite; // register -> index in finalInstructions

  for (const auto &instr : block.instructions) {
    // If this instruction reads registers, clear their lastWrite entries
    // Operands after indices 0 are reads (usually)
    // We'll be conservative and say anything past 0 is a read
    for (size_t i = 1; i < instr.operands.size(); ++i) {
      if (instr.operands[i].type == IROperandType::Register) {
        lastWrite.erase(instr.operands[i].value);
      }
    }

    // Check if this is a write to a register (usually operand 0)
    if (!instr.operands.empty() &&
        instr.operands[0].type == IROperandType::Register) {
      u32 reg = instr.operands[0].value;
      if (lastWrite.count(reg)) {
        // The previous write was redundant!
        finalInstructions[lastWrite[reg]].op = IROp::None;
      }
      lastWrite[reg] = finalInstructions.size();
    }
    finalInstructions.push_back(instr);
  }

  // Filter out None instructions
  block.instructions.clear();
  for (const auto &instr : finalInstructions) {
    if (instr.op != IROp::None) {
      block.instructions.push_back(instr);
    }
  }
}

} // namespace gcrecomp
