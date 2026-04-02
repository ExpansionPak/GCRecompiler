#include <gcrecomp/analysis/lifter.h>
#include <gcrecomp/log.h>

namespace gcrecomp {

namespace {

u32 decodeCrField(u32 raw) {
    return (raw >> 23) & 0x7;
}

u32 decodeSpr(u32 ra, u32 rb) {
    return (rb << 5) | ra;
}

IROperand gprOrZeroBase(u32 ra) {
    return ra == 0 ? IROperand::Imm(0) : IROperand::Reg(ra);
}

IROperand branchRegisterOperand(BranchRegisterTarget target) {
    switch (target) {
        case BranchRegisterTarget::LinkRegister:
            return IROperand::Special(IRSpecialRegister::LinkRegister);
        case BranchRegisterTarget::CountRegister:
            return IROperand::Special(IRSpecialRegister::CountRegister);
        default:
            return {};
    }
}

void emitCompare(std::vector<IRInstruction>& out, IROp op, u32 crField,
                 const IROperand& lhs, const IROperand& rhs) {
    out.push_back({ op, { IROperand::Imm(crField), lhs, rhs } });
}

void maybeEmitRecordCompare(std::vector<IRInstruction>& out, u32 raw, u32 destReg) {
    if ((raw & 1u) == 0u) {
        return;
    }
    emitCompare(out, IROp::Cmp, 0, IROperand::Reg(destReg), IROperand::Imm(0));
}

void emitBranchInstruction(const Instruction& instr, std::vector<IRInstruction>& out) {
    if (instr.type == InstructionType::Return) {
        out.push_back({ instr.isInterruptReturn ? IROp::Rfi : IROp::Return });
        return;
    }

    IROperand target = instr.branchRegisterTarget == BranchRegisterTarget::None
        ? IROperand::Addr(instr.branchTarget)
        : branchRegisterOperand(instr.branchRegisterTarget);

    switch (instr.type) {
        case InstructionType::Call:
            out.push_back({
                instr.branchRegisterTarget == BranchRegisterTarget::None ? IROp::Call : IROp::CallIndirect,
                { target }
            });
            break;
        case InstructionType::Branch:
            out.push_back({
                instr.branchRegisterTarget == BranchRegisterTarget::None ? IROp::Branch : IROp::BranchIndirect,
                { target }
            });
            break;
        case InstructionType::ConditionalBranch:
            out.push_back({ IROp::BranchCond, { IROperand::Imm(instr.bo), IROperand::Imm(instr.bi), target } });
            break;
        default:
            break;
    }
}

} // namespace

IRBlock Lifter::liftBlock(const BasicBlock& block) {
    IRBlock irBlock;
    irBlock.startAddr = block.startAddr;
    size_t instructionCount = block.instructions.size();

    if (block.localJumpTable &&
        block.localJumpTable->patternStartInstructionIndex < instructionCount) {
        instructionCount = block.localJumpTable->patternStartInstructionIndex;
    }

    for (size_t instrIndex = 0; instrIndex < instructionCount; ++instrIndex) {
        const auto& instr = block.instructions[instrIndex];
        const size_t oldSize = irBlock.instructions.size();
        liftInstruction(instr, irBlock.instructions);
        for (size_t i = oldSize; i < irBlock.instructions.size(); ++i) {
            irBlock.instructions[i].address = instr.address;
        }
    }

    if (block.localJumpTable) {
        IRInstruction tail(IROp::BranchTable);
        tail.address = block.instructions.empty() ? block.startAddr : block.instructions.back().address;
        tail.operands.push_back(IROperand::Reg(block.localJumpTable->indexRegister));
        tail.operands.push_back(IROperand::Addr(block.localJumpTable->defaultTarget));
        for (u32 target : block.localJumpTable->targets) {
            tail.operands.push_back(IROperand::Addr(target));
        }
        irBlock.instructions.push_back(std::move(tail));
    }

    return irBlock;
}

void Lifter::liftInstruction(const Instruction& instr, std::vector<IRInstruction>& out) {
    const u32 raw = instr.raw;
    const u32 op = raw >> 26;
    const u32 rd = (raw >> 21) & 0x1F;
    const u32 ra = (raw >> 16) & 0x1F;
    const u32 rb = (raw >> 11) & 0x1F;
    const u16 imm = raw & 0xFFFF;
    const s16 simm = static_cast<s16>(imm);
    const u32 rs = rd;

    switch (op) {
        case 3: // twi
            out.push_back({ IROp::Trap, { IROperand::Imm(rd), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 4: { // Paired-single arithmetic subset
            const u32 xop = (raw >> 1) & 0x1F;
            const u32 frc = (raw >> 6) & 0x1F;
            switch (xop) {
                case 18: out.push_back({ IROp::FDiv,  { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb) } }); break;
                case 20: out.push_back({ IROp::FSub,  { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb) } }); break;
                case 21: out.push_back({ IROp::FAdd,  { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb) } }); break;
                case 23: out.push_back({ IROp::FSel,  { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb), IROperand::FReg(frc) } }); break;
                case 25: out.push_back({ IROp::FMul,  { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb) } }); break;
                case 28: out.push_back({ IROp::FMsub, { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb), IROperand::FReg(frc) } }); break;
                case 29: out.push_back({ IROp::FMadd, { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb), IROperand::FReg(frc) } }); break;
                default:
                    break;
            }
            break;
        }

        case 7: // mulli
            out.push_back({ IROp::Mul, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 8: // subfic
            out.push_back({ IROp::Sub, { IROperand::Reg(rd), IROperand::Imm(static_cast<u32>(simm)), IROperand::Reg(ra) } });
            break;

        case 10: // cmpli
            emitCompare(out, IROp::Cmpl, decodeCrField(raw), IROperand::Reg(ra), IROperand::Imm(imm));
            break;

        case 11: // cmpi
            emitCompare(out, IROp::Cmp, decodeCrField(raw), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)));
            break;

        case 12: // addic
            out.push_back({ IROp::Addic, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 13: // addic.
            out.push_back({ IROp::Addic, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            emitCompare(out, IROp::Cmp, 0, IROperand::Reg(rd), IROperand::Imm(0));
            break;

        case 14: // addi
            if (ra == 0) {
                out.push_back({ IROp::SetImm, { IROperand::Reg(rd), IROperand::Imm(static_cast<u32>(simm)) } });
            } else {
                out.push_back({ IROp::Add, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            }
            break;

        case 15: // addis
            if (ra == 0) {
                out.push_back({ IROp::SetImm, { IROperand::Reg(rd), IROperand::Imm(static_cast<u32>(simm) << 16) } });
            } else {
                out.push_back({ IROp::Add, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm) << 16) } });
            }
            break;

        case 16: // bc / bcl
        case 18: // b / bl
            emitBranchInstruction(instr, out);
            break;

        case 17: // sc
            out.push_back({ IROp::Syscall, {} });
            break;

        case 19: { // bclr/bcctr/CR logical ops
            if (instr.isBranch) {
                emitBranchInstruction(instr, out);
                break;
            }

            const u32 xop = (raw >> 1) & 0x3FF;
            switch (xop) {
                case 33:  out.push_back({ IROp::CrNor, { IROperand::Imm(rd), IROperand::Imm(ra), IROperand::Imm(rb) } }); break;
                case 193: out.push_back({ IROp::CrXor, { IROperand::Imm(rd), IROperand::Imm(ra), IROperand::Imm(rb) } }); break;
                case 257: out.push_back({ IROp::CrAnd, { IROperand::Imm(rd), IROperand::Imm(ra), IROperand::Imm(rb) } }); break;
                case 449: out.push_back({ IROp::CrOr,  { IROperand::Imm(rd), IROperand::Imm(ra), IROperand::Imm(rb) } }); break;
                default:
                    break;
            }
            break;
        }

        case 20: { // rlwimi
            const u32 sh = (raw >> 11) & 0x1F;
            const u32 mb = (raw >> 6) & 0x1F;
            const u32 me = (raw >> 1) & 0x1F;
            out.push_back({ IROp::Rlwimi, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Imm(sh), IROperand::Imm(mb), IROperand::Imm(me) } });
            maybeEmitRecordCompare(out, raw, ra);
            break;
        }

        case 21: { // rlwinm
            const u32 sh = (raw >> 11) & 0x1F;
            const u32 mb = (raw >> 6) & 0x1F;
            const u32 me = (raw >> 1) & 0x1F;
            out.push_back({ IROp::Rol, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Imm(sh) } });
            out.push_back({ IROp::Mask, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(mb), IROperand::Imm(me) } });
            maybeEmitRecordCompare(out, raw, ra);
            break;
        }

        case 23: { // rlwnm
            const u32 mb = (raw >> 6) & 0x1F;
            const u32 me = (raw >> 1) & 0x1F;
            out.push_back({ IROp::Rol, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Reg(rb) } });
            out.push_back({ IROp::Mask, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(mb), IROperand::Imm(me) } });
            maybeEmitRecordCompare(out, raw, ra);
            break;
        }

        case 24: // ori
            out.push_back({ IROp::Or, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Imm(imm) } });
            break;

        case 25: // oris
            out.push_back({ IROp::Or, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Imm(imm << 16) } });
            break;

        case 26: // xori
            out.push_back({ IROp::Xor, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Imm(imm) } });
            break;

        case 27: // xoris
            out.push_back({ IROp::Xor, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Imm(imm << 16) } });
            break;

        case 28: // andi.
            out.push_back({ IROp::And, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Imm(imm) } });
            emitCompare(out, IROp::Cmp, 0, IROperand::Reg(ra), IROperand::Imm(0));
            break;

        case 29: // andis.
            out.push_back({ IROp::And, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Imm(imm << 16) } });
            emitCompare(out, IROp::Cmp, 0, IROperand::Reg(ra), IROperand::Imm(0));
            break;

        case 31: { // XO / X-form
            const u32 xop = (raw >> 1) & 0x3FF;
            switch (xop) {
                case 0:   emitCompare(out, IROp::Cmp, decodeCrField(raw), IROperand::Reg(ra), IROperand::Reg(rb)); break;
                case 4:   out.push_back({ IROp::Trap, { IROperand::Imm(rd), IROperand::Reg(ra), IROperand::Reg(rb) } }); break;
                case 11:  out.push_back({ IROp::MulHighU, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, rd); break;
                case 19:  out.push_back({ IROp::Mfcr, { IROperand::Reg(rd) } }); break;
                case 20:  out.push_back({ IROp::ReservationLoad, { IROperand::Reg(rd), gprOrZeroBase(ra), IROperand::Reg(rb) } }); break;
                case 23:  out.push_back({ IROp::Load32, { IROperand::Reg(rd), gprOrZeroBase(ra), IROperand::Reg(rb) } }); break;
                case 24:  out.push_back({ IROp::Shl, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 26:  out.push_back({ IROp::Cntlzw, { IROperand::Reg(ra), IROperand::Reg(rs) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 28:  out.push_back({ IROp::And, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 32:  emitCompare(out, IROp::Cmpl, decodeCrField(raw), IROperand::Reg(ra), IROperand::Reg(rb)); break;
                case 40:  out.push_back({ IROp::Sub, { IROperand::Reg(rd), IROperand::Reg(rb), IROperand::Reg(ra) } });
                          maybeEmitRecordCompare(out, raw, rd); break;
                case 54:  out.push_back({ IROp::Sync, {} }); break; // dcbst
                case 55:  out.push_back({ IROp::Load32, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Reg(rb) } });
                          out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Reg(rb) } }); break;
                case 60:  out.push_back({ IROp::Andc, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 83:  out.push_back({ IROp::Mfmsr, { IROperand::Reg(rd) } }); break;
                case 86:  out.push_back({ IROp::Sync, {} }); break; // dcbf
                case 87:  out.push_back({ IROp::Load8, { IROperand::Reg(rd), gprOrZeroBase(ra), IROperand::Reg(rb) } }); break;
                case 75:  out.push_back({ IROp::MulHighS, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, rd); break;
                case 104: out.push_back({ IROp::Sub, { IROperand::Reg(rd), IROperand::Imm(0), IROperand::Reg(ra) } });
                          maybeEmitRecordCompare(out, raw, rd); break; // neg
                case 119: out.push_back({ IROp::Load8, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Reg(rb) } });
                          out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Reg(rb) } }); break;
                case 124: out.push_back({ IROp::Nor, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 144: out.push_back({ IROp::Mtcrf, { IROperand::Imm((raw >> 12) & 0xFF), IROperand::Reg(rs) } }); break;
                case 146: out.push_back({ IROp::Mtmsr, { IROperand::Reg(rs) } }); break;
                case 150: out.push_back({ IROp::ReservationStore, { IROperand::Reg(rs), gprOrZeroBase(ra), IROperand::Reg(rb) } }); break;
                case 151: out.push_back({ IROp::Store32, { IROperand::Reg(rs), gprOrZeroBase(ra), IROperand::Reg(rb) } }); break;
                case 183: out.push_back({ IROp::Store32, { IROperand::Reg(rs), IROperand::Reg(ra), IROperand::Reg(rb) } });
                          out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Reg(rb) } }); break;
                case 215: out.push_back({ IROp::Store8, { IROperand::Reg(rs), gprOrZeroBase(ra), IROperand::Reg(rb) } }); break;
                case 235: out.push_back({ IROp::Mul, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, rd); break;
                case 246: out.push_back({ IROp::Sync, {} }); break; // dcbtst
                case 247: out.push_back({ IROp::Store8, { IROperand::Reg(rs), IROperand::Reg(ra), IROperand::Reg(rb) } });
                          out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Reg(rb) } }); break;
                case 266: out.push_back({ IROp::Add, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, rd); break;
                case 278: out.push_back({ IROp::Sync, {} }); break; // dcbt
                case 279: out.push_back({ IROp::Load16, { IROperand::Reg(rd), gprOrZeroBase(ra), IROperand::Reg(rb) } }); break;
                case 311: out.push_back({ IROp::Load16, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Reg(rb) } });
                          out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Reg(rb) } }); break;
                case 316: out.push_back({ IROp::Xor, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 339:
                case 371:
                    out.push_back({ IROp::Mfspr, { IROperand::Reg(rd), IROperand::Imm(decodeSpr(ra, rb)) } });
                    break;
                case 343: out.push_back({ IROp::Load16a, { IROperand::Reg(rd), gprOrZeroBase(ra), IROperand::Reg(rb) } }); break;
                case 375: out.push_back({ IROp::Load16a, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Reg(rb) } });
                          out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Reg(rb) } }); break;
                case 407: out.push_back({ IROp::Store16, { IROperand::Reg(rs), gprOrZeroBase(ra), IROperand::Reg(rb) } }); break;
                case 412: out.push_back({ IROp::Orc, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 439: out.push_back({ IROp::Store16, { IROperand::Reg(rs), IROperand::Reg(ra), IROperand::Reg(rb) } });
                          out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Reg(rb) } }); break;
                case 444: out.push_back({ IROp::Or, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 467: out.push_back({ IROp::Mtspr, { IROperand::Imm(decodeSpr(ra, rb)), IROperand::Reg(rs) } }); break;
                case 476: out.push_back({ IROp::Nand, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 459: out.push_back({ IROp::DivU, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, rd); break;
                case 491: out.push_back({ IROp::DivS, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, rd); break;
                case 536: out.push_back({ IROp::Shr, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 598: out.push_back({ IROp::Sync, {} }); break;
                case 792: out.push_back({ IROp::Sar, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Reg(rb) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 824: out.push_back({ IROp::Sar, { IROperand::Reg(ra), IROperand::Reg(rs), IROperand::Imm(rb) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 854: out.push_back({ IROp::Sync, {} }); break; // eieio
                case 922: out.push_back({ IROp::Extsh, { IROperand::Reg(ra), IROperand::Reg(rs) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 954: out.push_back({ IROp::Extsb, { IROperand::Reg(ra), IROperand::Reg(rs) } });
                          maybeEmitRecordCompare(out, raw, ra); break;
                case 982: out.push_back({ IROp::Isync, {} }); break; // icbi
                case 1014: out.push_back({ IROp::Isync, {} }); break; // dcbz approximation
                default:
                    break;
            }
            break;
        }

        case 32: // lwz
            out.push_back({ IROp::Load32, { IROperand::Reg(rd), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 33: // lwzu
            out.push_back({ IROp::Load32, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 34: // lbz
            out.push_back({ IROp::Load8, { IROperand::Reg(rd), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 35: // lbzu
            out.push_back({ IROp::Load8, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 36: // stw
            out.push_back({ IROp::Store32, { IROperand::Reg(rs), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 37: // stwu
            out.push_back({ IROp::Store32, { IROperand::Reg(rs), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 38: // stb
            out.push_back({ IROp::Store8, { IROperand::Reg(rs), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 39: // stbu
            out.push_back({ IROp::Store8, { IROperand::Reg(rs), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 40: // lhz
            out.push_back({ IROp::Load16, { IROperand::Reg(rd), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 41: // lhzu
            out.push_back({ IROp::Load16, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 42: // lha
            out.push_back({ IROp::Load16a, { IROperand::Reg(rd), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 43: // lhau
            out.push_back({ IROp::Load16a, { IROperand::Reg(rd), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 44: // sth
            out.push_back({ IROp::Store16, { IROperand::Reg(rs), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 45: // sthu
            out.push_back({ IROp::Store16, { IROperand::Reg(rs), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 46: // lmw
            out.push_back({ IROp::Lmw, { IROperand::Reg(rd), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 47: // stmw
            out.push_back({ IROp::Stmw, { IROperand::Reg(rs), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 48: // lfs
            out.push_back({ IROp::LoadFloat, { IROperand::FReg(rd), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 49: // lfsu
            out.push_back({ IROp::LoadFloat, { IROperand::FReg(rd), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 50: // lfd
            out.push_back({ IROp::LoadDouble, { IROperand::FReg(rd), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 51: // lfdu
            out.push_back({ IROp::LoadDouble, { IROperand::FReg(rd), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 52: // stfs
            out.push_back({ IROp::StoreFloat, { IROperand::FReg(rs), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 53: // stfsu
            out.push_back({ IROp::StoreFloat, { IROperand::FReg(rs), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 54: // stfd
            out.push_back({ IROp::StoreDouble, { IROperand::FReg(rs), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 55: // stfdu
            out.push_back({ IROp::StoreDouble, { IROperand::FReg(rs), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 56: // psq_l
            out.push_back({ IROp::LoadDouble, { IROperand::FReg(rd), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 57: // psq_lu
            out.push_back({ IROp::LoadDouble, { IROperand::FReg(rd), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 59: { // Single precision FPU subset
            const u32 xop = (raw >> 1) & 0x1F;
            const u32 frc = (raw >> 6) & 0x1F;
            switch (xop) {
                case 18: out.push_back({ IROp::FDiv,  { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb) } }); break;
                case 20: out.push_back({ IROp::FSub,  { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb) } }); break;
                case 21: out.push_back({ IROp::FAdd,  { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb) } }); break;
                case 25: out.push_back({ IROp::FMul,  { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(frc) } }); break;
                case 28: out.push_back({ IROp::FMsub, { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(frc), IROperand::FReg(rb) } }); break;
                case 29: out.push_back({ IROp::FMadd, { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(frc), IROperand::FReg(rb) } }); break;
                default:
                    break;
            }
            break;
        }

        case 60: // psq_st
            out.push_back({ IROp::StoreDouble, { IROperand::FReg(rs), gprOrZeroBase(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 61: // psq_stu
            out.push_back({ IROp::StoreDouble, { IROperand::FReg(rs), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            out.push_back({ IROp::Add, { IROperand::Reg(ra), IROperand::Reg(ra), IROperand::Imm(static_cast<u32>(simm)) } });
            break;

        case 63: { // Double precision FPU / misc subset
            const u32 xop = (raw >> 1) & 0x3FF;
            const u32 frc = (raw >> 6) & 0x1F;

            if ((xop & 0x1F) == 18) { out.push_back({ IROp::FDiv, { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb) } }); break; }
            if ((xop & 0x1F) == 20) { out.push_back({ IROp::FSub, { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb) } }); break; }
            if ((xop & 0x1F) == 21) { out.push_back({ IROp::FAdd, { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb) } }); break; }
            if ((xop & 0x1F) == 25) { out.push_back({ IROp::FMul, { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(frc) } }); break; }
            if ((xop & 0x1F) == 28) { out.push_back({ IROp::FMsub, { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(frc), IROperand::FReg(rb) } }); break; }
            if ((xop & 0x1F) == 29) { out.push_back({ IROp::FMadd, { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(frc), IROperand::FReg(rb) } }); break; }

            switch (xop) {
                case 0:  out.push_back({ IROp::FCmpu, { IROperand::Imm(decodeCrField(raw)), IROperand::FReg(ra), IROperand::FReg(rb) } }); break;
                case 12: out.push_back({ IROp::Frsp, { IROperand::FReg(rd), IROperand::FReg(rb) } }); break;
                case 15: out.push_back({ IROp::Fctiw, { IROperand::FReg(rd), IROperand::FReg(rb) } }); break;
                case 23: out.push_back({ IROp::FSel, { IROperand::FReg(rd), IROperand::FReg(ra), IROperand::FReg(rb), IROperand::FReg(frc) } }); break;
                case 32: out.push_back({ IROp::FCmpo, { IROperand::Imm(decodeCrField(raw)), IROperand::FReg(ra), IROperand::FReg(rb) } }); break;
                case 40: out.push_back({ IROp::FNeg, { IROperand::FReg(rd), IROperand::FReg(rb) } }); break;
                case 72: out.push_back({ IROp::SetReg, { IROperand::FReg(rd), IROperand::FReg(rb) } }); break; // fmr
                case 136: out.push_back({ IROp::Fnabs, { IROperand::FReg(rd), IROperand::FReg(rb) } }); break;
                case 264: out.push_back({ IROp::FAbs, { IROperand::FReg(rd), IROperand::FReg(rb) } }); break;
                default:
                    break;
            }
            break;
        }

        default:
            break;
    }
}

} // namespace gcrecomp
