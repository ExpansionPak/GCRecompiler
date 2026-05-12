#include <gcrecomp/analysis/analyzer.h>
#include <gcrecomp/codegen/emitter.h>
#include <gcrecomp/log.h>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <sstream>

namespace gcrecomp {

namespace {

inline s32 signExtend(u32 value, u32 width) {
    const u32 shift = 32 - width;
    return static_cast<s32>(value << shift) >> shift;
}

bool decodeLis(u32 raw, u32& rt, s16& imm) {
    if ((raw >> 26) != 15 || ((raw >> 16) & 0x1F) != 0) {
        return false;
    }
    rt = (raw >> 21) & 0x1F;
    imm = static_cast<s16>(raw & 0xFFFF);
    return true;
}

bool decodeAddi(u32 raw, u32& rt, u32& ra, s16& imm) {
    if ((raw >> 26) != 14) {
        return false;
    }
    rt = (raw >> 21) & 0x1F;
    ra = (raw >> 16) & 0x1F;
    imm = static_cast<s16>(raw & 0xFFFF);
    return true;
}

bool decodeOri(u32 raw, u32& ra, u32& rs, u16& imm) {
    if ((raw >> 26) != 24) {
        return false;
    }
    rs = (raw >> 21) & 0x1F;
    ra = (raw >> 16) & 0x1F;
    imm = static_cast<u16>(raw & 0xFFFF);
    return true;
}

bool decodeScaleWordOffset(u32 raw, u32& ra, u32& rs) {
    if ((raw >> 26) != 21) {
        return false;
    }

    const u32 sh = (raw >> 11) & 0x1F;
    const u32 mb = (raw >> 6) & 0x1F;
    const u32 me = (raw >> 1) & 0x1F;
    if (sh != 2 || mb != 0 || me != 29) {
        return false;
    }

    rs = (raw >> 21) & 0x1F;
    ra = (raw >> 16) & 0x1F;
    return true;
}

bool decodeLwzx(u32 raw, u32& rt, u32& ra, u32& rb) {
    if ((raw >> 26) != 31 || ((raw >> 1) & 0x3FF) != 23) {
        return false;
    }

    rt = (raw >> 21) & 0x1F;
    ra = (raw >> 16) & 0x1F;
    rb = (raw >> 11) & 0x1F;
    return true;
}

bool decodeMtspr(u32 raw, u32& spr, u32& rs) {
    if ((raw >> 26) != 31 || ((raw >> 1) & 0x3FF) != 467) {
        return false;
    }

    rs = (raw >> 21) & 0x1F;
    const u32 sprLo = (raw >> 16) & 0x1F;
    const u32 sprHi = (raw >> 11) & 0x1F;
    spr = (sprHi << 5) | sprLo;
    return true;
}

bool decodeCmplwi(u32 raw, u32& ra, u16& imm) {
    if ((raw >> 26) != 10) {
        return false;
    }

    ra = (raw >> 16) & 0x1F;
    imm = raw & 0xFFFF;
    return true;
}

bool isControlFlowBoundary(u32 raw) {
    const u32 op = raw >> 26;
    return op == 16 || op == 18 || op == 19;
}

bool isStackFramePrologue(u32 raw) {
    return (raw >> 16) == 0x9421;
}

bool hasNearbyStackFramePrologue(const Binary& binary, u32 addr);

bool isLinkSavePrelude(const Binary& binary, u32 addr) {
    if (!binary.isExecutable(addr)) {
        return false;
    }

    u32 first = 0;
    if (!binary.read32(addr, first)) {
        return false;
    }

    return first == 0x7C0802A6u && hasNearbyStackFramePrologue(binary, addr);
}

bool hasNearbyStackFramePrologue(const Binary& binary, u32 addr) {
    for (u32 offset = 0; offset <= 8; offset += 4) {
        const u32 probe = addr + offset;
        if (!binary.isExecutable(probe)) {
            break;
        }

        u32 raw = 0;
        if (!binary.read32(probe, raw)) {
            break;
        }

        if (isStackFramePrologue(raw)) {
            return true;
        }
    }

    return false;
}

bool isTextSectionStart(const Binary& binary, u32 addr) {
    for (const auto& sec : binary.getSections()) {
        if (sec.isText && sec.address == addr) {
            return true;
        }
    }
    return false;
}

u32 normalizeExecutableReference(u32 addr) {
    if (addr < 0x01800000u) {
        return addr | 0x80000000u;
    }

    if (addr >= 0xC0000000u && addr < 0xC1800000u) {
        return (addr & 0x1FFFFFFFu) | 0x80000000u;
    }

    return addr;
}

bool looksLikeDataReferencedEntry(const Binary& binary, Disassembler& disasm, u32 addr) {
    if ((addr & 3u) != 0 || !binary.isExecutable(addr)) {
        return false;
    }

    u32 raw = 0;
    if (!binary.read32(addr, raw)) {
        return false;
    }

    if (isStackFramePrologue(raw) ||
        isLinkSavePrelude(binary, addr) ||
        hasNearbyStackFramePrologue(binary, addr) ||
        isTextSectionStart(binary, addr)) {
        return true;
    }

    Instruction instr;
    if (!disasm.disassemble(binary, addr, instr)) {
        return false;
    }

    if (instr.type == InstructionType::Return || instr.type == InstructionType::Branch) {
        return true;
    }

    if (addr < 4 || !binary.isExecutable(addr - 4)) {
        return true;
    }

    Instruction prevInstr;
    if (!disasm.disassemble(binary, addr - 4, prevInstr)) {
        return true;
    }

    return prevInstr.isBranch || prevInstr.type == InstructionType::Return;
}

bool decodeAbsoluteAddressLoad(const Binary& binary, u32 addr, u32& target) {
    constexpr u32 kMaxAddressMaterializationScanBytes = 0x40;

    u32 lisRaw = 0;
    if (!binary.read32(addr, lisRaw)) {
        return false;
    }

    u32 reg = 0;
    s16 hi = 0;
    if (!decodeLis(lisRaw, reg, hi)) {
        return false;
    }

    // PPC callback setup often batches several `lis` instructions first and only
    // resolves each low half later in the same basic block.
    for (u32 offset = 4; offset <= kMaxAddressMaterializationScanBytes; offset += 4) {
        u32 loRaw = 0;
        if (!binary.read32(addr + offset, loRaw)) {
            break;
        }
        if (isControlFlowBoundary(loRaw)) {
            break;
        }

        u32 nextRt = 0;
        s16 nextHi = 0;
        if (decodeLis(loRaw, nextRt, nextHi) && nextRt == reg) {
            break;
        }

        u32 loRt = 0;
        u32 loRa = 0;
        s16 loAddi = 0;
        if (decodeAddi(loRaw, loRt, loRa, loAddi) && loRa == reg) {
            target = (static_cast<u32>(static_cast<u16>(hi)) << 16) +
                static_cast<u32>(static_cast<s32>(loAddi));
            return true;
        }
        if (decodeAddi(loRaw, loRt, loRa, loAddi) && loRt == reg) {
            break;
        }

        u32 loOriRa = 0;
        u32 loOriRs = 0;
        u16 loOri = 0;
        if (decodeOri(loRaw, loOriRa, loOriRs, loOri) && loOriRs == reg) {
            target = (static_cast<u32>(static_cast<u16>(hi)) << 16) | static_cast<u32>(loOri);
            return true;
        }
        if (decodeOri(loRaw, loOriRa, loOriRs, loOri) && loOriRa == reg && loOriRs != reg) {
            break;
        }
    }

    return false;
}

bool looksLikeIndependentFunctionEntry(const Binary& binary, Disassembler& disasm, u32 addr) {
    if ((addr & 3u) != 0 || !binary.isExecutable(addr)) {
        return false;
    }

    u32 raw = 0;
    if (!binary.read32(addr, raw)) {
        return false;
    }

    if (isStackFramePrologue(raw) || isTextSectionStart(binary, addr)) {
        return true;
    }

    if (addr < 4 || !binary.isExecutable(addr - 4)) {
        return true;
    }

    Instruction prevInstr;
    if (!disasm.disassemble(binary, addr - 4, prevInstr)) {
        return true;
    }

    return prevInstr.isBranch || prevInstr.type == InstructionType::Return;
}

bool decodeConditionalBranch(u32 raw, u32 addr, u32& target, u32& bo, u32& bi) {
    if ((raw >> 26) != 16) {
        return false;
    }

    bo = (raw >> 21) & 0x1F;
    bi = (raw >> 16) & 0x1F;
    const bool aa = ((raw >> 1) & 1u) != 0;
    const s32 disp = signExtend(raw & 0x0000FFFCu, 16);
    target = aa ? static_cast<u32>(disp) : static_cast<u32>(static_cast<s32>(addr) + disp);
    return true;
}

bool isLikelyLocalJumpTarget(u32 blockAddr, u32 target) {
    const s64 delta = static_cast<s64>(static_cast<s32>(target)) -
        static_cast<s64>(static_cast<s32>(blockAddr));
    return delta >= -0x1000 && delta <= 0x1000;
}

std::optional<BasicBlock::LocalJumpTable> detectLocalJumpTable(
    const Binary& binary,
    const BasicBlock& block) {
    if (block.instructions.size() < 5) {
        return std::nullopt;
    }

    const Instruction& branch = block.instructions.back();
    if (branch.type != InstructionType::Branch ||
        branch.branchRegisterTarget != BranchRegisterTarget::CountRegister ||
        branch.isLink) {
        return std::nullopt;
    }

    const size_t count = block.instructions.size();

    u32 ctrSpr = 0;
    u32 tableValueReg = 0;
    if (!decodeMtspr(block.instructions[count - 2].raw, ctrSpr, tableValueReg) || ctrSpr != 9) {
        return std::nullopt;
    }

    u32 loadedReg = 0;
    u32 tableReg = 0;
    u32 offsetReg = 0;
    if (!decodeLwzx(block.instructions[count - 3].raw, loadedReg, tableReg, offsetReg) ||
        loadedReg != tableValueReg) {
        return std::nullopt;
    }

    u32 indexReg = 0;
    size_t scaleInstructionIndex = count;
    for (size_t i = count - 3; i-- > 0;) {
        u32 computedOffsetReg = 0;
        u32 candidateIndexReg = 0;
        if (!decodeScaleWordOffset(block.instructions[i].raw, computedOffsetReg, candidateIndexReg) ||
            computedOffsetReg != offsetReg) {
            continue;
        }

        scaleInstructionIndex = i;
        indexReg = candidateIndexReg;
        break;
    }

    if (scaleInstructionIndex == count) {
        return std::nullopt;
    }

    u32 tableBaseReg = 0;
    s16 tableBaseHi = 0;
    size_t tableBaseInstructionIndex = count;
    bool foundTableBase = false;
    for (size_t i = 0; i < count - 3; ++i) {
        u32 lisRt = 0;
        s16 lisImm = 0;
        if (!decodeLis(block.instructions[i].raw, lisRt, lisImm) || lisRt != tableReg) {
            continue;
        }

        for (size_t j = i + 1; j < count - 2; ++j) {
            const u32 candidateRaw = block.instructions[j].raw;
            if (isControlFlowBoundary(candidateRaw)) {
                break;
            }

            u32 nextLisRt = 0;
            s16 nextLisImm = 0;
            if (decodeLis(candidateRaw, nextLisRt, nextLisImm) && nextLisRt == tableReg) {
                break;
            }

            u32 addiRt = 0;
            u32 addiRa = 0;
            s16 addiImm = 0;
            if (!decodeAddi(candidateRaw, addiRt, addiRa, addiImm) ||
                addiRt != tableReg || addiRa != tableReg) {
                continue;
            }

            tableBaseReg = (static_cast<u32>(static_cast<u16>(lisImm)) << 16) +
                static_cast<u32>(static_cast<s32>(addiImm));
            tableBaseHi = lisImm;
            tableBaseInstructionIndex = i;
            foundTableBase = true;
            break;
        }

        if (foundTableBase) {
            break;
        }
    }

    if (!foundTableBase || tableBaseHi == 0) {
        return std::nullopt;
    }

    u32 defaultTarget = 0;
    u16 maxIndex = 0;
    bool foundGuard = false;
    if (block.startAddr >= 8) {
        u32 branchRaw = 0;
        u32 cmpRaw = 0;
        if (binary.read32(block.startAddr - 4, branchRaw) &&
            binary.read32(block.startAddr - 8, cmpRaw)) {
            u32 bo = 0;
            u32 bi = 0;
            u32 branchTarget = 0;
            u32 cmpReg = 0;
            u16 cmpImm = 0;
            if (decodeConditionalBranch(branchRaw, block.startAddr - 4, branchTarget, bo, bi) &&
                decodeCmplwi(cmpRaw, cmpReg, cmpImm) &&
                cmpReg == indexReg &&
                bo == 12 && bi == 1) {
                defaultTarget = branchTarget;
                maxIndex = cmpImm;
                foundGuard = true;
            }
        }
    }

    if (!foundGuard || maxIndex > 0xFF) {
        return std::nullopt;
    }

    BasicBlock::LocalJumpTable jumpTable;
    jumpTable.indexRegister = indexReg;
    jumpTable.defaultTarget = defaultTarget;
    jumpTable.patternStartInstructionIndex = std::min(scaleInstructionIndex, tableBaseInstructionIndex);

    for (u32 i = 0; i <= maxIndex; ++i) {
        u32 target = 0;
        if (!binary.read32(tableBaseReg + (i * 4), target)) {
            return std::nullopt;
        }
        jumpTable.targets.push_back(target);
    }

    return jumpTable;
}

bool readTextFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }

    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    out << text;
    return true;
}

