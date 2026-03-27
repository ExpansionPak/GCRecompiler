#pragma once

#include <gcrecomp/analysis/ir.h>
#include <map>

namespace gcrecomp {

class Optimizer {
public:
    Optimizer() = default;

    // Optimizes an IR block (constant propagation/folding)
    void optimizeBlock(IRBlock& block);

private:
    void constantPropagation(IRBlock& block);
};

} // namespace gcrecomp
