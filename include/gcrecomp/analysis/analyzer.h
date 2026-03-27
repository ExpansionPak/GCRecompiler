#pragma once

#include <gcrecomp/types.h>
#include <gcrecomp/loader/binary.h>
#include <gcrecomp/analysis/disasm.h>
#include <gcrecomp/analysis/cfg.h>
#include <gcrecomp/analysis/lifter.h>
#include <gcrecomp/analysis/optimizer.h>
#include <queue>
#include <set>

namespace gcrecomp {

class Analyzer {
public:
    Analyzer(const Binary& binary);
    
    void analyze(u32 entryPoint);
    const ControlFlowGraph& getCfg() const { return m_cfg; }

private:
    const Binary& m_binary;
    Disassembler m_disasm;
    ControlFlowGraph m_cfg;
    Lifter m_lifter;
    Optimizer m_optimizer;
    std::set<u32> m_visited;
    std::queue<u32> m_workList;

    void analyzeBlock(u32 startAddr);
};

} // namespace gcrecomp