bool replaceOnce(std::string& text, const std::string& from, const std::string& to) {
    const size_t pos = text.find(from);
    if (pos == std::string::npos) {
        return false;
    }

    text.replace(pos, from.size(), to);
    return true;
}

void patchGeneratedGxShader(const std::filesystem::path& shaderPath) {
    std::string source;
    if (!readTextFile(shaderPath, source) || source.find("u_tev_color_chan") != std::string::npos) {
        return;
    }

    bool changed = false;
    changed |= replaceOnce(source,
        "uniform int u_tev_tex_map[GX_MAX_TEV_STAGES];\n",
        "uniform int u_tev_tex_map[GX_MAX_TEV_STAGES];\n"
        "uniform int u_tev_color_chan[GX_MAX_TEV_STAGES];\n");
    changed |= replaceOnce(source,
        "uniform int u_light_mask;\n"
        "uniform vec3 u_light_pos[8];\n",
        "uniform int u_light_mask;\n"
        "uniform int u_lighting_enabled1;\n"
        "uniform vec4 u_mat_color1;\n"
        "uniform vec4 u_amb_color1;\n"
        "uniform int u_chan_mat_src1;\n"
        "uniform int u_chan_amb_src1;\n"
        "uniform int u_alpha_lighting_enabled1;\n"
        "uniform int u_alpha_mat_src1;\n"
        "uniform int u_light_mask1;\n"
        "uniform vec3 u_light_pos[8];\n");
    changed |= replaceOnce(source,
        "bool alphaTest(int comp, float val, float ref) {\n"
        "    const float EPS = 0.5 / 255.0;\n"
        "    if (comp == 0) return false;                   /* NEVER */\n"
        "    if (comp == 1) return val < ref;               /* LESS */\n"
        "    if (comp == 2) return abs(val - ref) < EPS;    /* EQUAL */\n"
        "    if (comp == 3) return val <= ref;              /* LEQUAL */\n"
        "    if (comp == 4) return val > ref;               /* GREATER */\n"
        "    if (comp == 5) return abs(val - ref) >= EPS;   /* NEQUAL */\n"
        "    if (comp == 6) return val >= ref;              /* GEQUAL */\n"
        "    return true;                                   /* ALWAYS */\n"
        "}\n"
        "\n"
        "void main() {\n",
        "bool alphaTest(int comp, float val, float ref) {\n"
        "    const float EPS = 0.5 / 255.0;\n"
        "    if (comp == 0) return false;                   /* NEVER */\n"
        "    if (comp == 1) return val < ref;               /* LESS */\n"
        "    if (comp == 2) return abs(val - ref) < EPS;    /* EQUAL */\n"
        "    if (comp == 3) return val <= ref;              /* LEQUAL */\n"
        "    if (comp == 4) return val > ref;               /* GREATER */\n"
        "    if (comp == 5) return abs(val - ref) >= EPS;   /* NEQUAL */\n"
        "    if (comp == 6) return val >= ref;              /* GEQUAL */\n"
        "    return true;                                   /* ALWAYS */\n"
        "}\n"
        "\n"
        "vec4 evalSimpleChanRGB(int enabled, int matSrc, int ambSrc, vec4 matColor, vec4 ambColor, int lightMask) {\n"
        "    vec4 outColor;\n"
        "    vec3 matC = (matSrc != 0) ? v_color.rgb : matColor.rgb;\n"
        "    vec3 ambC = (ambSrc != 0) ? v_color.rgb : ambColor.rgb;\n"
        "    if (enabled != 0) {\n"
        "        vec3 lightAccum = ambC;\n"
        "        for (int i = 0; i < 8; i++) {\n"
        "            if ((lightMask & (1 << i)) != 0) {\n"
        "                vec3 L = normalize(u_light_pos[i]);\n"
        "                float diff = clamp(dot(v_normal, L), 0.0, 1.0);\n"
        "                lightAccum += diff * u_light_color[i].rgb;\n"
        "            }\n"
        "        }\n"
        "        outColor.rgb = matC * clamp(lightAccum, 0.0, 1.0);\n"
        "    } else {\n"
        "        outColor.rgb = matC;\n"
        "    }\n"
        "    outColor.a = (matSrc != 0) ? v_color.a : matColor.a;\n"
        "    return outColor;\n"
        "}\n"
        "\n"
        "vec4 rasterColorForChan(int chanSel, vec4 ras0, vec4 ras1) {\n"
        "    if (chanSel == 1 || chanSel == 3 || chanSel == 5 || chanSel == 8) return ras1;\n"
        "    if (chanSel == 6 || chanSel == 255) return vec4(1.0);\n"
        "    return ras0;\n"
        "}\n"
        "\n"
        "void main() {\n");
    changed |= replaceOnce(source,
        "    /* Rasterized color: GX lighting model */\n"
        "    vec4 rasColor;\n"
        "    if (u_num_chans == 0) {\n"
        "        rasColor = vec4(1.0);\n"
        "    } else {\n"
        "        vec3 matC = (u_chan_mat_src != 0) ? v_color.rgb : u_mat_color.rgb;\n"
        "        vec3 ambC = (u_chan_amb_src != 0) ? v_color.rgb : u_amb_color.rgb;\n"
        "        if (u_lighting_enabled != 0) {\n"
        "            vec3 lightAccum = ambC;\n"
        "            for (int i = 0; i < 8; i++) {\n"
        "                if ((u_light_mask & (1 << i)) != 0) {\n"
        "                    vec3 L = normalize(u_light_pos[i]);\n"
        "                    float diff = clamp(dot(v_normal, L), 0.0, 1.0);\n"
        "                    lightAccum += diff * u_light_color[i].rgb;\n"
        "                }\n"
        "            }\n"
        "            rasColor.rgb = matC * clamp(lightAccum, 0.0, 1.0);\n"
        "        } else {\n"
        "            rasColor.rgb = matC;\n"
        "        }\n"
        "\n"
        "        float matA = (u_alpha_mat_src != 0) ? v_color.a : u_mat_color.a;\n"
        "        if (u_alpha_lighting_enabled != 0) {\n"
        "            rasColor.a = matA * u_amb_color.a;\n"
        "        } else {\n"
        "            rasColor.a = matA;\n"
        "        }\n"
        "    }\n",
        "    /* Rasterized color: GX lighting model */\n"
        "    vec4 rasColor0;\n"
        "    vec4 rasColor1;\n"
        "    if (u_num_chans == 0) {\n"
        "        rasColor0 = vec4(1.0);\n"
        "        rasColor1 = vec4(1.0);\n"
        "    } else {\n"
        "        rasColor0 = evalSimpleChanRGB(u_lighting_enabled, u_chan_mat_src, u_chan_amb_src, u_mat_color, u_amb_color, u_light_mask);\n"
        "        rasColor1 = evalSimpleChanRGB(u_lighting_enabled1, u_chan_mat_src1, u_chan_amb_src1, u_mat_color1, u_amb_color1, u_light_mask1);\n"
        "\n"
        "        float matA0 = (u_alpha_mat_src != 0) ? v_color.a : u_mat_color.a;\n"
        "        rasColor0.a = (u_alpha_lighting_enabled != 0) ? (matA0 * u_amb_color.a) : matA0;\n"
        "\n"
        "        float matA1 = (u_alpha_mat_src1 != 0) ? v_color.a : u_mat_color1.a;\n"
        "        rasColor1.a = (u_alpha_lighting_enabled1 != 0) ? (matA1 * u_amb_color1.a) : matA1;\n"
        "    }\n");
    changed |= replaceOnce(source,
        "        vec4 sRas = applySwap(rasColor, u_swap_table[u_tev_swap[s].x]);\n",
        "        vec4 sRas = applySwap(rasterColorForChan(u_tev_color_chan[s], rasColor0, rasColor1), u_swap_table[u_tev_swap[s].x]);\n");

    if (changed) {
        writeTextFile(shaderPath, source);
    }
}

