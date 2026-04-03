#include <gcrecomp/analysis/cfg.h>
#include <gcrecomp/analysis/analyzer.h>
#include <gcrecomp/analysis/disasm.h>
#include <gcrecomp/analysis/lifter.h>
#include <gcrecomp/analysis/optimizer.h>
#include <gcrecomp/codegen/emitter.h>
#include <gcrecomp/loader/binary.h>
#include <recomp_runtime.h>

#include <fstream>
#include <string>
#include <vector>

using namespace gcrecomp;

u8* ram = nullptr;

namespace {

struct TestBinary : Binary {
    bool load(const std::string&) override { return false; }
    u32 getEntryPoint() const override { return m_entryPoint; }
    const std::vector<Section>& getSections() const override { return m_sections; }

    void setTextWords(u32 start, const std::vector<u32>& words) {
        m_regions.clear();
        m_sections.clear();
        appendWords(start, words, true);
        m_entryPoint = start;
    }

    void setEntryPoint(u32 entryPoint) {
        m_entryPoint = entryPoint;
    }

    void addDataWords(u32 start, const std::vector<u32>& words) {
        appendWords(start, words, false);
    }

private:
    void appendWords(u32 start, const std::vector<u32>& words, bool isText) {
        MemoryRegion region;
        region.start = start;
        region.size = static_cast<u32>(words.size() * 4);
        region.data.resize(region.size);
        for (size_t i = 0; i < words.size(); ++i) {
            const u32 word = words[i];
            region.data[i * 4 + 0] = static_cast<u8>(word >> 24);
            region.data[i * 4 + 1] = static_cast<u8>(word >> 16);
            region.data[i * 4 + 2] = static_cast<u8>(word >> 8);
            region.data[i * 4 + 3] = static_cast<u8>(word);
        }
        m_regions.push_back(region);
        m_sections.push_back({ start, region.size, 0, isText });
    }
};

u32 encodeDForm(u32 op, u32 rt, u32 ra, s16 imm) {
    return (op << 26) | (rt << 21) | (ra << 16) | static_cast<u16>(imm);
}

u32 encodeBForm(u32 byteOffset, bool absolute, bool link) {
    return (18u << 26) | (byteOffset & 0x03FFFFFCu) | (absolute ? 2u : 0u) | (link ? 1u : 0u);
}

u32 encodeCmpi(u32 crField, u32 ra, s16 imm) {
    return (11u << 26) | (crField << 23) | (ra << 16) | static_cast<u16>(imm);
}

u32 encodeCmpli(u32 crField, u32 ra, u16 imm) {
    return (10u << 26) | (crField << 23) | (ra << 16) | imm;
}

u32 encodeBcRelative(u32 address, u32 bo, u32 bi, u32 target, bool link = false) {
    const s32 disp = static_cast<s32>(target) - static_cast<s32>(address);
    return (16u << 26) | (bo << 21) | (bi << 16) |
           (static_cast<u32>(disp) & 0x0000FFFCu) |
           (link ? 1u : 0u);
}

u32 encodeAddis(u32 rt, u32 ra, s16 imm) {
    return (15u << 26) | (rt << 21) | (ra << 16) | static_cast<u16>(imm);
}

u32 encodeAddi(u32 rt, u32 ra, s16 imm) {
    return (14u << 26) | (rt << 21) | (ra << 16) | static_cast<u16>(imm);
}

u32 encodeOri(u32 rs, u32 ra, u16 imm) {
    return (24u << 26) | (rs << 21) | (ra << 16) | imm;
}

u32 encodeRlwinm(u32 rs, u32 ra, u32 sh, u32 mb, u32 me) {
    return (21u << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1);
}

u32 encodeX(u32 rsOrRt, u32 ra, u32 rb, u32 xop) {
    return (31u << 26) | (rsOrRt << 21) | (ra << 16) | (rb << 11) | (xop << 1);
}

u32 encodeFcmp(u32 crField, u32 fra, u32 frb, bool ordered) {
    return (63u << 26) | (crField << 23) | (fra << 16) | (frb << 11) | ((ordered ? 32u : 0u) << 1);
}

u32 encodeAForm(u32 op, u32 frt, u32 fra, u32 frb, u32 frc, u32 xo5) {
    return (op << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo5 << 1);
}

u32 encodeFUnary(u32 frt, u32 frb, u32 xop) {
    return (63u << 26) | (frt << 21) | (frb << 11) | (xop << 1);
}

u32 encodeXfx(u32 xop, u32 rt, u32 spr) {
    const u32 sprLo = spr & 0x1F;
    const u32 sprHi = (spr >> 5) & 0x1F;
    return (31u << 26) | (rt << 21) | (sprLo << 16) | (sprHi << 11) | (xop << 1);
}

u32 encodeMtspr(u32 rs, u32 spr) {
    const u32 sprLo = spr & 0x1F;
    const u32 sprHi = (spr >> 5) & 0x1F;
    return (31u << 26) | (rs << 21) | (sprLo << 16) | (sprHi << 11) | (467u << 1);
}

u32 encodeRfi() {
    return 0x4C000064u;
}

Instruction disassembleSingle(u32 address, u32 raw) {
    TestBinary binary;
    binary.setTextWords(address, { raw });
    Disassembler disassembler;
    Instruction instr;
    disassembler.disassemble(binary, address, instr);
    return instr;
}

IRInstruction liftSingle(const Instruction& instr) {
    Lifter lifter;
    BasicBlock block;
    block.startAddr = instr.address;
    block.instructions.push_back(instr);
    IRBlock ir = lifter.liftBlock(block);
    return ir.instructions.empty() ? IRInstruction(IROp::None) : ir.instructions.front();
}

bool testBranchDecoding() {
    const Instruction directCall = disassembleSingle(0x80000000, encodeBForm(0x100, false, true));
    if (directCall.type != InstructionType::Call || !directCall.isLink || directCall.branchTarget != 0x80000100) {
        return false;
    }

    const Instruction blr = disassembleSingle(0x80001000, 0x4E800020u);
    if (blr.type != InstructionType::Return || blr.branchRegisterTarget != BranchRegisterTarget::LinkRegister) {
        return false;
    }

    const Instruction bctrl = disassembleSingle(0x80002000, 0x4E800421u);
    if (bctrl.type != InstructionType::Call || bctrl.branchRegisterTarget != BranchRegisterTarget::CountRegister) {
        return false;
    }

    const Instruction rfi = disassembleSingle(0x80003000, encodeRfi());
    if (rfi.type != InstructionType::Return || !rfi.isInterruptReturn || rfi.mnemonic != "rfi") {
        return false;
    }

    return true;
}

bool testLifterZeroBaseAndCompareField() {
    Instruction lwz {};
    lwz.address = 0x80000000;
    lwz.raw = encodeDForm(32, 3, 0, 0x20);

    IRInstruction load = liftSingle(lwz);
    if (load.op != IROp::Load32 || load.operands.size() != 3) {
        return false;
    }
    if (load.operands[1].type != IROperandType::Immediate || load.operands[1].value != 0) {
        return false;
    }

    Instruction cmpi {};
    cmpi.address = 0x80000004;
    cmpi.raw = encodeCmpi(2, 3, -1);
    IRInstruction cmp = liftSingle(cmpi);
    if (cmp.op != IROp::Cmp || cmp.operands.size() != 3) {
        return false;
    }
    if (cmp.operands[0].type != IROperandType::Immediate || cmp.operands[0].value != 2) {
        return false;
    }

    return true;
}

bool testLifterIndirectCall() {
    Instruction instr = disassembleSingle(0x80002000, 0x4E800421u);
    IRInstruction ir = liftSingle(instr);
    if (ir.op != IROp::CallIndirect || ir.operands.size() != 1) {
        return false;
    }
    return ir.operands[0].type == IROperandType::SpecialRegister &&
           ir.operands[0].value == static_cast<u32>(IRSpecialRegister::CountRegister);
}

bool testLifterTimeBaseRead() {
    Instruction instr {};
    instr.address = 0x80000008;
    instr.raw = encodeXfx(371, 4, 268);

    IRInstruction ir = liftSingle(instr);
    return ir.op == IROp::Mfspr &&
           ir.operands.size() == 2 &&
           ir.operands[1].type == IROperandType::Immediate &&
           ir.operands[1].value == 268;
}

bool testLifterMsrOps() {
    Instruction mfmsr {};
    mfmsr.address = 0x8000000C;
    mfmsr.raw = 0x7C0000A6u;
    IRInstruction mfmsrIr = liftSingle(mfmsr);
    if (mfmsrIr.op != IROp::Mfmsr || mfmsrIr.operands.size() != 1 || mfmsrIr.operands[0].value != 0) {
        return false;
    }

    Instruction mtmsr {};
    mtmsr.address = 0x80000010;
    mtmsr.raw = 0x7C000124u;
    IRInstruction mtmsrIr = liftSingle(mtmsr);
    return mtmsrIr.op == IROp::Mtmsr &&
           mtmsrIr.operands.size() == 1 &&
           mtmsrIr.operands[0].type == IROperandType::Register &&
           mtmsrIr.operands[0].value == 0;
}

bool testLifterInterruptReturn() {
    Instruction rfi = disassembleSingle(0x80000014, encodeRfi());
    IRInstruction rfiIr = liftSingle(rfi);
    return rfiIr.op == IROp::Rfi;
}

bool testLifterRecordFormUpdatesCr0() {
    Instruction instr {};
    instr.address = 0x80000014;
    instr.raw = encodeRlwinm(3, 3, 0, 31, 31) | 1u;

    Lifter lifter;
    BasicBlock block;
    block.startAddr = instr.address;
    block.instructions.push_back(instr);
    IRBlock ir = lifter.liftBlock(block);

    if (ir.instructions.size() != 3) {
        return false;
    }
    if (ir.instructions[0].op != IROp::Rol || ir.instructions[1].op != IROp::Mask || ir.instructions[2].op != IROp::Cmp) {
        return false;
    }
    return ir.instructions[2].operands.size() == 3 &&
           ir.instructions[2].operands[0].type == IROperandType::Immediate &&
           ir.instructions[2].operands[0].value == 0 &&
           ir.instructions[2].operands[1].type == IROperandType::Register &&
           ir.instructions[2].operands[1].value == 3 &&
           ir.instructions[2].operands[2].type == IROperandType::Immediate &&
           ir.instructions[2].operands[2].value == 0;
}

bool testLifterDivwu() {
    Instruction instr {};
    instr.address = 0x80000018;
    instr.raw = encodeX(3, 4, 5, 459);

    IRInstruction ir = liftSingle(instr);
    return ir.op == IROp::DivU &&
           ir.operands.size() == 3 &&
           ir.operands[0].type == IROperandType::Register &&
           ir.operands[0].value == 3 &&
           ir.operands[1].type == IROperandType::Register &&
           ir.operands[1].value == 4 &&
           ir.operands[2].type == IROperandType::Register &&
           ir.operands[2].value == 5;
}

bool testLifterMulhwu() {
    Instruction instr {};
    instr.address = 0x8000001A;
    instr.raw = encodeX(3, 4, 5, 11);

    IRInstruction ir = liftSingle(instr);
    return ir.op == IROp::MulHighU &&
           ir.operands.size() == 3 &&
           ir.operands[0].type == IROperandType::Register &&
           ir.operands[0].value == 3 &&
           ir.operands[1].type == IROperandType::Register &&
           ir.operands[1].value == 4 &&
           ir.operands[2].type == IROperandType::Register &&
           ir.operands[2].value == 5;
}

bool testLifterMulhw() {
    Instruction instr {};
    instr.address = 0x8000001C;
    instr.raw = encodeX(3, 4, 5, 75);

    IRInstruction ir = liftSingle(instr);
    return ir.op == IROp::MulHighS &&
           ir.operands.size() == 3 &&
           ir.operands[0].type == IROperandType::Register &&
           ir.operands[0].value == 3 &&
           ir.operands[1].type == IROperandType::Register &&
           ir.operands[1].value == 4 &&
           ir.operands[2].type == IROperandType::Register &&
           ir.operands[2].value == 5;
}

bool testLifterDivw() {
    Instruction instr {};
    instr.address = 0x8000001E;
    instr.raw = encodeX(3, 4, 5, 491);

    IRInstruction ir = liftSingle(instr);
    return ir.op == IROp::DivS &&
           ir.operands.size() == 3 &&
           ir.operands[0].type == IROperandType::Register &&
           ir.operands[0].value == 3 &&
           ir.operands[1].type == IROperandType::Register &&
           ir.operands[1].value == 4 &&
           ir.operands[2].type == IROperandType::Register &&
           ir.operands[2].value == 5;
}

bool testLifterFloatingCompare() {
    Instruction unordered {};
    unordered.address = 0x80000020;
    unordered.raw = encodeFcmp(2, 6, 7, false);

    IRInstruction unorderedIr = liftSingle(unordered);
    if (unorderedIr.op != IROp::FCmpu ||
        unorderedIr.operands.size() != 3 ||
        unorderedIr.operands[0].type != IROperandType::Immediate ||
        unorderedIr.operands[0].value != 2 ||
        unorderedIr.operands[1].regClass != IRRegisterClass::FPR ||
        unorderedIr.operands[1].value != 6 ||
        unorderedIr.operands[2].regClass != IRRegisterClass::FPR ||
        unorderedIr.operands[2].value != 7) {
        return false;
    }

    Instruction ordered {};
    ordered.address = 0x80000024;
    ordered.raw = encodeFcmp(5, 8, 9, true);

    IRInstruction orderedIr = liftSingle(ordered);
    return orderedIr.op == IROp::FCmpo &&
           orderedIr.operands.size() == 3 &&
           orderedIr.operands[0].type == IROperandType::Immediate &&
           orderedIr.operands[0].value == 5 &&
           orderedIr.operands[1].regClass == IRRegisterClass::FPR &&
           orderedIr.operands[1].value == 8 &&
           orderedIr.operands[2].regClass == IRRegisterClass::FPR &&
           orderedIr.operands[2].value == 9;
}

bool testLifterFloatingAFormOperands() {
    Instruction fmuls {};
    fmuls.address = 0x80000028;
    fmuls.raw = encodeAForm(59, 4, 4, 0, 1, 25);

    IRInstruction fmulsIr = liftSingle(fmuls);
    if (fmulsIr.op != IROp::FMul ||
        fmulsIr.operands.size() != 3 ||
        fmulsIr.operands[0].regClass != IRRegisterClass::FPR ||
        fmulsIr.operands[0].value != 4 ||
        fmulsIr.operands[1].regClass != IRRegisterClass::FPR ||
        fmulsIr.operands[1].value != 4 ||
        fmulsIr.operands[2].regClass != IRRegisterClass::FPR ||
        fmulsIr.operands[2].value != 1) {
        return false;
    }

    Instruction fmadd {};
    fmadd.address = 0x8000002C;
    fmadd.raw = encodeAForm(63, 7, 8, 9, 10, 29);

    IRInstruction fmaddIr = liftSingle(fmadd);
    return fmaddIr.op == IROp::FMadd &&
           fmaddIr.operands.size() == 4 &&
           fmaddIr.operands[0].regClass == IRRegisterClass::FPR &&
           fmaddIr.operands[0].value == 7 &&
           fmaddIr.operands[1].regClass == IRRegisterClass::FPR &&
           fmaddIr.operands[1].value == 8 &&
           fmaddIr.operands[2].regClass == IRRegisterClass::FPR &&
           fmaddIr.operands[2].value == 10 &&
           fmaddIr.operands[3].regClass == IRRegisterClass::FPR &&
           fmaddIr.operands[3].value == 9;
}

bool testLifterFloatingUnaryOps() {
    Instruction fneg {};
    fneg.address = 0x80000030;
    fneg.raw = encodeFUnary(28, 2, 40);
    IRInstruction fnegIr = liftSingle(fneg);
    if (fnegIr.op != IROp::FNeg ||
        fnegIr.operands.size() != 2 ||
        fnegIr.operands[0].regClass != IRRegisterClass::FPR ||
        fnegIr.operands[0].value != 28 ||
        fnegIr.operands[1].regClass != IRRegisterClass::FPR ||
        fnegIr.operands[1].value != 2) {
        return false;
    }

    Instruction fnabs {};
    fnabs.address = 0x80000034;
    fnabs.raw = encodeFUnary(5, 6, 136);
    IRInstruction fnabsIr = liftSingle(fnabs);
    if (fnabsIr.op != IROp::Fnabs ||
        fnabsIr.operands.size() != 2 ||
        fnabsIr.operands[0].regClass != IRRegisterClass::FPR ||
        fnabsIr.operands[0].value != 5 ||
        fnabsIr.operands[1].regClass != IRRegisterClass::FPR ||
        fnabsIr.operands[1].value != 6) {
        return false;
    }

    Instruction fabsInstr {};
    fabsInstr.address = 0x80000038;
    fabsInstr.raw = encodeFUnary(7, 8, 264);
    IRInstruction fabsIr = liftSingle(fabsInstr);
    return fabsIr.op == IROp::FAbs &&
           fabsIr.operands.size() == 2 &&
           fabsIr.operands[0].regClass == IRRegisterClass::FPR &&
           fabsIr.operands[0].value == 7 &&
           fabsIr.operands[1].regClass == IRRegisterClass::FPR &&
           fabsIr.operands[1].value == 8;
}

bool testEmitterUsesNewHelpers() {
    ControlFlowGraph cfg;
    Function function;
    function.startAddr = 0x80000000;
    function.name = "test_fn";
    function.blocks.insert(0x80000000);
    cfg.addFunction(function);

    BasicBlock block;
    block.startAddr = 0x80000000;
    block.irInstructions.push_back({ IROp::DivU, { IROperand::Reg(3), IROperand::Reg(4), IROperand::Reg(5) } });
    block.irInstructions.push_back({ IROp::DivS, { IROperand::Reg(6), IROperand::Reg(7), IROperand::Reg(8) } });
    block.irInstructions.push_back({ IROp::MulHighU, { IROperand::Reg(9), IROperand::Reg(10), IROperand::Reg(11) } });
    block.irInstructions.push_back({ IROp::MulHighS, { IROperand::Reg(12), IROperand::Reg(13), IROperand::Reg(14) } });
    block.irInstructions.push_back({ IROp::Mask, { IROperand::Reg(3), IROperand::Reg(3), IROperand::Imm(5), IROperand::Imm(10) } });
    block.irInstructions.push_back({ IROp::FCmpo, { IROperand::Imm(2), IROperand::FReg(3), IROperand::FReg(4) } });
    block.irInstructions.push_back({ IROp::FCmpu, { IROperand::Imm(5), IROperand::FReg(6), IROperand::FReg(7) } });
    block.irInstructions.push_back({ IROp::Fctiw, { IROperand::FReg(1), IROperand::FReg(2) } });
    block.irInstructions.push_back({ IROp::FNeg, { IROperand::FReg(8), IROperand::FReg(9) } });
    block.irInstructions.push_back({ IROp::FAbs, { IROperand::FReg(10), IROperand::FReg(11) } });
    block.irInstructions.push_back({ IROp::Fnabs, { IROperand::FReg(12), IROperand::FReg(13) } });
    block.irInstructions.push_back({ IROp::CallIndirect, { IROperand::Special(IRSpecialRegister::CountRegister) } });
    block.irInstructions.push_back({ IROp::Rfi, {} });
    cfg.addBlock(block);

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(function, cfg);
    return emitted.find("PPC_DIVWU(ctx, ctx->gpr[4], ctx->gpr[5]);") != std::string::npos &&
           emitted.find("PPC_DIVW(ctx, ctx->gpr[7], ctx->gpr[8]);") != std::string::npos &&
           emitted.find("PPC_MULHWU(ctx, ctx->gpr[10], ctx->gpr[11]);") != std::string::npos &&
           emitted.find("PPC_MULHW(ctx, ctx->gpr[13], ctx->gpr[14]);") != std::string::npos &&
           emitted.find("MASK32(") != std::string::npos &&
           emitted.find("set_fp_cr_field(ctx, 0x2, ctx->fpr[3], ctx->fpr[4], 1);") != std::string::npos &&
           emitted.find("set_fp_cr_field(ctx, 0x5, ctx->fpr[6], ctx->fpr[7], 0);") != std::string::npos &&
           emitted.find("FCTIW(") != std::string::npos &&
           emitted.find("ctx->fpr[8] = -ctx->fpr[9];") != std::string::npos &&
           emitted.find("ctx->fpr[10] = fabs(ctx->fpr[11]);") != std::string::npos &&
           emitted.find("ctx->fpr[12] = -fabs(ctx->fpr[13]);") != std::string::npos &&
           emitted.find("call_by_addr(ctx, ctx->ctr);") != std::string::npos &&
           emitted.find("ctx->msr = get_spr(ctx, 0x1b); call_by_addr(ctx, get_spr(ctx, 0x1a)); return;") != std::string::npos;
}

bool testEmitterResumesAtSavedPc() {
    ControlFlowGraph cfg;
    Function function;
    function.startAddr = 0x80000000u;
    function.name = "resume_fn";
    function.blocks.insert(0x80000000u);
    function.blocks.insert(0x80000008u);
    cfg.addFunction(function);

    BasicBlock entryBlock;
    entryBlock.startAddr = 0x80000000u;
    entryBlock.endAddr = 0x80000004u;
    entryBlock.isAnalyzed = true;
    cfg.addBlock(entryBlock);

    BasicBlock resumeBlock;
    resumeBlock.startAddr = 0x80000008u;
    resumeBlock.endAddr = 0x80000008u;
    resumeBlock.isAnalyzed = true;
    cfg.addBlock(resumeBlock);

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(function, cfg);
    return emitted.find("switch (ctx->pc)") != std::string::npos &&
           emitted.find("case 0x80000008: goto label_0x80000008;") != std::string::npos &&
           emitted.find("default: goto label_0x80000000;") != std::string::npos;
}

bool testEmitterTreatsLrBranchesAsReturns() {
    ControlFlowGraph cfg;
    Function function;
    function.startAddr = 0x80000000;
    function.name = "lr_branch_fn";
    function.blocks.insert(0x80000000);
    cfg.addFunction(function);

    BasicBlock block;
    block.startAddr = 0x80000000;
    block.irInstructions.push_back({ IROp::BranchCond, {
        IROperand::Imm(12),
        IROperand::Imm(2),
        IROperand::Special(IRSpecialRegister::LinkRegister)
    } });
    block.irInstructions.push_back({ IROp::BranchIndirect, {
        IROperand::Special(IRSpecialRegister::LinkRegister)
    } });
    cfg.addBlock(block);

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(function, cfg);
    return emitted.find("if (CHECK_COND(ctx, 12, 2)) { return; }") != std::string::npos &&
           emitted.find("call_by_addr(ctx, ctx->lr);") == std::string::npos;
}

bool testEmitterDispatchesLocalCtrBranches() {
    ControlFlowGraph cfg;
    Function function;
    function.startAddr = 0x80000000u;
    function.name = "ctr_branch_fn";
    function.blocks.insert(0x80000000u);
    function.blocks.insert(0x80000010u);
    function.blocks.insert(0x80000020u);
    cfg.addFunction(function);

    BasicBlock entryBlock;
    entryBlock.startAddr = 0x80000000u;
    entryBlock.irInstructions.push_back({ IROp::BranchIndirect, {
        IROperand::Special(IRSpecialRegister::CountRegister)
    } });
    cfg.addBlock(entryBlock);

    BasicBlock targetA;
    targetA.startAddr = 0x80000010u;
    targetA.irInstructions.push_back({ IROp::Return, {} });
    cfg.addBlock(targetA);

    BasicBlock targetB;
    targetB.startAddr = 0x80000020u;
    targetB.irInstructions.push_back({ IROp::Return, {} });
    cfg.addBlock(targetB);

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(function, cfg);
    return emitted.find("const uint32_t branch_target = ctx->ctr; switch (branch_target)") != std::string::npos &&
           emitted.find("case 0x80000010: goto label_0x80000010;") != std::string::npos &&
           emitted.find("case 0x80000020: goto label_0x80000020;") != std::string::npos &&
           emitted.find("default: call_by_addr(ctx, branch_target); return;") != std::string::npos;
}

bool testEmitterDoesNotInventLocalLrResumes() {
    ControlFlowGraph cfg;
    Function function;
    function.startAddr = 0x80000000;
    function.name = "plain_return_fn";
    function.blocks.insert(0x80000000);
    function.blocks.insert(0x80000010);
    cfg.addFunction(function);

    BasicBlock entryBlock;
    entryBlock.startAddr = 0x80000000;
    entryBlock.irInstructions.push_back({ IROp::Branch, {
        IROperand::Addr(0x80000010)
    } });
    entryBlock.successors.insert(0x80000010);
    cfg.addBlock(entryBlock);

    BasicBlock returnBlock;
    returnBlock.startAddr = 0x80000010;
    returnBlock.irInstructions.push_back({ IROp::Return, {} });
    cfg.addBlock(returnBlock);

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(function, cfg);
    return emitted.find("switch (ctx->lr)") == std::string::npos &&
           emitted.find("label_0x80000010:") != std::string::npos &&
           emitted.find("return;") != std::string::npos;
}

bool testEmitterIgnoresMissingFunctionSeedBlocks() {
    ControlFlowGraph cfg;
    Function function;
    function.startAddr = 0x80000000u;
    function.name = "phantom_seed_fn";
    function.blocks.insert(0x60u);
    function.blocks.insert(0x80000000u);
    cfg.addFunction(function);

    BasicBlock block;
    block.startAddr = 0x80000000u;
    block.irInstructions.push_back({ IROp::Call, { IROperand::Addr(0x60u) } });
    cfg.addBlock(block);

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(function, cfg);
    return emitted.find("case 0x00000060: goto label_0x60;") == std::string::npos &&
           emitted.find("goto label_0x60;") == std::string::npos &&
           emitted.find("call_by_addr(ctx, 0x00000060);") != std::string::npos;
}

bool testEmitterSetsLrForExternalCalls() {
    ControlFlowGraph cfg;
    Function function;
    function.startAddr = 0x80000000u;
    function.name = "external_call_fn";
    function.blocks.insert(0x80000000u);
    cfg.addFunction(function);

    BasicBlock block;
    block.startAddr = 0x80000000u;

    IRInstruction directCall { IROp::Call, { IROperand::Addr(0x80000100u) } };
    directCall.address = 0x80000020u;
    block.irInstructions.push_back(directCall);

    IRInstruction indirectCall { IROp::CallIndirect, { IROperand::Special(IRSpecialRegister::CountRegister) } };
    indirectCall.address = 0x80000024u;
    block.irInstructions.push_back(indirectCall);

    cfg.addBlock(block);

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(function, cfg);
    return emitted.find("ctx->lr = 0x80000024; call_by_addr(ctx, 0x80000100);") != std::string::npos &&
           emitted.find("ctx->lr = 0x80000028; call_by_addr(ctx, ctx->ctr);") != std::string::npos;
}

bool testEmitterPreservesLrIndirectCallTarget() {
    ControlFlowGraph cfg;
    Function function;
    function.startAddr = 0x80000000u;
    function.name = "lr_indirect_call_fn";
    function.blocks.insert(0x80000000u);
    cfg.addFunction(function);

    BasicBlock block;
    block.startAddr = 0x80000000u;

    IRInstruction indirectCall { IROp::CallIndirect, { IROperand::Special(IRSpecialRegister::LinkRegister) } };
    indirectCall.address = 0x80000020u;
    block.irInstructions.push_back(indirectCall);

    cfg.addBlock(block);

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(function, cfg);
    return emitted.find("const uint32_t call_target = ctx->lr; ctx->lr = 0x80000024; call_by_addr(ctx, call_target);") != std::string::npos &&
           emitted.find("ctx->lr = 0x80000024; call_by_addr(ctx, ctx->lr);") == std::string::npos;
}

bool testEmitterLogsUnexpectedLocalResumeTargets() {
    ControlFlowGraph cfg;
    Function function;
    function.startAddr = 0x80000000u;
    function.name = "resume_trace_fn";
    function.blocks.insert(0x80000000u);
    function.blocks.insert(0x80000004u);
    function.blocks.insert(0x80000010u);
    cfg.addFunction(function);

    BasicBlock entryBlock;
    entryBlock.startAddr = 0x80000000u;

    IRInstruction call { IROp::Call, { IROperand::Addr(0x80000010u) } };
    call.address = 0x80000000u;
    entryBlock.irInstructions.push_back(call);
    cfg.addBlock(entryBlock);

    BasicBlock resumeBlock;
    resumeBlock.startAddr = 0x80000004u;
    resumeBlock.irInstructions.push_back({ IROp::Return, {} });
    cfg.addBlock(resumeBlock);

    BasicBlock calleeBlock;
    calleeBlock.startAddr = 0x80000010u;
    calleeBlock.irInstructions.push_back({ IROp::Return, {} });
    cfg.addBlock(calleeBlock);

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(function, cfg);
    return emitted.find("case 0x80000004: goto label_0x80000004;") != std::string::npos &&
           emitted.find("default: return;") != std::string::npos &&
           emitted.find("[RUNTIME] Unhandled local resume target") == std::string::npos;
}

bool testAnalyzerEmitsLocalJumpTablesAsSwitches() {
    TestBinary binary;
    binary.setTextWords(0x80000000u, {
        0x9421FFF0u,                                  // stwu r1, -16(r1)
        encodeCmpli(0, 5, 2),                         // cmplwi r5, 2
        encodeBcRelative(0x80000008u, 12, 1, 0x80000030u),
        encodeAddis(4, 0, static_cast<s16>(0x8000)), // lis r4, 0x8000
        encodeAddi(4, 4, 0x100),                      // addi r4, r4, 0x100
        encodeRlwinm(5, 0, 2, 0, 29),                // rlwinm r0, r5, 2, 0, 29
        encodeX(0, 4, 0, 23),                        // lwzx r0, r4, r0
        encodeMtspr(0, 9),                           // mtctr r0
        0x4E800420u,                                 // bctr
        encodeAddi(31, 0, 0),                        // li r31, 0
        encodeBForm(0x0C, false, false),             // b 0x80000034
        encodeAddi(31, 0, 1),                        // li r31, 1
        0x4E800020u,                                 // blr (default / case 2)
        0x4E800020u,                                 // blr (join)
    });
    binary.addDataWords(0x80000100u, {
        0x80000024u,
        0x8000002Cu,
        0x80000030u,
    });

    Analyzer analyzer(binary);
    analyzer.analyze(binary.getEntryPoint());

    const Function* function = analyzer.getCfg().getFunction(0x80000000u);
    if (function == nullptr) {
        return false;
    }

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(*function, analyzer.getCfg());
    return emitted.find("switch (ctx->gpr[5])") != std::string::npos &&
           emitted.find("goto label_0x80000024;") != std::string::npos &&
           emitted.find("label_0x80000024:") != std::string::npos &&
           emitted.find("label_0x80000034:") != std::string::npos;
}

bool testAnalyzerHandlesInterleavedJumpTableSetup() {
    TestBinary binary;
    binary.setTextWords(0x80000000u, {
        0x9421FFF0u,                                  // stwu r1, -16(r1)
        encodeCmpli(0, 0, 1),                         // cmplwi r0, 1
        encodeBcRelative(0x80000008u, 12, 1, 0x80000030u),
        encodeAddis(4, 0, static_cast<s16>(0x8000)), // lis r4, 0x8000
        encodeRlwinm(0, 0, 2, 0, 29),                // rlwinm r0, r0, 2, 0, 29
        encodeAddi(4, 4, 0x100),                     // addi r4, r4, 0x100
        encodeX(0, 4, 0, 23),                        // lwzx r0, r4, r0
        encodeMtspr(0, 9),                           // mtctr r0
        0x4E800420u,                                 // bctr
        0x4E800020u,                                 // blr (case 0 target at 0x80000024)
        encodeAddi(31, 0, 1),                        // li r31, 1 (case 1 target)
        0x4E800020u,                                 // blr
        encodeAddi(31, 0, 2),                        // li r31, 2 (default target)
        0x4E800020u,                                 // blr
    });
    binary.addDataWords(0x80000100u, {
        0x80000024u,
        0x80000028u,
    });

    Analyzer analyzer(binary);
    analyzer.analyze(binary.getEntryPoint());

    const Function* function = analyzer.getCfg().getFunction(0x80000000u);
    if (function == nullptr) {
        return false;
    }

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(*function, analyzer.getCfg());
    return emitted.find("switch (ctx->gpr[0])") != std::string::npos &&
           emitted.find("case 0: goto label_0x80000024;") != std::string::npos &&
           emitted.find("case 1: goto label_0x80000028;") != std::string::npos &&
           emitted.find("label_0x80000024:") != std::string::npos &&
           emitted.find("label_0x80000028:") != std::string::npos;
}

bool testLifterPreservesClobberedLocalJumpTableIndex() {
    TestBinary binary;
    binary.setTextWords(0x80000000u, {
        0x9421FFF0u,                                  // stwu r1, -16(r1)
        encodeCmpli(0, 0, 2),                         // cmplwi r0, 2
        encodeBcRelative(0x80000008u, 12, 1, 0x80000030u),
        encodeAddis(4, 0, static_cast<s16>(0x8000)), // lis r4, 0x8000
        encodeAddi(4, 4, 0x100),                      // addi r4, r4, 0x100
        encodeRlwinm(0, 0, 2, 0, 29),                // rlwinm r0, r0, 2, 0, 29
        encodeX(0, 4, 0, 23),                        // lwzx r0, r4, r0
        encodeMtspr(0, 9),                           // mtctr r0
        0x4E800420u,                                 // bctr
        encodeAddi(31, 0, 0),                        // li r31, 0
        encodeBForm(0x0C, false, false),             // b 0x80000034
        encodeAddi(31, 0, 1),                        // li r31, 1
        0x4E800020u,                                 // blr (default / case 2)
        0x4E800020u,                                 // blr (join)
    });
    binary.addDataWords(0x80000100u, {
        0x80000024u,
        0x8000002Cu,
        0x80000030u,
    });

    Analyzer analyzer(binary);
    analyzer.analyze(binary.getEntryPoint());

    const BasicBlock* jumpBlock = nullptr;
    for (const auto& [addr, block] : analyzer.getCfg().getBlocks()) {
        if (block.localJumpTable) {
            jumpBlock = &block;
            break;
        }
    }
    if (jumpBlock == nullptr || !jumpBlock->localJumpTable) {
        return false;
    }

    Lifter lifter;
    IRBlock ir = lifter.liftBlock(*jumpBlock);
    if (ir.instructions.empty() || ir.instructions.back().op != IROp::BranchTable) {
        return false;
    }

    for (const auto& instr : ir.instructions) {
        if (instr.op == IROp::BranchIndirect || instr.op == IROp::Mtspr) {
            return false;
        }
        if (instr.op == IROp::Load32 &&
            !instr.operands.empty() &&
            instr.operands[0].type == IROperandType::Register &&
            instr.operands[0].value == 0) {
            return false;
        }
    }

    return true;
}

bool testOptimizerKeepsStoreSourcesAndFprsSeparate() {
    Optimizer optimizer;
    IRBlock block;
    block.instructions.push_back({ IROp::SetImm, { IROperand::Reg(3), IROperand::Imm(1) } });
    block.instructions.push_back({ IROp::Store32, { IROperand::Reg(3), IROperand::Imm(0x1000), IROperand::Imm(0) } });
    block.instructions.push_back({ IROp::Add, { IROperand::Reg(4), IROperand::Reg(3), IROperand::Imm(2) } });
    block.instructions.push_back({ IROp::SetImm, { IROperand::Reg(1), IROperand::Imm(0x1234) } });
    block.instructions.push_back({ IROp::FAdd, { IROperand::FReg(1), IROperand::FReg(2), IROperand::FReg(3) } });

    optimizer.optimizeBlock(block);

    if (block.instructions.size() != 5) {
        return false;
    }
    if (block.instructions[0].op != IROp::SetImm || block.instructions[1].op != IROp::Store32) {
        return false;
    }
    if (block.instructions[2].op != IROp::SetImm || block.instructions[2].operands[1].value != 3) {
        return false;
    }
    if (block.instructions[4].op != IROp::FAdd || block.instructions[4].operands[1].type != IROperandType::Register) {
        return false;
    }
    return block.instructions[4].operands[1].regClass == IRRegisterClass::FPR;
}

bool testOptimizerClearsConstantsAfterLoadClobbers() {
    Optimizer optimizer;
    IRBlock block;
    block.instructions.push_back({ IROp::SetImm, { IROperand::Reg(4), IROperand::Imm(0x66660001u) } });
    block.instructions.push_back({ IROp::Load32, { IROperand::Reg(4), IROperand::Reg(13), IROperand::Imm(0x1F30u) } });
    block.instructions.push_back({ IROp::Load32, { IROperand::Reg(3), IROperand::Reg(4), IROperand::Imm(0x1530u) } });

    optimizer.optimizeBlock(block);

    if (block.instructions.size() != 2) {
        return false;
    }
    if (block.instructions[0].op != IROp::Load32 || block.instructions[1].op != IROp::Load32) {
        return false;
    }
    return block.instructions[1].operands[1].type == IROperandType::Register &&
           block.instructions[1].operands[1].value == 4 &&
           block.instructions[1].operands[1].regClass == IRRegisterClass::GPR;
}

bool testOptimizerPreservesReadModifyWriteDestRegisters() {
    Optimizer optimizer;
    IRBlock block;
    block.instructions.push_back({ IROp::SetImm, { IROperand::Reg(8), IROperand::Imm(0xF3000000u) } });
    block.instructions.push_back({ IROp::Rlwimi, {
        IROperand::Reg(8),
        IROperand::Reg(4),
        IROperand::Imm(0),
        IROperand::Imm(24),
        IROperand::Imm(31)
    } });

    optimizer.optimizeBlock(block);

    if (block.instructions.size() != 2) {
        return false;
    }

    const IRInstruction& rlwimi = block.instructions[1];
    return rlwimi.op == IROp::Rlwimi &&
           rlwimi.operands.size() == 5 &&
           rlwimi.operands[0].type == IROperandType::Register &&
           rlwimi.operands[0].value == 8 &&
           rlwimi.operands[0].regClass == IRRegisterClass::GPR;
}

bool testEmitterTracksPcAtBlockEntry() {
    ControlFlowGraph cfg;
    Function function;
    function.startAddr = 0x80000000;
    function.name = "pc_track_fn";
    function.blocks.insert(0x80000000);
    cfg.addFunction(function);

    BasicBlock block;
    block.startAddr = 0x80000000;
    block.irInstructions.push_back({ IROp::Return, {} });
    cfg.addBlock(block);

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(function, cfg);
    return emitted.find("ctx->pc = 0x80000000;") != std::string::npos;
}

bool testRuntimeHelpers() {
    std::vector<u8> memory(0x100, 0);
    ram = memory.data();

    MEM_WRITE32(0x10, 0x12345678u);
    if (memory[0x10] != 0x12 || memory[0x11] != 0x34 || memory[0x12] != 0x56 || memory[0x13] != 0x78) {
        return false;
    }
    if (MEM_READ32(0x10) != 0x12345678u) {
        return false;
    }

    MEM_WRITE16(0x20, 0xFEDCu);
    if (MEM_READ16A(0x20) != 0xFFFFFEDCu) {
        return false;
    }

    CPUContext ctx {};
    mtcrf(&ctx, 0x80, 0xA0000000u);
    if ((ctx.cr & 0xF0000000u) != 0xA0000000u) {
        return false;
    }

    set_spr(&ctx, 0x398, 0x12345678u);
    if (get_spr(&ctx, 0x398) != 0x12345678u) {
        return false;
    }

    if (PPC_DIVWU(&ctx, 12u, 3u) != 4u) {
        return false;
    }
    if (PPC_DIVWU(&ctx, 12u, 0u) != 0u) {
        return false;
    }
    if (PPC_MULHWU(&ctx, 0xFFFFFFFFu, 2u) != 1u) {
        return false;
    }
    if (PPC_MULHW(&ctx, 0x80000000u, 2u) != 0xFFFFFFFFu) {
        return false;
    }
    if ((s32)PPC_DIVW(&ctx, (u32)-12, (u32)3) != -4) {
        return false;
    }
    if (PPC_DIVW(&ctx, 12u, 0u) != 0u) {
        return false;
    }
    if (PPC_DIVW(&ctx, 0x80000000u, 0xFFFFFFFFu) != 0u) {
        return false;
    }

    ctx.msr = 0x42u;
    if (ctx.msr != 0x42u) {
        return false;
    }

    ctx.cr = 0;
    set_fp_cr_field(&ctx, 0, 1.0, 2.0, 1);
    if ((ctx.cr & 0xF0000000u) != 0x80000000u) {
        return false;
    }

    ctx.cr = 0;
    set_fp_cr_field(&ctx, 1, 2.0, 1.0, 0);
    if ((ctx.cr & 0x0F000000u) != 0x04000000u) {
        return false;
    }

    ctx.cr = 0;
    set_fp_cr_field(&ctx, 2, 2.0, 2.0, 1);
    if ((ctx.cr & 0x00F00000u) != 0x00200000u) {
        return false;
    }

    u64 nanBits = 0x7FF8000000000000ull;
    f64 nanValue = 0.0;
    memcpy(&nanValue, &nanBits, sizeof(nanValue));
    ctx.cr = 0;
    set_fp_cr_field(&ctx, 3, nanValue, 1.0, 1);
    if ((ctx.cr & 0x000F0000u) != 0x00010000u) {
        return false;
    }

    const f64 converted = FCTIW(3.75);
    MEM_WRITE_DOUBLE(0x30, converted);
    return MEM_READ32(0x34) == 3;
}

bool testAnalyzerRegistersDirectCallTargets() {
    TestBinary binary;
    binary.setTextWords(0x80000000, {
        encodeBForm(0x8, false, true), // bl 0x80000008
        0x4E800020u,                   // blr
        0x4E800020u,                   // target: blr
    });

    Analyzer analyzer(binary);
    analyzer.analyze(binary.getEntryPoint());

    const auto& functions = analyzer.getCfg().getFunctions();
    return functions.find(0x80000008u) != functions.end();
}

bool testAnalyzerRegistersDataReferencedEntryStubs() {
    TestBinary binary;
    binary.setTextWords(0x80000000u, {
        0x4E800020u, // blr
        0x4E800020u, // blr: data-referenced callback stub
    });
    binary.addDataWords(0x80000100u, {
        0x80000004u,
    });

    Analyzer analyzer(binary);
    analyzer.analyze(binary.getEntryPoint());

    const auto& functions = analyzer.getCfg().getFunctions();
    return functions.find(0x80000004u) != functions.end();
}

bool testAnalyzerRegistersTextReferencedEntryStubs() {
    TestBinary binary;
    binary.setTextWords(0x80000000u, {
        0x4E800020u,                                   // blr
        0x4E800020u,                                   // blr: text-referenced callback stub
        0x60000000u,                                   // nop padding
        encodeAddis(4, 0, static_cast<s16>(0x8000)),  // lis r4, 0x8000
        encodeAddi(3, 1, 0x0008),                     // unrelated addi between hi/lo materialization
        encodeAddi(6, 4, 0x0004),                     // addi r6, r4, 4
        encodeAddis(4, 0, static_cast<s16>(0x1234)),  // unrelated literal high
        encodeOri(4, 4, 0x5678),                      // unrelated literal low
        0x4E800020u,                                   // blr
    });
    binary.setEntryPoint(0x8000000Cu);

    Analyzer analyzer(binary);
    analyzer.analyze(binary.getEntryPoint());

    const auto& functions = analyzer.getCfg().getFunctions();
    return functions.find(0x80000004u) != functions.end();
}

bool testAnalyzerRegistersSplitTextReferencedCallbacks() {
    TestBinary binary;
    binary.setTextWords(0x80000000u, {
        0x9421FFF0u,                                   // stwu r1, -16(r1)
        encodeAddi(31, 0, 0),                         // li r31, 0
        encodeAddis(4, 0, static_cast<s16>(0x8000)),  // lis r4, 0x8000
        encodeAddi(3, 1, 0x0008),                     // unrelated scheduling gap
        encodeAddi(6, 4, 0x0018),                     // addi r6, r4, 0x18
        0x4E800020u,                                  // blr
        encodeAddi(4, 4, 1),                          // callback entry at 0x80000018
        0x4E800020u,                                  // blr
    });
    binary.setEntryPoint(0x8000000Cu);

    Analyzer analyzer(binary);
    analyzer.analyze(binary.getEntryPoint());

    const auto& functions = analyzer.getCfg().getFunctions();
    return functions.find(0x80000018u) != functions.end();
}

bool testAnalyzerRegistersLongGapTextReferencedCallbacks() {
    TestBinary binary;
    binary.setTextWords(0x80000000u, {
        0x9421FFF0u,                                   // stwu r1, -16(r1)
        encodeAddis(7, 0, static_cast<s16>(0x8000)),  // lis r7, 0x8000
        encodeAddis(6, 0, static_cast<s16>(0x8000)),  // lis r6, 0x8000
        encodeAddis(5, 0, static_cast<s16>(0x8000)),  // lis r5, 0x8000
        encodeAddi(31, 0, 0),                         // unrelated setup
        encodeAddi(3, 1, 8),                          // unrelated setup
        encodeAddi(4, 1, 12),                         // unrelated setup
        encodeAddi(0, 7, 0x0034),                     // unrelated callback pointer
        encodeAddi(6, 6, 0x0038),                     // unrelated callback pointer
        encodeAddi(5, 5, 0x0030),                     // callback entry at 0x80000030
        0x60000000u,                                  // nop padding
        0x4E800020u,                                  // blr before callback entry
        encodeAddi(3, 3, 1),                          // callback entry at 0x80000030
        0x4E800020u,                                  // blr
        0x4E800020u,                                  // unrelated stub
    });

    Analyzer analyzer(binary);
    analyzer.analyze(binary.getEntryPoint());

    const auto& functions = analyzer.getCfg().getFunctions();
    return functions.find(0x80000030u) != functions.end();
}

bool testAnalyzerPreservesOverlappingEntryFallthrough() {
    TestBinary binary;
    binary.setTextWords(0x80000000u, {
        encodeBForm(0x10, false, true),  // bl 0x80000010
        0x4E800020u,                     // blr
        0x60000000u,                     // nop padding
        0x60000000u,                     // nop padding
        0x7C0802A6u,                     // mflr r0
        encodeDForm(36, 0, 1, 4),        // stw r0, 4(r1)
        0x9421FFF0u,                     // stwu r1, -16(r1) -> overlapping prologue at 0x80000018
        encodeAddi(3, 0, 1),             // li r3, 1
        0x4E800020u,                     // blr
    });

    Analyzer analyzer(binary);
    analyzer.analyze(binary.getEntryPoint());

    const Function* function = analyzer.getCfg().getFunction(0x80000010u);
    if (function == nullptr) {
        std::ofstream debug("debug_analyzer_overlapping_entries.txt", std::ios::trunc);
        debug << "missing function 0x80000010\n";
        for (const auto& [addr, discovered] : analyzer.getCfg().getFunctions()) {
            debug << std::hex << addr << " " << discovered.name << "\n";
        }
        return false;
    }

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(*function, analyzer.getCfg());
    return emitted.find("label_0x80000018:") != std::string::npos &&
           emitted.find("ctx->gpr[1] = ctx->gpr[1] + 0xfffffff0;") != std::string::npos;
}

bool testAnalyzerKeepsInternalCallTargetsLocal() {
    TestBinary binary;
    binary.setTextWords(0x80000000u, {
        encodeAddi(4, 0, 1),                        // li r4, 1
        encodeAddi(4, 4, 1),                        // addi r4, r4, 1
        encodeAddi(3, 3, 2),                        // local target body
        0x4E800020u,                                // blr
        0x9421FFF0u,                                // real function entry at 0x80000010
        encodeBForm(static_cast<u32>(-12), false, true), // bl 0x80000008
        0x4E800020u,                                // blr
    });
    binary.setEntryPoint(0x80000010u);

    Analyzer analyzer(binary);
    analyzer.analyze(binary.getEntryPoint());

    if (analyzer.getCfg().getFunction(0x80000008u) != nullptr) {
        return false;
    }

    const Function* function = analyzer.getCfg().getFunction(0x80000010u);
    if (function == nullptr) {
        return false;
    }

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(*function, analyzer.getCfg());
    return emitted.find("ctx->lr = 0x80000018; goto label_0x80000008;") != std::string::npos &&
           emitted.find("case 0x80000018: goto label_0x80000018;") != std::string::npos;
}

bool testAnalyzerDoesNotInventNonExecutableLocalCallTargets() {
    TestBinary binary;
    binary.setTextWords(0x80000000u, {
        0x48000063u, // bla 0x60
        0x4E800020u, // blr
    });

    Analyzer analyzer(binary);
    analyzer.analyze(binary.getEntryPoint());

    const Function* function = analyzer.getCfg().getFunction(0x80000000u);
    if (function == nullptr) {
        return false;
    }

    Emitter emitter;
    const std::string emitted = emitter.emitFunction(*function, analyzer.getCfg());
    return emitted.find("goto label_0x60;") == std::string::npos &&
           emitted.find("case 0x00000060: goto label_0x60;") == std::string::npos &&
           emitted.find("call_by_addr(ctx, 0x00000060);") != std::string::npos;
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };

