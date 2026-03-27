#include <gcrecomp/analysis/analyzer.h>
#include <gcrecomp/log.h>

namespace gcrecomp {

Analyzer::Analyzer(const Binary& binary) : m_binary(binary) {}

void Analyzer::analyze(u32 entryPoint) {
    m_workList.push(entryPoint);

    while (!m_workList.empty()) {
        u32 addr = m_workList.front();
        m_workList.pop();

        if (m_visited.count(addr)) continue;
        
        analyzeBlock(addr);
    }
}

void Analyzer::analyzeBlock(u32 startAddr) {
    BasicBlock block;
    block.startAddr = startAddr;
    u32 currentAddr = startAddr;

    LOG_DEBUG("Analyzing block at 0x%08X", startAddr);

    while (true) {
        Instruction instr;
        if (!m_disasm.disassemble(m_binary, currentAddr, instr)) {
            block.type = BlockType::Invalid;
            break;
        }

        block.instructions.push_back(instr);
        m_visited.insert(currentAddr);

        if (instr.isBranch) {
            block.endAddr = currentAddr;
            
            if (instr.type == InstructionType::Return) {
                block.type = BlockType::Return;
            } else if (instr.type == InstructionType::Call) {
                block.type = BlockType::Call;
                // For calls, we assume they return to the next instruction
                u32 next = currentAddr + 4;
                block.successors.insert(next);
                m_workList.push(next);
                
                // Also add call target if known
                if (instr.branchTarget != 0) {
                    m_workList.push(instr.branchTarget);
                }
            } else if (instr.type == InstructionType::Branch) {
                if (instr.branchTarget != 0) {
                    block.successors.insert(instr.branchTarget);
                    m_workList.push(instr.branchTarget);
                }
                
                // If conditional, also add fallthrough
                if (instr.mnemonic.find('b') == 0 && instr.mnemonic.length() > 1 && instr.mnemonic[1] != ' ') {
                     // Very simple heuristic for conditional branch
                     u32 next = currentAddr + 4;
                     block.successors.insert(next);
                     m_workList.push(next);
                }
            }
            break;
        }

        currentAddr += 4;
        
        // Check if we hit another block's start
        if (m_visited.count(currentAddr) && !block.instructions.empty()) {
            block.endAddr = currentAddr - 4;
            block.successors.insert(currentAddr);
            break;
        }
    }

    block.isAnalyzed = true;
    m_cfg.addBlock(block);
}

} // namespace gcrecomp