void patchGeneratedGxInternal(const std::filesystem::path& internalPath) {
    std::string source;
    if (!readTextFile(internalPath, source) || source.find("tev_color_chan[PC_GX_MAX_TEV_STAGES]") != std::string::npos) {
        return;
    }

    bool changed = false;
    changed |= replaceOnce(source,
        "        GLint alpha_lighting_enabled, alpha_mat_src;\n"
        "        GLint light_mask, light_pos[8], light_color[8];\n",
        "        GLint alpha_lighting_enabled, alpha_mat_src;\n"
        "        GLint lighting_enabled1, mat_color1, amb_color1;\n"
        "        GLint chan_mat_src1, chan_amb_src1;\n"
        "        GLint alpha_lighting_enabled1, alpha_mat_src1;\n"
        "        GLint light_mask, light_mask1, light_pos[8], light_color[8];\n");
    changed |= replaceOnce(source,
        "        GLint tev_tc_src[PC_GX_MAX_TEV_STAGES];\n"
        "        GLint tev_tex_map[PC_GX_MAX_TEV_STAGES];\n",
        "        GLint tev_tc_src[PC_GX_MAX_TEV_STAGES];\n"
        "        GLint tev_tex_map[PC_GX_MAX_TEV_STAGES];\n"
        "        GLint tev_color_chan[PC_GX_MAX_TEV_STAGES];\n");

    if (changed) {
        writeTextFile(internalPath, source);
    }
}