    const TestCase tests[] = {
        { "branch_decoding", testBranchDecoding },
        { "lifter_zero_base_and_compare", testLifterZeroBaseAndCompareField },
        { "lifter_indirect_call", testLifterIndirectCall },
        { "lifter_time_base_read", testLifterTimeBaseRead },
        { "lifter_msr_ops", testLifterMsrOps },
        { "lifter_interrupt_return", testLifterInterruptReturn },
        { "lifter_record_cr0", testLifterRecordFormUpdatesCr0 },
        { "lifter_divwu", testLifterDivwu },
        { "lifter_mulhwu", testLifterMulhwu },
        { "lifter_mulhw", testLifterMulhw },
        { "lifter_divw", testLifterDivw },
        { "lifter_fcmp", testLifterFloatingCompare },
        { "lifter_fpu_aform_operands", testLifterFloatingAFormOperands },
        { "lifter_fpu_unary_ops", testLifterFloatingUnaryOps },
        { "emitter_helpers", testEmitterUsesNewHelpers },
        { "emitter_resume_pc", testEmitterResumesAtSavedPc },
        { "emitter_lr_returns", testEmitterTreatsLrBranchesAsReturns },
        { "emitter_ctr_locals", testEmitterDispatchesLocalCtrBranches },
        { "emitter_no_fake_lr_resumes", testEmitterDoesNotInventLocalLrResumes },
        { "emitter_ignores_missing_seed_blocks", testEmitterIgnoresMissingFunctionSeedBlocks },
        { "emitter_sets_lr_for_external_calls", testEmitterSetsLrForExternalCalls },
        { "emitter_preserves_lr_indirect_target", testEmitterPreservesLrIndirectCallTarget },
        { "emitter_logs_unexpected_local_resume_targets", testEmitterLogsUnexpectedLocalResumeTargets },
        { "analyzer_local_jump_tables", testAnalyzerEmitsLocalJumpTablesAsSwitches },
        { "analyzer_interleaved_jump_table_setup", testAnalyzerHandlesInterleavedJumpTableSetup },
        { "lifter_clobbered_local_jump_table_index", testLifterPreservesClobberedLocalJumpTableIndex },
        { "optimizer_safety", testOptimizerKeepsStoreSourcesAndFprsSeparate },
        { "optimizer_load_clobbers", testOptimizerClearsConstantsAfterLoadClobbers },
        { "optimizer_rmw_dest_regs", testOptimizerPreservesReadModifyWriteDestRegisters },
        { "emitter_tracks_pc", testEmitterTracksPcAtBlockEntry },
        { "runtime_helpers", testRuntimeHelpers },
        { "analyzer_direct_call_targets", testAnalyzerRegistersDirectCallTargets },
        { "analyzer_data_pointer_entries", testAnalyzerRegistersDataReferencedEntryStubs },
        { "analyzer_text_pointer_entries", testAnalyzerRegistersTextReferencedEntryStubs },
        { "analyzer_text_pointer_split_callbacks", testAnalyzerRegistersSplitTextReferencedCallbacks },
        { "analyzer_text_pointer_long_gap_callbacks", testAnalyzerRegistersLongGapTextReferencedCallbacks },
        { "analyzer_overlapping_entries", testAnalyzerPreservesOverlappingEntryFallthrough },
        { "analyzer_internal_call_targets", testAnalyzerKeepsInternalCallTargetsLocal },
        { "analyzer_non_exec_call_targets", testAnalyzerDoesNotInventNonExecutableLocalCallTargets },
    };

    std::vector<std::string> failures;
    for (const auto& test : tests) {
        if (!test.run()) {
            failures.push_back(test.name);
        }
    }

    std::ofstream report("test_results.txt", std::ios::trunc);
    if (failures.empty()) {
        report << "PASS\n";
        return 0;
    }

    report << "FAIL\n";
    for (const auto& failure : failures) {
        report << failure << "\n";
    }
    return 1;
}
