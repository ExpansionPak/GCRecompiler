#pragma once

#include <gcrecomp/types.h>
#include <gcrecomp/loader/binary.h>
#include <gcrecomp/analysis/disasm.h>
#include <gcrecomp/analysis/cfg.h>
#include <gcrecomp/analysis/lifter.h>
#include <gcrecomp/analysis/optimizer.h>
#include <queue>
#include <set>
#include <string>
#include <vector>
#include <optional>

namespace gcrecomp {

class Analyzer {
public:
    Analyzer(const Binary& binary);
    
    void analyze(u32 entryPoint);
    void emitAllFunctions(const std::string& outputDir);
    const ControlFlowGraph& getCfg() const { return m_cfg; }

private:
    const Binary& m_binary;
    Disassembler m_disasm;
    ControlFlowGraph m_cfg;
    Lifter m_lifter;
    Optimizer m_optimizer;
    std::set<u32> m_visitedBlocks;
    std::set<u32> m_discoveredFunctions;
    std::set<u32> m_analyzedFunctions;

    bool registerFunction(u32 addr, const std::string& name);
    void analyzeFunction(u32 entryAddr);
    void analyzeBlock(u32 startAddr, Function& currentFunc, std::set<u32>& pendingBlocks);
};

} // namespace gcrecomp