void patchGeneratedGxC(const std::filesystem::path& gxPath) {
    std::string source;
    if (!readTextFile(gxPath, source)) {
        return;
    }

    bool changed = false;
    changed |= replaceOnce(source,
        "        snprintf(name, sizeof(name), \"u_tev_tex_map[%d]\", i);\n"
        "        g_gx.uloc.tev_tex_map[i] = UL(name);\n"
        "        snprintf(name, sizeof(name), \"u_tev_ind_cfg[%d]\", i);\n",
        "        snprintf(name, sizeof(name), \"u_tev_tex_map[%d]\", i);\n"
        "        g_gx.uloc.tev_tex_map[i] = UL(name);\n"
        "        snprintf(name, sizeof(name), \"u_tev_color_chan[%d]\", i);\n"
        "        g_gx.uloc.tev_color_chan[i] = UL(name);\n"
        "        snprintf(name, sizeof(name), \"u_tev_ind_cfg[%d]\", i);\n");
    changed |= replaceOnce(source,
        "    g_gx.uloc.alpha_lighting_enabled = UL(\"u_alpha_lighting_enabled\");\n"
        "    g_gx.uloc.alpha_mat_src = UL(\"u_alpha_mat_src\");\n"
        "\n"
        "    g_gx.uloc.light_mask = UL(\"u_light_mask\");\n",
        "    g_gx.uloc.alpha_lighting_enabled = UL(\"u_alpha_lighting_enabled\");\n"
        "    g_gx.uloc.alpha_mat_src = UL(\"u_alpha_mat_src\");\n"
        "    g_gx.uloc.lighting_enabled1 = UL(\"u_lighting_enabled1\");\n"
        "    g_gx.uloc.mat_color1 = UL(\"u_mat_color1\");\n"
        "    g_gx.uloc.amb_color1 = UL(\"u_amb_color1\");\n"
        "    g_gx.uloc.chan_mat_src1 = UL(\"u_chan_mat_src1\");\n"
        "    g_gx.uloc.chan_amb_src1 = UL(\"u_chan_amb_src1\");\n"
        "    g_gx.uloc.alpha_lighting_enabled1 = UL(\"u_alpha_lighting_enabled1\");\n"
        "    g_gx.uloc.alpha_mat_src1 = UL(\"u_alpha_mat_src1\");\n"
        "\n"
        "    g_gx.uloc.light_mask = UL(\"u_light_mask\");\n"
        "    g_gx.uloc.light_mask1 = UL(\"u_light_mask1\");\n");
    changed |= replaceOnce(source,
        "                loc = UL(tev_tex_map[s]); if (loc >= 0) glUniform1i(loc, ts->tex_map);\n"
        "                loc = UL(tev_bsc[s]);  if (loc >= 0) glUniform4i(loc, ts->color_bias, ts->color_scale, ts->alpha_bias, ts->alpha_scale);\n",
        "                loc = UL(tev_tex_map[s]); if (loc >= 0) glUniform1i(loc, ts->tex_map);\n"
        "                loc = UL(tev_color_chan[s]); if (loc >= 0) glUniform1i(loc, ts->color_chan);\n"
        "                loc = UL(tev_bsc[s]);  if (loc >= 0) glUniform4i(loc, ts->color_bias, ts->color_scale, ts->alpha_bias, ts->alpha_scale);\n");
    changed |= replaceOnce(source,
        "            loc = UL(alpha_lighting_enabled); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_enable[1]);\n"
        "            loc = UL(alpha_mat_src); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_mat_src[1]);\n"
        "            {\n"
        "                int color_light_mask = g_gx.chan_ctrl_light_mask[0];\n",
        "            loc = UL(alpha_lighting_enabled); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_enable[1]);\n"
        "            loc = UL(alpha_mat_src); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_mat_src[1]);\n"
        "            loc = UL(lighting_enabled1); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_enable[2]);\n"
        "            loc = UL(mat_color1); if (loc >= 0) glUniform4fv(loc, 1, g_gx.chan_mat_color[1]);\n"
        "            loc = UL(amb_color1); if (loc >= 0) glUniform4fv(loc, 1, g_gx.chan_amb_color[1]);\n"
        "            loc = UL(chan_mat_src1); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_mat_src[2]);\n"
        "            loc = UL(chan_amb_src1); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_amb_src[2]);\n"
        "            loc = UL(alpha_lighting_enabled1); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_enable[3]);\n"
        "            loc = UL(alpha_mat_src1); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_mat_src[3]);\n"
        "            {\n"
        "                int color_light_mask1 = g_gx.chan_ctrl_light_mask[2];\n"
        "                loc = UL(light_mask1); if (loc >= 0) glUniform1i(loc, color_light_mask1);\n"
        "            }\n"
        "            {\n"
        "                int color_light_mask = g_gx.chan_ctrl_light_mask[0];\n");
    changed |= replaceOnce(source,
        "            loc = UL(mat_color1); if (loc >= 0) glUniform4f(loc,\n"
        "                g_gx.chan_mat_color[1].r / 255.0f,\n"
        "                g_gx.chan_mat_color[1].g / 255.0f,\n"
        "                g_gx.chan_mat_color[1].b / 255.0f,\n"
        "                g_gx.chan_mat_color[1].a / 255.0f);\n"
        "            loc = UL(amb_color1); if (loc >= 0) glUniform4f(loc,\n"
        "                g_gx.chan_amb_color[1].r / 255.0f,\n"
        "                g_gx.chan_amb_color[1].g / 255.0f,\n"
        "                g_gx.chan_amb_color[1].b / 255.0f,\n"
        "                g_gx.chan_amb_color[1].a / 255.0f);\n",
        "            loc = UL(mat_color1); if (loc >= 0) glUniform4fv(loc, 1, g_gx.chan_mat_color[1]);\n"
        "            loc = UL(amb_color1); if (loc >= 0) glUniform4fv(loc, 1, g_gx.chan_amb_color[1]);\n");
    changed |= replaceOnce(source,
        "                texcoord_nonzero);\n"
        "\n"
        "        if (g_gx.current_primitive == GX_QUADS && count >= 4 &&\n",
        "                texcoord_nonzero);\n"
        "\n"
        "        if (tex0_w > 0 && tex0_h > 0 && ts0->tex_coord >= 0 && ts0->tex_coord < PC_GX_MAX_TEX_GENS) {\n"
        "            const int tg = ts0->tex_coord;\n"
        "            const int src_id = g_gx.tex_gen_src[tg];\n"
        "            const int type_id = g_gx.tex_gen_type[tg];\n"
        "            const int mtx_id = g_gx.tex_gen_mtx[tg];\n"
        "            const int slot = pc_tex_mtx_id_to_slot(mtx_id);\n"
        "            float input[4] = {0.0f, 0.0f, 1.0f, 1.0f};\n"
        "            float gen_s = 0.0f;\n"
        "            float gen_t = 0.0f;\n"
        "            float row0[4] = {1.0f, 0.0f, 0.0f, 0.0f};\n"
        "            float row1[4] = {0.0f, 1.0f, 0.0f, 0.0f};\n"
        "\n"
        "            if (src_id == GX_TG_POS) {\n"
        "                input[0] = v0->position[0];\n"
        "                input[1] = v0->position[1];\n"
        "                input[2] = v0->position[2];\n"
        "            } else if (src_id >= GX_TG_TEX0 && src_id <= GX_TG_TEX7) {\n"
        "                const int si = src_id - GX_TG_TEX0;\n"
        "                input[0] = v0->texcoord[si][0];\n"
        "                input[1] = v0->texcoord[si][1];\n"
        "            } else if (src_id == GX_TG_COLOR0) {\n"
        "                input[0] = v0->color0[0] / 255.0f;\n"
        "                input[1] = v0->color0[1] / 255.0f;\n"
        "            } else if (src_id == GX_TG_COLOR1) {\n"
        "                input[0] = v0->color1[0] / 255.0f;\n"
        "                input[1] = v0->color1[1] / 255.0f;\n"
        "            } else if (src_id >= GX_TG_TEXCOORD0 && src_id <= GX_TG_TEXCOORD6) {\n"
        "                const int si = src_id - GX_TG_TEXCOORD0;\n"
        "                input[0] = v0->texcoord[si][0];\n"
        "                input[1] = v0->texcoord[si][1];\n"
        "            } else {\n"
        "                input[0] = v0->texcoord[0][0];\n"
        "                input[1] = v0->texcoord[0][1];\n"
        "            }\n"
        "\n"
        "            if (slot >= 0 && slot < 10) {\n"
        "                const float* tm = (const float*)g_gx.tex_mtx[slot];\n"
        "                for (int k = 0; k < 4; ++k) {\n"
        "                    row0[k] = tm[k];\n"
        "                    row1[k] = tm[4 + k];\n"
        "                }\n"
        "                gen_s = row0[0] * input[0] + row0[1] * input[1] + row0[2] * input[2] + row0[3] * input[3];\n"
        "                gen_t = row1[0] * input[0] + row1[1] * input[1] + row1[2] * input[2] + row1[3] * input[3];\n"
        "            } else {\n"
        "                gen_s = input[0];\n"
        "                gen_t = input[1];\n"
        "            }\n"
        "\n"
        "            fprintf(stderr,\n"
        "                    \"[GX/TEXGEN] frame=%d draw=%d stage0_tc=%d map=%d key=0x%08X size=%dx%d src=%d type=%d mtx=%d slot=%d input=%.4f,%.4f,%.4f,%.4f gen=%.6f,%.6f norm_by_tex=%.6f,%.6f row0=%.8f,%.8f,%.8f,%.8f row1=%.8f,%.8f,%.8f,%.8f rawA=0x%08X rawB=0x%08X\\n\",\n"
        "                    g_pc_gx_frame_number,\n"
        "                    pc_gx_draw_call_count,\n"
        "                    tg,\n"
        "                    tex0_map,\n"
        "                    (unsigned)((tex0_map >= 0 && tex0_map < 8) ? g_gx.tex_obj_key[tex0_map] : 0u),\n"
        "                    tex0_w,\n"
        "                    tex0_h,\n"
        "                    src_id,\n"
        "                    type_id,\n"
        "                    mtx_id,\n"
        "                    slot,\n"
        "                    input[0], input[1], input[2], input[3],\n"
        "                    gen_s,\n"
        "                    gen_t,\n"
        "                    tex0_w > 0 ? (gen_s / (float)tex0_w) : 0.0f,\n"
        "                    tex0_h > 0 ? (gen_t / (float)tex0_h) : 0.0f,\n"
        "                    row0[0], row0[1], row0[2], row0[3],\n"
        "                    row1[0], row1[1], row1[2], row1[3],\n"
        "                    (unsigned)g_pc_gx_fifo.raw_mat_idx_a,\n"
        "                    (unsigned)g_pc_gx_fifo.raw_mat_idx_b);\n"
        "        }\n"
        "\n"
        "        if (g_gx.current_primitive == GX_QUADS && count >= 4 &&\n");

    if (changed) {
        writeTextFile(gxPath, source);
    }
}

void patchGeneratedPadC(const std::filesystem::path& padPath) {
    std::string source;
    if (!readTextFile(padPath, source) || source.find("GCRECOMP_PAD_DISABLE_CONTROLLERS") != std::string::npos) {
        return;
    }

    bool changed = false;
    changed |= replaceOnce(source,
        "static int g_pad_auto_a_pulses_sent = 0;\n"
        "static SDL_SpinLock g_pad_snapshot_lock = 0;\n",
        "static int g_pad_auto_a_pulses_sent = 0;\n"
        "static int g_pad_disable_controllers_initialized = 0;\n"
        "static int g_pad_disable_controllers_flag = 0;\n"
        "static int g_pad_disable_keyboard_initialized = 0;\n"
        "static int g_pad_disable_keyboard_flag = 0;\n"
        "static SDL_SpinLock g_pad_snapshot_lock = 0;\n");
    changed |= replaceOnce(source,
        "static u32 pc_pad_channel_bit(int channel) {\n",
        "static int pc_pad_cached_disable_flag(const char* specific_name,\n"
        "                                      const char* label,\n"
        "                                      int* initialized,\n"
        "                                      int* flag) {\n"
        "    if (!*initialized) {\n"
        "        *flag = pc_pad_env_flag_enabled(\"GCRECOMP_PAD_DISABLE_INPUT\") ||\n"
        "                pc_pad_env_flag_enabled(specific_name);\n"
        "        if (*flag) {\n"
        "            fprintf(stderr, \"[PAD] %s disabled by env\\n\", label);\n"
        "        }\n"
        "        *initialized = 1;\n"
        "    }\n"
        "\n"
        "    return *flag;\n"
        "}\n"
        "\n"
        "static int pc_pad_disable_controllers_enabled(void) {\n"
        "    return pc_pad_cached_disable_flag(\"GCRECOMP_PAD_DISABLE_CONTROLLERS\",\n"
        "                                      \"live SDL controllers\",\n"
        "                                      &g_pad_disable_controllers_initialized,\n"
        "                                      &g_pad_disable_controllers_flag);\n"
        "}\n"
        "\n"
        "static int pc_pad_disable_keyboard_enabled(void) {\n"
        "    return pc_pad_cached_disable_flag(\"GCRECOMP_PAD_DISABLE_KEYBOARD\",\n"
        "                                      \"keyboard input\",\n"
        "                                      &g_pad_disable_keyboard_initialized,\n"
        "                                      &g_pad_disable_keyboard_flag);\n"
        "}\n"
        "\n"
        "static u32 pc_pad_channel_bit(int channel) {\n");
    changed |= replaceOnce(source,
        "static void pc_pad_refresh_controllers(void) {\n"
        "    int device_index;\n"
        "\n"
        "    pc_pad_prune_detached();\n",
        "static void pc_pad_refresh_controllers(void) {\n"
        "    int device_index;\n"
        "\n"
        "    if (pc_pad_disable_controllers_enabled()) {\n"
        "        for (device_index = 0; device_index < PC_PAD_MAX_CONTROLLERS; ++device_index) {\n"
        "            pc_pad_close_slot(device_index);\n"
        "        }\n"
        "        return;\n"
        "    }\n"
        "\n"
        "    pc_pad_prune_detached();\n");
    changed |= replaceOnce(source,
        "    /* Suppress keyboard-to-button mapping when typing into the in-game text editor */\n"
        "    if (!(g_pc_typing_mode && g_pc_editor_active)) {\n",
        "    /* Suppress keyboard-to-button mapping when typing into the in-game text editor */\n"
        "    if (!pc_pad_disable_keyboard_enabled() && !(g_pc_typing_mode && g_pc_editor_active)) {\n");

    if (changed) {
        writeTextFile(padPath, source);
    }
}

