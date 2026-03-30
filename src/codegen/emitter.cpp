#include <gcrecomp/codegen/emitter.h>
#include <gcrecomp/analysis/cfg.h>
#include <gcrecomp/analysis/ir.h>
#include <gcrecomp/log.h>
#include <recomp_runtime.h>
#include <iomanip>
#include <sstream>

namespace gcrecomp {

namespace {

std::string formatHex(u32 value) {
    std::stringstream ss;
    ss << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return ss.str();
}

std::string formatImmediate(u32 value) {
    std::stringstream ss;
    ss << "0x" << std::hex << value;
    return ss.str();
}

} // namespace

Emitter::Emitter() {}
Emitter::~Emitter() {}

std::string Emitter::emitFunction(const Function& func, const ControlFlowGraph& cfg) {
    LOG_DEBUG("Emitter::emitFunction for %s", func.name.c_str());
    std::stringstream ss;
    ss << "// Function: " << func.name << "\n";
    ss << "void fn_0x" << std::hex << std::setw(8) << std::setfill('0') << func.startAddr << "(CPUContext* ctx) {\n";

    std::set<u32> emittedBlocks = func.blocks;
    std::set<u32> pendingBlocks;
    std::set<u32> localResumeTargets;

    auto queueExtraBlock = [&](u32 addr) {
        if (addr == 0 || emittedBlocks.count(addr) != 0 || cfg.getBlock(addr) == nullptr) {
            return;
        }
        pendingBlocks.insert(addr);
    };

    for (u32 blockAddr : func.blocks) {
        const BasicBlock* block = cfg.getBlock(blockAddr);
        if (!block || !block->localJumpTable) {
            continue;
        }

        queueExtraBlock(block->localJumpTable->defaultTarget);
        for (u32 target : block->localJumpTable->targets) {
            queueExtraBlock(target);
        }
    }

    while (!pendingBlocks.empty()) {
        const u32 blockAddr = *pendingBlocks.begin();
        pendingBlocks.erase(pendingBlocks.begin());
        if (!emittedBlocks.insert(blockAddr).second) {
            continue;
        }

        const BasicBlock* block = cfg.getBlock(blockAddr);
        if (!block) {
            continue;
        }

        for (u32 successor : block->successors) {
            if (successor == 0 || emittedBlocks.count(successor) != 0) {
                continue;
            }
            if (cfg.getBlock(successor) == nullptr) {
                continue;
            }
            pendingBlocks.insert(successor);
        }
    }

    for (u32 blockAddr : emittedBlocks) {
        const BasicBlock* block = cfg.getBlock(blockAddr);
        if (!block) {
            continue;
        }

        for (const auto& instr : block->irInstructions) {
            if (instr.op == IROp::Call &&
                !instr.operands.empty() &&
                emittedBlocks.count(instr.operands[0].value) != 0 &&
                instr.address != 0) {
                localResumeTargets.insert(instr.address + 4);
            }
        }
    }

    ss << "    switch (ctx->pc) {\n";
    for (u32 blockAddr : emittedBlocks) {
        ss << "        case " << formatHex(blockAddr) << ": goto label_0x"
           << formatImmediate(blockAddr).substr(2) << ";\n";
    }
    ss << "        default: goto label_0x" << formatImmediate(func.startAddr).substr(2) << ";\n";
    ss << "    }\n";

    for (u32 blockAddr : emittedBlocks) {
        const BasicBlock* block = cfg.getBlock(blockAddr);
        if (!block) {
            continue;
        }

        ss << "label_0x" << std::hex << blockAddr << ":\n";
        ss << "    ctx->pc = " << formatHex(blockAddr) << ";\n";
        for (const auto& instr : block->irInstructions) {
            ss << "    " << emitInstruction(instr, emittedBlocks, localResumeTargets) << "\n";
        }
    }

    ss << "}\n";
    return ss.str();
}

std::string Emitter::emitInstruction(const IRInstruction& instr,
                                     const std::set<u32>& emittedBlocks,
                                     const std::set<u32>& localResumeTargets) {
    auto emitResolvedTarget = [&](u32 target) {
        if (target != 0 && emittedBlocks.count(target)) {
            return "goto label_0x" + formatImmediate(target).substr(2) + ";";
        }
        return "call_by_addr(ctx, " + formatHex(target) + "); return;";
    };

    auto emitLocalResumeOrReturn = [&](const std::string& targetExpr) {
        if (localResumeTargets.empty()) {
            return std::string("return;");
        }

        std::stringstream ss;
        ss << "switch (" << targetExpr << ") { ";
        for (u32 blockAddr : localResumeTargets) {
            ss << "case " << formatHex(blockAddr) << ": goto label_0x"
               << formatImmediate(blockAddr).substr(2) << "; ";
        }
        ss << "default: return; }";
        return ss.str();
    };

    auto gpr = [&](int index) {
        const auto& op = instr.operands[index];
        if (op.type == IROperandType::Register && op.regClass == IRRegisterClass::GPR) {
            return "ctx->gpr[" + std::to_string(op.value) + "]";
        }
        if (op.type == IROperandType::SpecialRegister) {
            return operandToC(op);
        }
        return operandToC(op);
    };

    auto fpr = [&](int index) {
        const auto& op = instr.operands[index];
        if (op.type == IROperandType::Register && op.regClass == IRRegisterClass::FPR) {
            return "ctx->fpr[" + std::to_string(op.value) + "]";
        }
        return operandToC(op);
    };

    auto destGpr = [&](int index) {
        return "ctx->gpr[" + std::to_string(instr.operands[index].value) + "]";
    };

    auto destFpr = [&](int index) {
        return "ctx->fpr[" + std::to_string(instr.operands[index].value) + "]";
    };

    auto branchTarget = [&](const IROperand& op) {
        if (op.type == IROperandType::SpecialRegister) {
            return operandToC(op);
        }
        return formatHex(op.value);
    };

    auto isLinkRegisterTarget = [&](const IROperand& op) {
        return op.type == IROperandType::SpecialRegister &&
               static_cast<IRSpecialRegister>(op.value) == IRSpecialRegister::LinkRegister;
    };

    switch (instr.op) {
        case IROp::SetImm: return destGpr(0) + " = " + operandToC(instr.operands[1]) + ";";
        case IROp::Add:    return destGpr(0) + " = " + gpr(1) + " + " + gpr(2) + ";";
        case IROp::Sub:    return destGpr(0) + " = " + gpr(1) + " - " + gpr(2) + ";";
        case IROp::Mul:    return destGpr(0) + " = " + gpr(1) + " * " + gpr(2) + ";";
        case IROp::DivS:   return destGpr(0) + " = PPC_DIVW(ctx, " + gpr(1) + ", " + gpr(2) + ");";
        case IROp::DivU:   return destGpr(0) + " = PPC_DIVWU(ctx, " + gpr(1) + ", " + gpr(2) + ");";
        case IROp::And:    return destGpr(0) + " = " + gpr(1) + " & " + gpr(2) + ";";
        case IROp::Or:     return destGpr(0) + " = " + gpr(1) + " | " + gpr(2) + ";";
        case IROp::Xor:    return destGpr(0) + " = " + gpr(1) + " ^ " + gpr(2) + ";";
        case IROp::Nor:    return destGpr(0) + " = ~(" + gpr(1) + " | " + gpr(2) + ");";
        case IROp::Andc:   return destGpr(0) + " = " + gpr(1) + " & ~" + gpr(2) + ";";
        case IROp::Orc:    return destGpr(0) + " = " + gpr(1) + " | ~" + gpr(2) + ";";
        case IROp::Nand:   return destGpr(0) + " = ~(" + gpr(1) + " & " + gpr(2) + ");";
        case IROp::Addic:  return destGpr(0) + " = ADDIC(ctx, " + gpr(1) + ", " + operandToC(instr.operands[2]) + ");";
        case IROp::Shl:    return destGpr(0) + " = " + gpr(1) + " << (" + gpr(2) + " & 31);";
        case IROp::Shr:    return destGpr(0) + " = " + gpr(1) + " >> (" + gpr(2) + " & 31);";
        case IROp::Sar:    return destGpr(0) + " = (uint32_t)((int32_t)" + gpr(1) + " >> (" + gpr(2) + " & 31));";
        case IROp::Rol:    return destGpr(0) + " = rotl32(" + gpr(1) + ", " + gpr(2) + ");";
        case IROp::Mask:   return destGpr(0) + " = MASK32(" + gpr(1) + ", " + operandToC(instr.operands[2]) + ", " + operandToC(instr.operands[3]) + ");";
        case IROp::Rlwimi: return destGpr(0) + " = RLWIMI(" + gpr(0) + ", " + gpr(1) + ", " +
                                 operandToC(instr.operands[2]) + ", " + operandToC(instr.operands[3]) + ", " +
                                 operandToC(instr.operands[4]) + ");";
        case IROp::Extsb:  return destGpr(0) + " = EXTSB(" + gpr(1) + ");";
        case IROp::Extsh:  return destGpr(0) + " = EXTSH(" + gpr(1) + ");";
        case IROp::Cntlzw: return destGpr(0) + " = cntlzw(" + gpr(1) + ");";

        case IROp::Load8:      return destGpr(0) + " = MEM_READ8(" + gpr(1) + " + " + gpr(2) + ");";
        case IROp::Load16:     return destGpr(0) + " = MEM_READ16(" + gpr(1) + " + " + gpr(2) + ");";
        case IROp::Load16a:    return destGpr(0) + " = MEM_READ16A(" + gpr(1) + " + " + gpr(2) + ");";
        case IROp::Load32:     return destGpr(0) + " = MEM_READ32(" + gpr(1) + " + " + gpr(2) + ");";
        case IROp::Store8:     return "MEM_WRITE8(" + gpr(1) + " + " + gpr(2) + ", " + gpr(0) + ");";
        case IROp::Store16:    return "MEM_WRITE16(" + gpr(1) + " + " + gpr(2) + ", " + gpr(0) + ");";
        case IROp::Store32:    return "MEM_WRITE32(" + gpr(1) + " + " + gpr(2) + ", " + gpr(0) + ");";
        case IROp::LoadFloat:  return destFpr(0) + " = MEM_READ_FLOAT(" + gpr(1) + " + " + gpr(2) + ");";
        case IROp::LoadDouble: return destFpr(0) + " = MEM_READ_DOUBLE(" + gpr(1) + " + " + gpr(2) + ");";
        case IROp::StoreFloat: return "MEM_WRITE_FLOAT(" + gpr(1) + " + " + gpr(2) + ", " + fpr(0) + ");";
        case IROp::StoreDouble:return "MEM_WRITE_DOUBLE(" + gpr(1) + " + " + gpr(2) + ", " + fpr(0) + ");";
        case IROp::Lmw:        return "LMW(ctx, " + std::to_string(instr.operands[0].value) + ", " + gpr(1) + ", " + operandToC(instr.operands[2]) + ");";
        case IROp::Stmw:       return "STMW(ctx, " + std::to_string(instr.operands[0].value) + ", " + gpr(1) + ", " + operandToC(instr.operands[2]) + ");";

        case IROp::Cmp:   return "set_cr_field(ctx, " + operandToC(instr.operands[0]) + ", " + gpr(1) + ", " + gpr(2) + ", 1);";
        case IROp::Cmpl:  return "set_cr_field(ctx, " + operandToC(instr.operands[0]) + ", " + gpr(1) + ", " + gpr(2) + ", 0);";
        case IROp::CrAnd: return "cr_and(ctx, " + operandToC(instr.operands[0]) + ", " + operandToC(instr.operands[1]) + ", " + operandToC(instr.operands[2]) + ");";
        case IROp::CrOr:  return "cr_or(ctx, " + operandToC(instr.operands[0]) + ", " + operandToC(instr.operands[1]) + ", " + operandToC(instr.operands[2]) + ");";
        case IROp::CrXor: return "cr_xor(ctx, " + operandToC(instr.operands[0]) + ", " + operandToC(instr.operands[1]) + ", " + operandToC(instr.operands[2]) + ");";
        case IROp::CrNor: return "cr_nor(ctx, " + operandToC(instr.operands[0]) + ", " + operandToC(instr.operands[1]) + ", " + operandToC(instr.operands[2]) + ");";

        case IROp::Mfcr:  return destGpr(0) + " = ctx->cr;";
        case IROp::Mtcrf: return "mtcrf(ctx, " + operandToC(instr.operands[0]) + ", " + gpr(1) + ");";
        case IROp::Mfspr: return destGpr(0) + " = get_spr(ctx, " + operandToC(instr.operands[1]) + ");";
        case IROp::Mtspr: return "set_spr(ctx, " + operandToC(instr.operands[0]) + ", " + gpr(1) + ");";
        case IROp::Mfmsr: return destGpr(0) + " = ctx->msr;";
        case IROp::Mtmsr: return "ctx->msr = " + gpr(0) + ";";

        case IROp::FAdd:  return destFpr(0) + " = " + fpr(1) + " + " + fpr(2) + ";";
        case IROp::FSub:  return destFpr(0) + " = " + fpr(1) + " - " + fpr(2) + ";";
        case IROp::FMul:  return destFpr(0) + " = " + fpr(1) + " * " + fpr(2) + ";";
        case IROp::FDiv:  return destFpr(0) + " = " + fpr(1) + " / " + fpr(2) + ";";
        case IROp::FMadd: return destFpr(0) + " = (" + fpr(1) + " * " + fpr(2) + ") + " + fpr(3) + ";";
        case IROp::FMsub: return destFpr(0) + " = (" + fpr(1) + " * " + fpr(2) + ") - " + fpr(3) + ";";
        case IROp::FSel:  return destFpr(0) + " = FSEL(" + fpr(1) + ", " + fpr(2) + ", " + fpr(3) + ");";
        case IROp::Fctiw: return destFpr(0) + " = FCTIW(" + fpr(1) + ");";
        case IROp::Frsp:  return destFpr(0) + " = FRSP(" + fpr(1) + ");";
        case IROp::SetReg:
            if (instr.operands[0].regClass == IRRegisterClass::FPR) {
                return destFpr(0) + " = " + fpr(1) + ";";
            }
            return destGpr(0) + " = " + gpr(1) + ";";

        case IROp::ReservationLoad:
            return destGpr(0) + " = MEM_READ32(" + gpr(1) + " + " + gpr(2) + "); ctx->xer = 1;";
        case IROp::ReservationStore:
            return "MEM_WRITE32(" + gpr(1) + " + " + gpr(2) + ", " + gpr(0) + ");";

        case IROp::Sync:    return "/* sync/eieio */";
        case IROp::Isync:   return "/* isync/icbi/dcbz */";
        case IROp::Trap:    return "abort();";
        case IROp::Syscall: return "/* sc */";
        case IROp::Rfi:     return "ctx->msr = get_spr(ctx, 0x1b); call_by_addr(ctx, get_spr(ctx, 0x1a)); return;";

        case IROp::Branch: {
            if (isLinkRegisterTarget(instr.operands[0])) {
                return emitLocalResumeOrReturn("ctx->lr");
            }
            const u32 target = instr.operands[0].value;
            if (emittedBlocks.count(target)) {
                return "goto label_0x" + formatImmediate(target).substr(2) + ";";
            }
            return "call_by_addr(ctx, " + formatHex(target) + "); return;";
        }

        case IROp::BranchCond: {
            const auto& target = instr.operands[2];
            std::stringstream ss;
            ss << "if (CHECK_COND(ctx, " << instr.operands[0].value << ", " << instr.operands[1].value << ")) { ";
            if (target.type == IROperandType::Address && emittedBlocks.count(target.value)) {
                ss << "goto label_0x" << std::hex << target.value << ";";
            } else if (isLinkRegisterTarget(target)) {
                ss << emitLocalResumeOrReturn("ctx->lr");
            } else if (target.type == IROperandType::Address) {
                ss << "call_by_addr(ctx, " << formatHex(target.value) << "); return;";
            } else {
                ss << "call_by_addr(ctx, " << branchTarget(target) << "); return;";
            }
            ss << " }";
            return ss.str();
        }

        case IROp::BranchIndirect:
            if (isLinkRegisterTarget(instr.operands[0])) {
                return emitLocalResumeOrReturn("ctx->lr");
            }
            return "call_by_addr(ctx, " + branchTarget(instr.operands[0]) + "); return;";

        case IROp::BranchTable: {
            std::stringstream ss;
            ss << "switch (" << regOrImm(instr.operands[0]) << ") { ";
            for (size_t i = 2; i < instr.operands.size(); ++i) {
                const u32 target = instr.operands[i].value;
                ss << "case " << std::dec << (i - 2) << ": " << emitResolvedTarget(target) << " ";
            }

            const u32 defaultTarget = instr.operands.size() > 1 ? instr.operands[1].value : 0;
            ss << "default: " << emitResolvedTarget(defaultTarget) << " }";
            return ss.str();
        }

        case IROp::Call:
            if (!instr.operands.empty() && emittedBlocks.count(instr.operands[0].value)) {
                return "ctx->lr = " + formatHex(instr.address + 4) + "; goto label_0x" +
                       formatImmediate(instr.operands[0].value).substr(2) + ";";
            }
            return "call_by_addr(ctx, " + formatHex(instr.operands[0].value) + ");";

        case IROp::CallIndirect:
            return "call_by_addr(ctx, " + branchTarget(instr.operands[0]) + ");";

        case IROp::Return:
            return emitLocalResumeOrReturn("ctx->lr");

        default:
            return "// Unsupported IROp (" + std::to_string(static_cast<int>(instr.op)) + ")";
    }
}

std::string Emitter::regOrImm(const IROperand& op) {
    if (op.type == IROperandType::Register && op.regClass == IRRegisterClass::GPR) {
        return "ctx->gpr[" + std::to_string(op.value) + "]";
    }
    return operandToC(op);
}

std::string Emitter::fRegOrImm(const IROperand& op) {
    if (op.type == IROperandType::Register && op.regClass == IRRegisterClass::FPR) {
        return "ctx->fpr[" + std::to_string(op.value) + "]";
    }
    return operandToC(op);
}

std::string Emitter::operandToC(const IROperand& op) {
    switch (op.type) {
        case IROperandType::Register:
            return std::to_string(op.value);
        case IROperandType::Immediate:
            return formatImmediate(op.value);
        case IROperandType::Address:
            return formatHex(op.value);
        case IROperandType::SpecialRegister:
            switch (static_cast<IRSpecialRegister>(op.value)) {
                case IRSpecialRegister::LinkRegister:
                    return "ctx->lr";
                case IRSpecialRegister::CountRegister:
                    return "ctx->ctr";
                default:
                    return "0";
            }
        default:
            return "0";
    }
}

} // namespace gcrecomp
