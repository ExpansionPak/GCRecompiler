#include <gcrecomp/analysis/cfg.h>

namespace gcrecomp {

void ControlFlowGraph::addBlock(const BasicBlock& block) {
    m_blocks[block.startAddr] = block;
}

BasicBlock* ControlFlowGraph::getBlock(u32 addr) {
    auto it = m_blocks.find(addr);
    if (it != m_blocks.end()) {
        return &it->second;
    }
    return nullptr;
}

} // namespace gcrecomp