void patchGeneratedGxTextureC(const std::filesystem::path& texturePath) {
    std::string source;
    if (!readTextFile(texturePath, source) || source.find("Dolphin CMPR interpolation") != std::string::npos) {
        return;
    }

    bool changed = false;
    changed |= replaceOnce(source,
        "                    for (int c = 0; c < 3; c++) {\n"
        "                        palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;\n"
        "                        palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;\n"
        "                    }\n",
        "                    /* Dolphin CMPR interpolation: GX uses a 3/8 blend, not exact thirds. */\n"
        "                    for (int c = 0; c < 3; c++) {\n"
        "                        palette[2][c] = (u8)((3 * palette[1][c] + 5 * palette[0][c]) >> 3);\n"
        "                        palette[3][c] = (u8)((3 * palette[0][c] + 5 * palette[1][c]) >> 3);\n"
        "                    }\n");
    changed |= replaceOnce(source,
        "                    palette[3][0] = palette[3][1] = palette[3][2] = 0;\n"
        "                    palette[3][3] = 0;\n",
        "                    /* GX CMPR keeps the averaged RGB for the transparent entry. */\n"
        "                    palette[3][0] = palette[2][0];\n"
        "                    palette[3][1] = palette[2][1];\n"
        "                    palette[3][2] = palette[2][2];\n"
        "                    palette[3][3] = 0;\n");

    if (changed) {
        writeTextFile(texturePath, source);
    }
}

void patchGeneratedMtxC(const std::filesystem::path& mtxPath) {
    std::string source;
    if (!readTextFile(mtxPath, source) || source.find("PSMTXTransApply preserves aliased source rows") != std::string::npos) {
        return;
    }

    bool changed = false;
    changed |= replaceOnce(source,
        "void PSMTXTransApply(const MtxP src, MtxP dst, f32 tx, f32 ty, f32 tz) {\n"
        "    f32 in[3][4];\n"
        "    f32 out[3][4];\n"
        "    pc_load_mtx34(src, in);\n"
        "    if (src != dst) {\n"
        "        out[0][0] = in[0][0];\n"
        "        out[0][1] = in[0][1];\n"
        "        out[0][2] = in[0][2];\n"
        "        out[1][0] = in[1][0];\n"
        "        out[1][1] = in[1][1];\n"
        "        out[1][2] = in[1][2];\n"
        "        out[2][0] = in[2][0];\n"
        "        out[2][1] = in[2][1];\n"
        "        out[2][2] = in[2][2];\n"
        "    }\n"
        "    out[0][3] = in[0][3] + tx;\n"
        "    out[1][3] = in[1][3] + ty;\n"
        "    out[2][3] = in[2][3] + tz;\n"
        "    pc_store_mtx34(dst, out);\n"
        "}\n",
        "void PSMTXTransApply(const MtxP src, MtxP dst, f32 tx, f32 ty, f32 tz) {\n"
        "    f32 in[3][4];\n"
        "    f32 out[3][4];\n"
        "    pc_load_mtx34(src, in);\n"
        "    /* PSMTXTransApply preserves aliased source rows before adding translation. */\n"
        "    for (int row = 0; row < 3; ++row) {\n"
        "        for (int col = 0; col < 4; ++col) {\n"
        "            out[row][col] = in[row][col];\n"
        "        }\n"
        "    }\n"
        "    out[0][3] = in[0][3] + tx;\n"
        "    out[1][3] = in[1][3] + ty;\n"
        "    out[2][3] = in[2][3] + tz;\n"
        "    pc_store_mtx34(dst, out);\n"
        "}\n");

    if (changed) {
        writeTextFile(mtxPath, source);
    }
}

void patchGeneratedGxSupport(const std::filesystem::path& outputDir) {
    const auto gxDir = outputDir / "gx_ems";
    patchGeneratedGxShader(gxDir / "shaders" / "default.frag");
    patchGeneratedGxInternal(gxDir / "pc_gx_internal.h");
    patchGeneratedGxC(gxDir / "pc_gx.c");
    patchGeneratedGxTextureC(gxDir / "pc_gx_texture.c");
    patchGeneratedMtxC(gxDir / "pc_mtx.c");
    patchGeneratedPadC(gxDir / "pc_pad.c");
}

void patchGeneratedHleStubs(const std::filesystem::path& stubsPath) {
    std::string source;
    if (!readTextFile(stubsPath, source)) {
        return;
    }

    bool changed = false;
    if (source.find("runtime_hle_os_load_context") == std::string::npos) {
        changed |= replaceOnce(source,
            "/*\n"
            " * Placeholder for manual runtime hooks.\n"
            " * Return 1 after handling an address to bypass the generated dispatch table.\n"
            " */\n",
            "static int runtime_hle_os_load_context(CPUContext* ctx) {\n"
            "    const u32 contextAddr = ctx != NULL ? ctx->gpr[3] : 0u;\n"
            "    const u32 srr0 = contextAddr != 0u ? MEM_READ32(contextAddr + 0x198u) : 0u;\n"
            "    const u32 srr1 = contextAddr != 0u ? MEM_READ32(contextAddr + 0x19Cu) : 0u;\n"
            "    static int bad_context_budget = 64;\n"
            "\n"
            "    if (ctx == NULL || contextAddr == 0u || runtime_can_dispatch_addr(srr0)) {\n"
            "        return 0;\n"
            "    }\n"
            "\n"
            "    if (bad_context_budget > 0) {\n"
            "        fprintf(stderr,\n"
            "                \"[HLE/OSCTX] skipped invalid OSLoadContext context=0x%08X srr0=0x%08X srr1=0x%08X lr=0x%08X pc=0x%08X state=0x%04X pri=%u ready=0x%08X current=0x%08X\\n\",\n"
            "                contextAddr,\n"
            "                srr0,\n"
            "                srr1,\n"
            "                ctx->lr,\n"
            "                ctx->pc,\n"
            "                MEM_READ16(contextAddr + 0x2C8u),\n"
            "                MEM_READ32(contextAddr + 0x2D0u),\n"
            "                ctx->gpr[13] != 0u ? MEM_READ32(ctx->gpr[13] + 0x21D0u) : 0u,\n"
            "                MEM_READ32(0x800000E4u));\n"
            "        --bad_context_budget;\n"
            "    }\n"
            "\n"
            "    return 1;\n"
            "}\n"
            "\n"
            "/*\n"
            " * Placeholder for manual runtime hooks.\n"
            " * Return 1 after handling an address to bypass the generated dispatch table.\n"
            " */\n");
    }

    if (source.find("addr == 0x802971B4u && runtime_hle_os_load_context") == std::string::npos) {
        changed |= replaceOnce(source,
            "int try_hle_stub(CPUContext* ctx, u32 addr) {\n",
            "int try_hle_stub(CPUContext* ctx, u32 addr) {\n"
            "    if (addr == 0x802971B4u && runtime_hle_os_load_context(ctx)) {\n"
            "        return 1;\n"
            "    }\n"
            "\n");
    }

    if (source.find("g_gcrecomp_last_pos_mtx_guest_addr = ctx->gpr[3]") == std::string::npos) {
        changed |= replaceOnce(source,
            "void GXLoadTexMtxImm(const void* mtx, u32 id, u32 type);\n"
            "void GXSetCurrentMtx(u32 id);\n",
            "void GXLoadTexMtxImm(const void* mtx, u32 id, u32 type);\n"
            "extern u32 g_gcrecomp_last_pos_mtx_guest_addr;\n"
            "void GXSetCurrentMtx(u32 id);\n");
        changed |= replaceOnce(source,
            "    GXLoadPosMtxImm(&g_memory[translated], ctx->gpr[4]);\n",
            "    g_gcrecomp_last_pos_mtx_guest_addr = ctx->gpr[3];\n"
            "    GXLoadPosMtxImm(&g_memory[translated], ctx->gpr[4]);\n");
    }

    if (changed) {
        writeTextFile(stubsPath, source);
    }
}

} // namespace

Analyzer::Analyzer(const Binary &binary) : m_binary(binary) {}

bool Analyzer::registerFunction(u32 addr, const std::string& name) {
    if (!m_binary.isExecutable(addr)) {
        return false;
    }

    const bool discovered = m_discoveredFunctions.insert(addr).second;
    if (Function* existing = m_cfg.getFunction(addr)) {
        if ((existing->name.rfind("fn_0x", 0) == 0 ||
             existing->name.rfind("entry_0x", 0) == 0 ||
             existing->name.rfind("sub_0x", 0) == 0) &&
            existing->name != name) {
            existing->name = name;
        }
        return discovered;
    }

    Function function;
    function.startAddr = addr;
    function.name = name;
    m_cfg.addFunction(function);
    return true;
}

