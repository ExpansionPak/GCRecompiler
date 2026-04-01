#pragma once

#include <gcrecomp/analysis/cfg.h>
#include <gcrecomp/analysis/ir.h>
#include <string>
#include <vector>
#include <ostream>

namespace gcrecomp {

struct Function;
class ControlFlowGraph;
struct BasicBlock;
struct IRInstruction;
struct IROperand;

class Emitter {
public:
    Emitter();
    ~Emitter();

    std::string emitFunction(const Function& func, const ControlFlowGraph& cfg);
    std::string emitBlock(const BasicBlock& block);

private:
    std::string emitInstruction(const IRInstruction& instr,
                                const std::set<u32>& emittedBlocks,
                                const std::set<u32>& localResumeTargets);
    std::string operandToC(const IROperand& op);
    std::string regOrImm(const IROperand& op);
    std::string fRegOrImm(const IROperand& op);
};

} // namespace gcrecomp
