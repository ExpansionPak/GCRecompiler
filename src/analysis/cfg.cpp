#include <gcrecomp/analysis/cfg.h>
#include <fstream>
#include <sstream>

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

const BasicBlock* ControlFlowGraph::getBlock(u32 addr) const {
    auto it = m_blocks.find(addr);
    if (it != m_blocks.end()) {
        return &it->second;
    }
    return nullptr;
}

void ControlFlowGraph::addFunction(const Function& func) {
    m_functions[func.startAddr] = func;
}

Function* ControlFlowGraph::getFunction(u32 addr) {
    auto it = m_functions.find(addr);
    if (it != m_functions.end()) {
        return &it->second;
    }
    return nullptr;
}

const Function* ControlFlowGraph::getFunction(u32 addr) const {
    auto it = m_functions.find(addr);
    if (it != m_functions.end()) {
        return &it->second;
    }
    return nullptr;
}

void ControlFlowGraph::exportDot(const std::string& path) const {
    std::ofstream out(path);
    if (!out.is_open()) return;

    out << "digraph CFG {" << std::endl;
    out << "  node [shape=box, fontname=\"Courier\"];" << std::endl;

    for (const auto& [addr, block] : m_blocks) {
        std::stringstream label;
        label << "0x" << std::hex << addr << "\\n";
        if (block.instructions.size() > 0) {
            for (const auto& instr : block.instructions) {
               label << instr.mnemonic << " " << instr.operands << "\\n";
            }
        }
        
        out << "  node_" << std::hex << addr << " [label=\"" << label.str() << "\"];" << std::endl;

        for (u32 succ : block.successors) {
            out << "  node_" << std::hex << addr << " -> node_" << std::hex << succ << ";" << std::endl;
        }
    }

    out << "}" << std::endl;
}

} // namespace gcrecomp
