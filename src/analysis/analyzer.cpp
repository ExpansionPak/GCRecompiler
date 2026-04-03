#include <gcrecomp/analysis/analyzer.h>
#include <gcrecomp/codegen/emitter.h>
#include <gcrecomp/log.h>
#include <fstream>
#include <filesystem>
#include <set>
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
        runtimeHeader << "int runtime_dispatch_interrupt_safe_callbacks(CPUContext* ctx);\n";
        runtimeHeader << "int runtime_idle_host_services(CPUContext* ctx);\n";
        runtimeHeader << "void runtime_begin_gx_display_list(u32 guest_addr, u32 size);\n";
        runtimeHeader << "u32 runtime_end_gx_display_list(void);\n";
        runtimeHeader << "void runtime_register_request_guest_path(u32 req_addr, u32 guest_path_addr);\n";
        runtimeHeader << "const char* runtime_debug_request_host_path(u32 req_addr);\n";
        runtimeHeader << "u32 runtime_host_query_request_size(u32 req_addr);\n";
        runtimeHeader << "int runtime_host_read_request(u32 req_addr, u32 offset, u32 dst_addr, u32 length);\n\n";
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
}

} // namespace gcrecomp
