#pragma once

#include <gcrecomp/types.h>
#include <gcrecomp/analysis/instruction.h>
#include <vector>
#include <map>
#include <set>
#include <memory>

namespace gcrecomp {

enum class BlockType {
    Normal,
    Return,
    Call,
    Invalid
};

struct BasicBlock {
    u32 startAddr;
    u32 endAddr;
    std::vector<Instruction> instructions;
    std::set<u32> successors;
    std::set<u32> predecessors;
    BlockType type = BlockType::Normal;
    bool isAnalyzed = false;
};

class ControlFlowGraph {
public:
    void addBlock(const BasicBlock& block);
    BasicBlock* getBlock(u32 addr);
    const std::map<u32, BasicBlock>& getBlocks() const { return m_blocks; }

private:
    std::map<u32, BasicBlock> m_blocks;
};

} // namespace gcrecomp