void Analyzer::analyze(u32 entryPoint) {
  std::queue<u32> functionWorkList;
  size_t dataReferencedFunctions = 0;
  size_t textReferencedFunctions = 0;
  
  auto hexStr = [](u32 val) {
      std::stringstream ss;
      ss << std::hex << std::setw(8) << std::setfill('0') << val;
      return ss.str();
  };

  auto discover = [&](u32 addr, const std::string& name) {
      return registerFunction(addr, name);
  };

  // 1. Scan for prologues across all text sections
  for (const auto& sec : m_binary.getSections()) {
      if (sec.isText && sec.size > 0) {
          LOG_INFO("Scanning text section 0x%08X (size 0x%X) for prologues...", sec.address, sec.size);
          for (u32 addr = sec.address; addr < sec.address + sec.size; addr += 4) {
              u32 raw;
              if (m_binary.read32(addr, raw)) {
                  // stwu r1, -xx(r1) -> 0x9421XXXX
                  if ((raw >> 16) == 0x9421 || isLinkSavePrelude(m_binary, addr)) {
                      discover(addr, "fn_0x" + hexStr(addr));
                  }
              }
          }
          // Also discover the very start of the section just in case
          discover(sec.address, "entry_0x" + hexStr(sec.address));
      }
  }

  // 2. Discover entry point
  discover(entryPoint, "main_entry");

  // 3. Scan data for text pointers that look like callable entrypoints.
  // This helps recover callback/vtable stubs that never get reached via direct branches.
  for (const auto& sec : m_binary.getSections()) {
      if (sec.isText || sec.size < 4) {
          continue;
      }

      for (u32 addr = sec.address; addr + 3 < sec.address + sec.size; addr += 4) {
          u32 target = 0;
          if (!m_binary.read32(addr, target)) {
              continue;
          }

          target = normalizeExecutableReference(target);
          if (!looksLikeDataReferencedEntry(m_binary, m_disasm, target)) {
              continue;
          }

          if (discover(target, "sub_0x" + hexStr(target))) {
              ++dataReferencedFunctions;
          }
      }
  }

  // 4. Scan text for materialized executable addresses that behave like code pointers.
  for (const auto& sec : m_binary.getSections()) {
      if (!sec.isText || sec.size < 8) {
          continue;
      }

      const u32 limit = sec.address + sec.size - 4;
      for (u32 addr = sec.address; addr < limit; addr += 4) {
          u32 target = 0;
          if (!decodeAbsoluteAddressLoad(m_binary, addr, target)) {
              continue;
          }

          target = normalizeExecutableReference(target);
          if (!looksLikeDataReferencedEntry(m_binary, m_disasm, target)) {
              continue;
          }

          if (discover(target, "sub_0x" + hexStr(target))) {
              ++textReferencedFunctions;
          }
      }
  }

  // 5. Populate worklist
  for (u32 addr : m_discoveredFunctions) {
      functionWorkList.push(addr);
  }

  LOG_INFO("Seeded analysis with %zu discovered functions (%zu from data references, %zu from text references)",
           m_discoveredFunctions.size(),
           dataReferencedFunctions,
           textReferencedFunctions);

  while (!functionWorkList.empty()) {
      u32 addr = functionWorkList.front();
      functionWorkList.pop();

      if (m_analyzedFunctions.count(addr)) continue;
      m_analyzedFunctions.insert(addr);

      analyzeFunction(addr);

      // Check for newly discovered functions from analyzeFunction/analyzeBlock calls
      for (u32 discovered : m_discoveredFunctions) {
          if (m_analyzedFunctions.find(discovered) == m_analyzedFunctions.end()) {
              functionWorkList.push(discovered);
          }
      }
  }
}

void Analyzer::analyzeFunction(u32 entryAddr) {
    Function* func = m_cfg.getFunction(entryAddr);
    if (!func) return;

    std::set<u32> seeds;
    seeds.insert(entryAddr);
    
    // Pass 1: Identify all branch targets and block starts within reachable code
    std::queue<u32> scanQueue;
    scanQueue.push(entryAddr);
    std::set<u32> scanned;

    while (!scanQueue.empty()) {
        u32 addr = scanQueue.front();
        scanQueue.pop();

        if (scanned.count(addr)) continue;
        scanned.insert(addr);

        u32 current = addr;
        bool blockEnded = false;
        while (!blockEnded) {
            Instruction instr;
            if (!m_disasm.disassemble(m_binary, current, instr)) break;

            if (instr.isBranch) {
                const bool hasDirectTarget = instr.branchTarget != 0 &&
                    instr.branchRegisterTarget == BranchRegisterTarget::None;

                if (hasDirectTarget && instr.type != InstructionType::Call) {
                    if (m_binary.isExecutable(instr.branchTarget)) {
                        if (seeds.find(instr.branchTarget) == seeds.end()) {
                            seeds.insert(instr.branchTarget);
                            scanQueue.push(instr.branchTarget);
                        }
                    }
                }
                
                if (instr.type == InstructionType::Return) {
                    blockEnded = true;
                } else if (instr.type == InstructionType::Call) {
                    if (hasDirectTarget) {
                        if (looksLikeIndependentFunctionEntry(m_binary, m_disasm, instr.branchTarget)) {
                            std::stringstream ss;
                            ss << std::hex << std::setw(8) << std::setfill('0') << instr.branchTarget;
                            registerFunction(instr.branchTarget, "sub_0x" + ss.str());
                        } else if (m_binary.isExecutable(instr.branchTarget) &&
                                   seeds.find(instr.branchTarget) == seeds.end()) {
                            seeds.insert(instr.branchTarget);
                            scanQueue.push(instr.branchTarget);
                        }
                    }
                    current += 4;
                    // Seed the return point
                    if (seeds.find(current) == seeds.end()) {
                        seeds.insert(current);
                        scanQueue.push(current);
                    }
                    blockEnded = true;
                } else if (instr.type == InstructionType::Branch) {
                    blockEnded = true;
                } else {
                    // Conditional branch: both target and next are seeds
                    if (seeds.find(current + 4) == seeds.end()) {
                        seeds.insert(current + 4);
                        scanQueue.push(current + 4);
                    }
                    blockEnded = true;
                }
            } else {
                current += 4;
                if (current != entryAddr && m_cfg.getFunction(current) != nullptr) {
                    if (seeds.insert(current).second) {
                        scanQueue.push(current);
                    }
                    blockEnded = true;
                } else if (seeds.count(current)) {
                    blockEnded = true;
                }
            }
        }
    }

    // Pass 2: Build the blocks from discovered seeds, allowing new block starts
    // to be discovered while analyzing jump tables and intra-function branches.
    std::set<u32> pendingBlocks = seeds;
    for (u32 seed : seeds) {
        func->blocks.insert(seed);
    }

    while (!pendingBlocks.empty()) {
        const u32 seed = *pendingBlocks.begin();
        pendingBlocks.erase(pendingBlocks.begin());
        analyzeBlock(seed, *func, pendingBlocks);
    }
}

void Analyzer::analyzeBlock(u32 startAddr, Function& currentFunc, std::set<u32>& pendingBlocks) {
  if (m_cfg.getBlock(startAddr) != nullptr || !m_binary.isExecutable(startAddr)) {
      return;
  }

  BasicBlock block;
  block.startAddr = startAddr;
  u32 currentAddr = startAddr;

  LOG_DEBUG("Analyzing block at 0x%08X", startAddr);

    while (true) {
    Instruction instr;
    if (!m_disasm.disassemble(m_binary, currentAddr, instr)) {
      block.type = BlockType::Invalid;
      break;
    }

    block.instructions.push_back(instr);
    m_visitedBlocks.insert(currentAddr);

    if (instr.isBranch) {
      const bool hasExecutableDirectTarget =
          instr.branchTarget != 0 &&
          instr.branchRegisterTarget == BranchRegisterTarget::None &&
          m_binary.isExecutable(instr.branchTarget);

      if (instr.type == InstructionType::Return) {
        block.type = BlockType::Return;
      } else if (instr.type == InstructionType::Call) {
        block.type = BlockType::Call;
        u32 next = currentAddr + 4;
        block.successors.insert(next);
        if (currentFunc.blocks.insert(next).second) {
            pendingBlocks.insert(next);
        }

        if (hasExecutableDirectTarget) {
          if (looksLikeIndependentFunctionEntry(m_binary, m_disasm, instr.branchTarget)) {
              std::stringstream ss;
              ss << std::hex << std::setw(8) << std::setfill('0') << instr.branchTarget;
              registerFunction(instr.branchTarget, "sub_0x" + ss.str());
          } else {
              block.successors.insert(instr.branchTarget);
              if (currentFunc.blocks.insert(instr.branchTarget).second) {
                  pendingBlocks.insert(instr.branchTarget);
              }
          }
        }
      } else if (instr.type == InstructionType::Branch) {
        if (hasExecutableDirectTarget) {
          block.successors.insert(instr.branchTarget);
          if (currentFunc.blocks.insert(instr.branchTarget).second) {
              pendingBlocks.insert(instr.branchTarget);
          }
        }
      } else if (instr.type == InstructionType::ConditionalBranch) {
        if (hasExecutableDirectTarget) {
          block.successors.insert(instr.branchTarget);
          if (currentFunc.blocks.insert(instr.branchTarget).second) {
              pendingBlocks.insert(instr.branchTarget);
          }
        }
        block.successors.insert(currentAddr + 4);
        if (currentFunc.blocks.insert(currentAddr + 4).second) {
            pendingBlocks.insert(currentAddr + 4);
        }
      }
      block.endAddr = currentAddr;
      break;
    }

    currentAddr += 4;
    // Termination condition: hit another seed or already visited block
    if (currentAddr != startAddr &&
        (currentFunc.blocks.count(currentAddr) || m_cfg.getFunction(currentAddr) != nullptr)) {
        block.endAddr = currentAddr - 4;
        currentFunc.blocks.insert(currentAddr);
        block.successors.insert(currentAddr);
        break;
    }
  }

  block.endAddr = currentAddr;
  block.isAnalyzed = true;

  if (auto jumpTable = detectLocalJumpTable(m_binary, block)) {
      block.localJumpTable = *jumpTable;

      auto queueLocalTarget = [&](u32 target) {
          if (target == 0 || !m_binary.isExecutable(target) || !isLikelyLocalJumpTarget(block.startAddr, target)) {
              return;
          }
          if (m_cfg.getFunction(target) != nullptr || m_discoveredFunctions.count(target) != 0) {
              return;
          }
          if (currentFunc.blocks.insert(target).second) {
              pendingBlocks.insert(target);
          }
      };

      queueLocalTarget(jumpTable->defaultTarget);
      for (u32 target : jumpTable->targets) {
          queueLocalTarget(target);
          if (target != 0) {
              block.successors.insert(target);
          }
      }
      if (jumpTable->defaultTarget != 0) {
          block.successors.insert(jumpTable->defaultTarget);
      }
  }

  IRBlock ir = m_lifter.liftBlock(block);
  m_optimizer.optimizeBlock(ir);
  block.irInstructions = ir.instructions;

  m_cfg.addBlock(block);
}

