#include <gcrecomp/loader/dol.h>
#include <gcrecomp/analysis/analyzer.h>
#include <gcrecomp/log.h>
#include <iostream>
#include <iomanip>

using namespace gcrecomp;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: gcrecomp <game.dol>" << std::endl;
        return 1;
    }

    std::string dolPath = argv[1];
    DolBinary dol;

    if (!dol.load(dolPath)) {
        LOG_ERROR("Failed to load DOL: %s", dolPath.c_str());
        return 1;
    }

    LOG_INFO("Loaded DOL: %s", dolPath.c_str());
    LOG_INFO("Entry Point: 0x%08X", dol.getEntryPoint());

    std::cout << "\nMemory Regions:" << std::endl;
    for (const auto& sec : dol.getSections()) {
        std::cout << "[" << (sec.isText ? "TEXT" : "DATA") << "] "
                  << "0x" << std::hex << std::setw(8) << std::setfill('0') << sec.address
                  << " - 0x" << std::hex << std::setw(8) << std::setfill('0') << (sec.address + sec.size)
                  << " (size: 0x" << std::hex << sec.size << ")" << std::endl;
    }

    LOG_INFO("Starting Control Flow Analysis...");
    Analyzer analyzer(dol);
    analyzer.analyze(dol.getEntryPoint());

    const auto& cfg = analyzer.getCfg();
    LOG_INFO("Analysis Complete. Found %zu basic blocks.", cfg.getBlocks().size());

    std::cout << "\nFirst 10 Basic Blocks:" << std::endl;
    int count = 0;
    for (const auto& [addr, block] : cfg.getBlocks()) {
        std::cout << "Block 0x" << std::hex << std::setw(8) << std::setfill('0') << addr 
                  << " - 0x" << std::hex << std::setw(8) << std::setfill('0') << block.endAddr
                  << " (" << std::dec << block.instructions.size() << " instructions)" << std::endl;
        if (++count >= 10) break;
    }

    return 0;
}
