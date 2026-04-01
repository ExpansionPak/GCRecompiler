#pragma once

#include <gcrecomp/types.h>
#include <gcrecomp/analysis/ir.h>
#include <gcrecomp/analysis/instruction.h>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <memory>
#include <optional>

namespace gcrecomp {

enum class BlockType {
    Normal,
    Return,
    Call,
    Invalid
};

struct BasicBlock {
    struct LocalJumpTable {
        u32 indexRegister = 0;
        u32 defaultTarget = 0;
        size_t patternStartInstructionIndex = 0;
        std::vector<u32> targets;
    };

    u32 startAddr;
    u32 endAddr;
    std::vector<Instruction> instructions;
    std::vector<IRInstruction> irInstructions;
    std::set<u32> successors;
    std::set<u32> predecessors;
    BlockType type = BlockType::Normal;
    bool isAnalyzed = false;
    std::optional<LocalJumpTable> localJumpTable;
};

struct Function {
    u32 startAddr;
    std::string name;
    std::set<u32> blocks; // Addresses of basic blocks in this function
};

class ControlFlowGraph {
public:
    void addBlock(const BasicBlock& block);
    BasicBlock* getBlock(u32 addr);
    const BasicBlock* getBlock(u32 addr) const;
    const std::map<u32, BasicBlock>& getBlocks() const { return m_blocks; }

    void addFunction(const Function& func);
    Function* getFunction(u32 addr);
    const Function* getFunction(u32 addr) const;
    const std::map<u32, Function>& getFunctions() const { return m_functions; }

    void exportDot(const std::string& path) const;

private:
    std::map<u32, BasicBlock> m_blocks;
    std::map<u32, Function> m_functions;
};

} // namespace gcrecomp