void Analyzer::emitAllFunctions(const std::string& outputDir) {
    std::filesystem::create_directories(outputDir);

    for (const auto& entry : std::filesystem::directory_iterator(outputDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const auto filename = entry.path().filename().string();
        if (filename.rfind("batch_", 0) == 0 && entry.path().extension() == ".c") {
            std::filesystem::remove(entry.path());
        }
    }

    Emitter emitter;

    std::ofstream runtimeHeader(outputDir + "/recomp_runtime.h");
    if (runtimeHeader.is_open()) {
        runtimeHeader << "#ifndef OUTPUT_RECOMP_RUNTIME_H\n";
        runtimeHeader << "#define OUTPUT_RECOMP_RUNTIME_H\n\n";
        runtimeHeader << "/*\n";
        runtimeHeader << " * Reuse the maintained host runtime helpers from the main project.\n";
        runtimeHeader << " * The generated C batches use `g_memory`, while the shared header uses `ram`,\n";
        runtimeHeader << " * so alias the symbol before including it.\n";
        runtimeHeader << " */\n";
        runtimeHeader << "#include <stdio.h>\n";
        runtimeHeader << "#include <stdlib.h>\n\n";
        runtimeHeader << "#define ram g_memory\n";
        runtimeHeader << "#include \"../include/recomp_runtime.h\"\n";
        runtimeHeader << "#undef ram\n\n";
        runtimeHeader << "#define MEM_MASK GC_RAM_MASK\n\n";
        runtimeHeader << "extern void call_by_addr(CPUContext* ctx, u32 addr);\n\n";
        runtimeHeader << "void runtime_tracef(const char* fmt, ...);\n\n";
        runtimeHeader << "void runtime_dump_recent_guest_calls(FILE* out);\n";
        runtimeHeader << "int runtime_can_dispatch_addr(u32 addr);\n";
        runtimeHeader << "int runtime_dispatch_interrupt_safe_callbacks(CPUContext* ctx);\n";
        runtimeHeader << "int runtime_idle_host_services(CPUContext* ctx);\n";
        runtimeHeader << "void runtime_begin_gx_display_list(u32 guest_addr, u32 size);\n";
        runtimeHeader << "u32 runtime_end_gx_display_list(void);\n";
        runtimeHeader << "void runtime_register_request_guest_path(u32 req_addr, u32 guest_path_addr);\n";
        runtimeHeader << "const char* runtime_debug_request_host_path(u32 req_addr);\n";
        runtimeHeader << "u32 runtime_host_query_request_size(u32 req_addr);\n";
        runtimeHeader << "int runtime_host_read_request(u32 req_addr, u32 offset, u32 dst_addr, u32 length);\n\n";
        runtimeHeader << "#define PPC_FILL_FPR(ctx_, idx_, value_) do { \\\n";
        runtimeHeader << "    (ctx_)->fpr[(idx_)] = (f64)(value_); \\\n";
        runtimeHeader << "    (ctx_)->ps0[(idx_)] = (f64)(value_); \\\n";
        runtimeHeader << "    (ctx_)->ps1[(idx_)] = (f64)(value_); \\\n";
        runtimeHeader << "} while (0)\n\n";
        runtimeHeader << "static inline void fn_indirect(CPUContext* ctx, u32 addr) {\n";
        runtimeHeader << "    call_by_addr(ctx, addr);\n";
        runtimeHeader << "}\n\n";
        runtimeHeader << "#endif\n";
    }
    
    // First, generate functions.h
    std::string fhPath = outputDir + "/functions.h";
    std::ofstream fh(fhPath);
    if (fh.is_open()) {
        fh << "#ifndef FUNCTIONS_H\n#define FUNCTIONS_H\n\n";
        fh << "#include \"recomp_runtime.h\"\n\n";
        for (const auto& [addr, func] : m_cfg.getFunctions()) {
            fh << "extern void fn_0x" << std::hex << std::setw(8) << std::setfill('0') << addr << "(CPUContext* ctx);\n";
        }
        fh << "\n#endif\n";
        fh.close();
    }

    // Now emit each function in batches of 100
    int funcIdx = 0;
    int batchIdx = 0;
    std::ofstream out;
    
    for (const auto& [addr, func] : m_cfg.getFunctions()) {
        if (funcIdx % 100 == 0) {
            if (out.is_open()) out.close();
            std::string fileName = outputDir + "/batch_" + std::to_string(batchIdx++) + ".c";
            out.open(fileName);
            if (out.is_open()) {
                out << "#include \"recomp_runtime.h\"\n";
                out << "#include \"functions.h\"\n\n";
            }
        }
        
        if (out.is_open()) {
            out << "// Function: " << func.name << " at 0x" << std::hex << addr << "\n";
            out << emitter.emitFunction(func, m_cfg) << "\n\n";
        }
        funcIdx++;
    }
    if (out.is_open()) out.close();

    std::map<u32, u32> dispatchTargets;
    for (const auto& [addr, func] : m_cfg.getFunctions()) {
        for (u32 blockAddr : func.blocks) {
            dispatchTargets.emplace(blockAddr, func.startAddr);
        }
    }
    for (const auto& [addr, func] : m_cfg.getFunctions()) {
        dispatchTargets[func.startAddr] = func.startAddr;
    }

    // Emit jump_table.c
    std::ofstream jt(outputDir + "/jump_table.c");
    if (jt.is_open()) {
        jt << "#include \"recomp_runtime.h\"\n";
        jt << "#include \"functions.h\"\n\n";
        jt << "#include <stdio.h>\n";
        jt << "#include <stdlib.h>\n\n";
        jt << "typedef struct DispatchEntry {\n";
        jt << "    uint32_t addr;\n";
        jt << "    uint32_t function;\n";
        jt << "} DispatchEntry;\n\n";
        jt << "int try_hle_stub(CPUContext* ctx, uint32_t addr);\n\n";
        jt << "static int g_trace_initialized = 0;\n";
        jt << "static int g_trace_enabled = 0;\n";
        jt << "static uint32_t g_trace_limit = 0;\n";
        jt << "static uint32_t g_trace_count = 0;\n";
        jt << "static uint32_t g_trace_depth = 0;\n";
        jt << "typedef struct RecentGuestCall {\n";
        jt << "    uint32_t addr;\n";
        jt << "    uint32_t lr;\n";
        jt << "    uint32_t ctr;\n";
        jt << "    uint32_t pc;\n";
        jt << "    uint32_t depth;\n";
        jt << "} RecentGuestCall;\n";
        jt << "#define RECENT_GUEST_CALL_CAPACITY 64u\n";
        jt << "static RecentGuestCall g_recent_guest_calls[RECENT_GUEST_CALL_CAPACITY];\n";
        jt << "static uint32_t g_recent_guest_call_cursor = 0;\n";
        jt << "static uint32_t g_recent_guest_call_count = 0;\n\n";
        jt << "static void init_call_trace(void) {\n";
        jt << "    const char* envValue;\n\n";
        jt << "    if (g_trace_initialized) {\n";
        jt << "        return;\n";
        jt << "    }\n\n";
        jt << "    g_trace_initialized = 1;\n";
        jt << "    envValue = getenv(\"GCRECOMP_TRACE_CALLS\");\n";
        jt << "    if (envValue == NULL || *envValue == '\\0') {\n";
        jt << "        return;\n";
        jt << "    }\n\n";
        jt << "    g_trace_limit = (uint32_t)strtoul(envValue, NULL, 0);\n";
        jt << "    if (g_trace_limit == 0) {\n";
        jt << "        g_trace_limit = 256;\n";
        jt << "    }\n\n";
        jt << "    g_trace_enabled = 1;\n";
        jt << "    fprintf(stderr, \"[TRACE] Enabled call trace for %u calls.\\n\", g_trace_limit);\n";
        jt << "}\n\n";
        jt << "static void record_recent_guest_call(const CPUContext* ctx, uint32_t addr) {\n";
        jt << "    RecentGuestCall* slot = &g_recent_guest_calls[g_recent_guest_call_cursor % RECENT_GUEST_CALL_CAPACITY];\n";
        jt << "    slot->addr = addr;\n";
        jt << "    slot->lr = ctx != NULL ? ctx->lr : 0u;\n";
        jt << "    slot->ctr = ctx != NULL ? ctx->ctr : 0u;\n";
        jt << "    slot->pc = ctx != NULL ? ctx->pc : 0u;\n";
        jt << "    slot->depth = g_trace_depth;\n";
        jt << "    g_recent_guest_call_cursor = (g_recent_guest_call_cursor + 1u) % RECENT_GUEST_CALL_CAPACITY;\n";
        jt << "    if (g_recent_guest_call_count < RECENT_GUEST_CALL_CAPACITY) {\n";
        jt << "        ++g_recent_guest_call_count;\n";
        jt << "    }\n";
        jt << "}\n\n";
        jt << "void runtime_dump_recent_guest_calls(FILE* out) {\n";
        jt << "    uint32_t count;\n";
        jt << "    uint32_t start;\n";
        jt << "    uint32_t i;\n\n";
        jt << "    if (out == NULL) {\n";
        jt << "        return;\n";
        jt << "    }\n\n";
        jt << "    count = g_recent_guest_call_count;\n";
        jt << "    fprintf(out, \"[TRACE] Recent guest calls (%u stored, total=%u, depth=%u)\\n\", count, g_trace_count, g_trace_depth);\n";
        jt << "    if (count == 0u) {\n";
        jt << "        return;\n";
        jt << "    }\n\n";
        jt << "    start = (g_recent_guest_call_cursor + RECENT_GUEST_CALL_CAPACITY - count) % RECENT_GUEST_CALL_CAPACITY;\n";
        jt << "    for (i = 0; i < count; ++i) {\n";
        jt << "        const RecentGuestCall* slot = &g_recent_guest_calls[(start + i) % RECENT_GUEST_CALL_CAPACITY];\n";
        jt << "        fprintf(out, \"[TRACE] recent[%u] depth=%u addr=0x%08X lr=0x%08X ctr=0x%08X pc=0x%08X\\n\", i, slot->depth, slot->addr, slot->lr, slot->ctr, slot->pc);\n";
        jt << "    }\n";
        jt << "}\n\n";
        jt << "static uint32_t normalize_dispatch_addr(uint32_t addr) {\n";
        jt << "    if (addr < 0x01800000u) {\n";
        jt << "        return addr | 0x80000000u;\n";
        jt << "    }\n";
        jt << "    if (addr >= 0xC0000000u && addr < 0xC1800000u) {\n";
        jt << "        return (addr & 0x1FFFFFFFu) | 0x80000000u;\n";
        jt << "    }\n";
        jt << "    return addr;\n";
        jt << "}\n\n";
        jt << "static const DispatchEntry g_dispatch_entries[] = {\n";
        for (const auto& [blockAddr, functionAddr] : dispatchTargets) {
            jt << "    { 0x" << std::hex << std::setw(8) << std::setfill('0') << blockAddr
               << ", 0x" << std::hex << std::setw(8) << std::setfill('0') << functionAddr << " },\n";
        }
        jt << "};\n\n";
        jt << "static uint32_t resolve_dispatch_function(uint32_t addr) {\n";
        jt << "    int low = 0;\n";
        jt << "    int high = (int)(sizeof(g_dispatch_entries) / sizeof(g_dispatch_entries[0])) - 1;\n\n";
        jt << "    while (low <= high) {\n";
        jt << "        const int mid = low + ((high - low) / 2);\n";
        jt << "        const uint32_t midAddr = g_dispatch_entries[mid].addr;\n";
        jt << "        if (midAddr == addr) {\n";
        jt << "            return g_dispatch_entries[mid].function;\n";
        jt << "        }\n";
        jt << "        if (midAddr < addr) {\n";
        jt << "            low = mid + 1;\n";
        jt << "        } else {\n";
        jt << "            high = mid - 1;\n";
        jt << "        }\n";
        jt << "    }\n\n";
        jt << "    return 0;\n";
        jt << "}\n\n";
        jt << "int runtime_can_dispatch_addr(uint32_t addr) {\n";
        jt << "    addr = normalize_dispatch_addr(addr);\n";
        jt << "    return resolve_dispatch_function(addr) != 0;\n";
        jt << "}\n\n";
        jt << "static int runtime_is_disp_callback_site(const CPUContext* ctx) {\n";
        jt << "    return ctx != NULL && ctx->pc == 0x80010B10u && ctx->lr == 0x80010B28u;\n";
        jt << "}\n\n";
        jt << "void call_by_addr(CPUContext* ctx, uint32_t addr) {\n";
        jt << "    uint32_t functionAddr;\n";
        jt << "    addr = normalize_dispatch_addr(addr);\n";
        jt << "    init_call_trace();\n";
        jt << "    record_recent_guest_call(ctx, addr);\n";
        jt << "    if (g_trace_enabled && g_trace_count < g_trace_limit) {\n";
        jt << "        fprintf(stderr, \"[TRACE] #%u depth=%u addr=0x%08X lr=0x%08X ctr=0x%08X pc=0x%08X\\n\", g_trace_count, g_trace_depth, addr, ctx->lr, ctx->ctr, ctx->pc);\n";
        jt << "    }\n";
        jt << "    g_trace_count++;\n";
        jt << "    g_trace_depth++;\n";
        jt << "    if (try_hle_stub(ctx, addr)) {\n";
        jt << "        if (g_trace_depth > 0) {\n";
        jt << "            g_trace_depth--;\n";
        jt << "        }\n";
        jt << "        return;\n";
        jt << "    }\n";
        jt << "    functionAddr = resolve_dispatch_function(addr);\n";
        jt << "    if (functionAddr == 0) {\n";
        jt << "        if (runtime_is_disp_callback_site(ctx)) {\n";
        jt << "            static int disp_bad_callback_budget = 64;\n";
        jt << "            if (disp_bad_callback_budget > 0) {\n";
        jt << "                fprintf(stderr, \"[HLE/DISP] Skipped invalid display callback: 0x%08X (lr=0x%08X ctr=0x%08X pc=0x%08X)\\n\", addr, ctx->lr, ctx->ctr, ctx->pc);\n";
        jt << "                disp_bad_callback_budget--;\n";
        jt << "            }\n";
        jt << "            if (g_trace_depth > 0) {\n";
        jt << "                g_trace_depth--;\n";
        jt << "            }\n";
        jt << "            return;\n";
        jt << "        }\n";
        jt << "        fprintf(stderr, \"[RUNTIME] Unknown function call: 0x%08X (lr=0x%08X ctr=0x%08X pc=0x%08X)\\n\", addr, ctx->lr, ctx->ctr, ctx->pc);\n";
        jt << "        if (g_trace_depth > 0) {\n";
        jt << "            g_trace_depth--;\n";
        jt << "        }\n";
        jt << "        return;\n";
        jt << "    }\n";
        jt << "    ctx->pc = addr;\n";
        jt << "    switch (functionAddr) {\n";
        for (const auto& [addr, func] : m_cfg.getFunctions()) {
            jt << "        case 0x" << std::hex << addr << ": fn_0x" << std::hex << std::setw(8) << std::setfill('0') << addr << "(ctx); break;\n";
        }
        jt << "        default: fprintf(stderr, \"[RUNTIME] Missing function entry for 0x%08X (resolved from 0x%08X, lr=0x%08X ctr=0x%08X pc=0x%08X)\\n\", functionAddr, addr, ctx->lr, ctx->ctr, ctx->pc); break;\n";
        jt << "    }\n";
        jt << "    if (g_trace_depth > 0) {\n";
        jt << "        g_trace_depth--;\n";
        jt << "    }\n";
        jt << "}\n";
    }

    const std::string stubsPath = outputDir + "/hle_stubs.c";
    std::ifstream existingStubs(stubsPath);
    if (!existingStubs.good()) {
        std::ofstream stubs(stubsPath);
        if (stubs.is_open()) {
            stubs << "#include \"recomp_runtime.h\"\n\n";
            stubs << "#include \"functions.h\"\n\n";
            stubs << "/*\n";
            stubs << " * Placeholder for manual runtime hooks.\n";
            stubs << " * Return 1 after handling an address to bypass the generated dispatch table.\n";
            stubs << " */\n";
            stubs << "int try_hle_stub(CPUContext* ctx, u32 addr) {\n";
            stubs << "    (void)ctx;\n";
            stubs << "    (void)addr;\n";
            stubs << "    return 0;\n";
            stubs << "}\n";
        }
    }

    patchGeneratedHleStubs(stubsPath);
    patchGeneratedGxSupport(outputDir);
}

} // namespace gcrecomp
