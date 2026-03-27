#pragma once

#include <gcrecomp/types.h>
#include <gcrecomp/analysis/instruction.h>
#include <gcrecomp/loader/binary.h>
#include <capstone/capstone.h>
#include <memory>

namespace gcrecomp {

class Disassembler {
public:
    Disassembler();
    ~Disassembler();

    bool disassemble(const Binary& binary, u32 addr, Instruction& outInstr);

private:
    csh m_handle;
    bool m_valid = false;
};

} // namespace gcrecomp
