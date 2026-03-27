#include <gcrecomp/analysis/lifter.h>
#include <gcrecomp/log.h>

namespace gcrecomp {

IRBlock Lifter::liftBlock(const BasicBlock& block) {
    IRBlock irBlock;
    irBlock.startAddr = block.startAddr;

    for (const auto& instr : block.instructions) {
        liftInstruction(instr, irBlock.instructions);
    }

    return irBlock;
}

void Lifter::liftInstruction(const Instruction& instr, std::vector<IRInstruction>& out) {
    u32 raw = instr.raw;
    u32 op = raw >> 26;
    u32 rd = (raw >> 21) & 0x1F;
    u32 ra = (raw >> 16) & 0x1F;
    u32 rb = (raw >> 11) & 0x1F;
    u16 imm = raw & 0xFFFF;
    s16 simm = (s16)imm;

    u32 rs = rd; // RD and RS occupy the same bits (6-10)

    // Simplified lifting for core PPC instructions
    switch (op) {
        case 10: // cmpli
            out.push_back({ IROp::Cmpl, { IROperand::Reg(ra), IROperand::Imm(imm) } });
            break;
        case 11: // cmpi
            out.push_back({ IROp::Cmp, { IROperand::Reg(ra), IROperand::Imm((u32)simm) } });
            break;
        case 14: // addi
            if (ra == 0) { // li
                out.push_back({ IROp::SetImm, { IROperand::Reg(rd), IROperand::Imm((u32)simm) } });
            } else {
                out.push_back({ IROp::Add, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm((u32)simm) } });
            }
            break;
        case 15: // addis
            if (ra == 0) { // lis
                out.push_back({ IROp::SetImm, { IROperand::Reg(rd), IROperand::Imm((u32)simm << 16) } });
            } else {
                out.push_back({ IROp::Add, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm((u32)simm << 16) } });
            }
            break;
        case 21: { // rlwinm
            u32 sh = (raw >> 11) & 0x1F;
            u32 mb = (raw >> 6) & 0x1F;
            u32 me = (raw >> 1) & 0x1F;
            // Decompose or use specialized op
            out.push_back({ IROp::Rol, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm(sh) } });
            out.push_back({ IROp::Mask, { IROperand::Reg(rd), IROperand::Reg(rd), IROperand::Imm(mb), IROperand::Imm(me) } });
            break;
        }
        case 24: // ori
            out.push_back({ IROp::Or, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Imm(imm) } });
            break;
        case 28: // andi.
            out.push_back({ IROp::And, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Imm(imm) } });
            break;
        case 31: { // Integer X-form
            u32 xop = (raw >> 1) & 0x3FF;
            switch (xop) {
                case 266: // add
                    out.push_back({ IROp::Add, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Reg(rb) } });
                    break;
                case 444: // or
                    out.push_back({ IROp::Or, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Reg(rb) } });
                    break;
                case 28: // and
                    out.push_back({ IROp::And, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Reg(rb) } });
                    break;
            }
            break;
        }
        case 32: // lwz
            out.push_back({ IROp::Load32, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm((u32)simm) } });
            break;
        case 36: // stw
            out.push_back({ IROp::Store32, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm((u32)simm) } });
            break;
        case 48: // lfs
            out.push_back({ IROp::LoadFloat, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm((u32)simm) } });
            break;
        case 52: // stfs
            out.push_back({ IROp::StoreFloat, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm((u32)simm) } });
            break;
        
        // Control flow is handled by the block structure, 
        // but we can lift them for completeness or to help the emitter.
        case 18: // b/bl
            if (instr.isLink) {
                 out.push_back({ IROp::Call, { IROperand::Addr(instr.branchTarget) } });
            } else {
                 out.push_back({ IROp::Branch, { IROperand::Addr(instr.branchTarget) } });
            }
            break;
        case 19: { // rfi, bclr, etc.
             u32 xop = (raw >> 1) & 0x3FF;
             if (xop == 16) { // bclr
                 out.push_back({ IROp::Return, {} });
             }
             break;
        }
    }
}

} // namespace gcrecomp
