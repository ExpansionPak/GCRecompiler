#include <gcrecomp/analysis/disasm.h>
#include <gcrecomp/log.h>
#include <capstone/ppc.h>

namespace gcrecomp {

Disassembler::Disassembler() {
    if (cs_open(CS_ARCH_PPC, static_cast<cs_mode>(CS_MODE_32 | CS_MODE_BIG_ENDIAN), &m_handle) != CS_ERR_OK) {
        LOG_ERROR("Failed to initialize Capstone");
    } else {
        cs_option(m_handle, CS_OPT_DETAIL, CS_OPT_ON);
        m_valid = true;
    }
}

Disassembler::~Disassembler() {
    if (m_valid) {
        cs_close(&m_handle);
    }
}

bool Disassembler::disassemble(const Binary& binary, u32 addr, Instruction& outInstr) {
    if (!m_valid) return false;

    if (!binary.isValidAddress(addr)) return false;

    u32 raw = binary.read32(addr);
    u8 bytes[4];
    bytes[0] = static_cast<u8>(raw >> 24);
    bytes[1] = static_cast<u8>(raw >> 16);
    bytes[2] = static_cast<u8>(raw >> 8);
    bytes[3] = static_cast<u8>(raw);

    cs_insn* insn;
    size_t count = cs_disasm(m_handle, bytes, 4, addr, 1, &insn);

    if (count > 0) {
        outInstr.address = addr;
        outInstr.raw = raw;
        outInstr.mnemonic = insn[0].mnemonic;
        outInstr.operands = insn[0].op_str;
        outInstr.type = InstructionType::Compute;

        // Analyze instruction for control flow
        cs_ppc* ppc = &insn[0].detail->ppc;
        
        // Simple branch detection
        if (insn[0].id == PPC_INS_B || insn[0].id == PPC_INS_BA || 
            insn[0].id == PPC_INS_BL || insn[0].id == PPC_INS_BLA ||
            insn[0].id == PPC_INS_BC || insn[0].id == PPC_INS_BCA ||
            insn[0].id == PPC_INS_BCL || insn[0].id == PPC_INS_BCLA) {
            
            outInstr.isBranch = true;
            outInstr.type = InstructionType::Branch;

            if (insn[0].id == PPC_INS_BL || insn[0].id == PPC_INS_BLA ||
                insn[0].id == PPC_INS_BCL || insn[0].id == PPC_INS_BCLA) {
                outInstr.isLink = true;
                outInstr.type = InstructionType::Call;
            }

            // Find branch target
            for (int i = 0; i < ppc->op_count; i++) {
                if (ppc->operands[i].type == PPC_OP_IMM) {
                    outInstr.branchTarget = static_cast<u32>(ppc->operands[i].imm);
                    break;
                }
            }
        } else if (insn[0].id == PPC_INS_BLR) {
            outInstr.isBranch = true;
            outInstr.type = InstructionType::Return;
        } else if (insn[0].id == PPC_INS_BCTR || insn[0].id == PPC_INS_BCTRL) {
            outInstr.isBranch = true;
            outInstr.type = InstructionType::Call; // Indirect call
        }

        cs_free(insn, count);
        return true;
    }

    return false;
}

} // namespace gcrecomp
