#include <gcrecomp/loader/dol.h>
#include <gcrecomp/loader/rel.h>
#include <gcrecomp/analysis/analyzer.h>
#include <gcrecomp/log.h>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace gcrecomp;

namespace {

std::optional<u32> parseU32(const std::string& text) {
    try {
        size_t parsed = 0;
        const unsigned long value = std::stoul(text, &parsed, 0);
        if (parsed == text.size() && value <= 0xFFFFFFFFul) {
            return static_cast<u32>(value);
        }
    } catch (...) {
    }

    return std::nullopt;
}

std::vector<std::string> splitRelSpecs(const std::string& text) {
    std::vector<std::string> specs;
    std::string current;

    for (char c : text) {
        if (c == ',' || c == ';') {
            if (!current.empty()) {
                specs.push_back(current);
                current.clear();
            }
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            current.push_back(c);
        }
    }

    if (!current.empty()) {
        specs.push_back(current);
    }

    return specs;
}

bool loadRelSpec(DolBinary& dol,
                 const std::filesystem::path& relDir,
                 const std::string& spec,
                 u32 defaultTextBase) {
    std::string pathText = spec;
    std::optional<u32> textBase = defaultTextBase;
    const size_t baseSep = spec.rfind('@');

    if (baseSep != std::string::npos) {
        pathText = spec.substr(0, baseSep);
        textBase = parseU32(spec.substr(baseSep + 1));
        if (!textBase.has_value()) {
            LOG_ERROR("Invalid REL text base in spec: %s", spec.c_str());
            return false;
        }
    }

    std::filesystem::path relPath(pathText);
    if (!relPath.has_parent_path() && relPath.extension().empty()) {
        relPath = relDir / (pathText + ".rel");
    } else if (!relPath.has_parent_path()) {
        relPath = relDir / relPath;
    }

    if (!std::filesystem::exists(relPath)) {
        LOG_ERROR("REL file not found: %s", relPath.string().c_str());
        return false;
    }

    RelLoadOptions options;
    options.textSectionBase = textBase;
    options.moduleName = relPath.stem().string();
    return loadRelModuleIntoBinary(dol, relPath.string(), options);
}

} // namespace

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

    const std::filesystem::path dolFsPath(dolPath);
    const std::filesystem::path isoDir = dolFsPath.parent_path().parent_path();
    const std::filesystem::path relDir = isoDir / "files" / "rel";
    const char* relTextBaseEnv = std::getenv("GCRECOMP_REL_TEXT_BASE");
    const u32 defaultRelTextBase =
        (relTextBaseEnv != nullptr && relTextBaseEnv[0] != '\0')
            ? parseU32(relTextBaseEnv).value_or(0x805BAAB8u)
            : 0x805BAAB8u;
    const char* relSpecsEnv = std::getenv("GCRECOMP_RELS");
    std::vector<std::string> relSpecs;

    if (relSpecsEnv != nullptr && relSpecsEnv[0] != '\0') {
        relSpecs = splitRelSpecs(relSpecsEnv);
    } else if (std::filesystem::exists(relDir / "aaa.rel")) {
        relSpecs.push_back("aaa");
    }

    for (const std::string& relSpec : relSpecs) {
        if (!loadRelSpec(dol, relDir, relSpec, defaultRelTextBase)) {
            return 1;
        }
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
    size_t totalIR = 0;
    for (const auto& [addr, block] : cfg.getBlocks()) {
        totalIR += block.irInstructions.size();
    }

    LOG_INFO("Analysis Complete.");
    LOG_INFO("  Basic Blocks:    %zu", cfg.getBlocks().size());
    LOG_INFO("  Functions:       %zu", cfg.getFunctions().size());
    LOG_INFO("  IR Instructions: %zu", totalIR);

    std::string dotPath = "cfg.dot";
    cfg.exportDot(dotPath);
    LOG_INFO("CFG exported to %s", dotPath.c_str());

    LOG_INFO("Emitting C code to 'output'...");
    analyzer.emitAllFunctions("output");
    LOG_INFO("Code generation complete.");

    return 0;
}
