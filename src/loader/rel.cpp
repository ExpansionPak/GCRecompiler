#include <gcrecomp/loader/rel.h>
#include <gcrecomp/common.h>
#include <gcrecomp/log.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace gcrecomp {
namespace {

constexpr u8 R_PPC_ADDR32 = 1;
constexpr u8 R_PPC_ADDR24 = 2;
constexpr u8 R_PPC_ADDR16 = 3;
constexpr u8 R_PPC_ADDR16_LO = 4;
constexpr u8 R_PPC_ADDR16_HI = 5;
constexpr u8 R_PPC_ADDR16_HA = 6;
constexpr u8 R_PPC_ADDR14 = 7;
constexpr u8 R_PPC_ADDR14_BRTAKEN = 8;
constexpr u8 R_PPC_ADDR14_BRNTAKEN = 9;
constexpr u8 R_PPC_REL24 = 10;
constexpr u8 R_PPC_REL14 = 11;
constexpr u8 R_PPC_REL14_BRTAKEN = 12;
constexpr u8 R_PPC_REL14_BRNTAKEN = 13;
constexpr u8 R_DOLPHIN_NOP = 201;
constexpr u8 R_DOLPHIN_SECTION = 202;
constexpr u8 R_DOLPHIN_END = 203;

struct RelHeader {
    u32 id = 0;
    u32 numSections = 0;
    u32 sectionInfoOffset = 0;
    u32 bssSize = 0;
    u32 relOffset = 0;
    u32 impOffset = 0;
    u32 impSize = 0;
    u32 align = 0;
    u32 bssAlign = 0;
};

struct RelSection {
    u32 index = 0;
    u32 address = 0;
    u32 fileOffset = 0;
    u32 size = 0;
    bool executable = false;
    bool present = false;
    std::vector<u8> data;
};

u32 alignUp(u32 value, u32 align) {
    if (align <= 1u) {
        return value;
    }
    return (value + align - 1u) & ~(align - 1u);
}

bool checkedRange(const std::vector<u8>& data, u32 offset, u32 size) {
    return offset <= data.size() && size <= data.size() - offset;
}

bool readFile(const std::string& path, std::vector<u8>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open REL file: %s", path.c_str());
        return false;
    }

    const std::streamsize size = file.tellg();
    if (size < 0) {
        LOG_ERROR("Failed to determine REL file size: %s", path.c_str());
        return false;
    }

    file.seekg(0, std::ios::beg);
    data.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        LOG_ERROR("Failed to read REL file data: %s", path.c_str());
        return false;
    }

    return true;
}

bool parseHeader(const std::vector<u8>& data, RelHeader& header) {
    if (data.size() < 0x4Cu) {
        LOG_ERROR("REL file too small to contain a module header");
        return false;
    }

    header.id = read_be32(data.data() + 0x00);
    header.numSections = read_be32(data.data() + 0x0C);
    header.sectionInfoOffset = read_be32(data.data() + 0x10);
    header.bssSize = read_be32(data.data() + 0x20);
    header.relOffset = read_be32(data.data() + 0x24);
    header.impOffset = read_be32(data.data() + 0x28);
    header.impSize = read_be32(data.data() + 0x2C);
    header.align = read_be32(data.data() + 0x40);
    header.bssAlign = read_be32(data.data() + 0x44);

    if (header.numSections == 0 ||
        header.numSections > 256 ||
        !checkedRange(data, header.sectionInfoOffset, header.numSections * 8u) ||
        !checkedRange(data, header.impOffset, header.impSize) ||
        (header.impSize % 8u) != 0u) {
        LOG_ERROR("REL header has invalid section/import bounds");
        return false;
    }

    return true;
}

bool addressToSectionOffset(std::vector<RelSection>& sections,
                            u32 address,
                            RelSection*& sectionOut,
                            u32& offsetOut) {
    for (RelSection& section : sections) {
        if (!section.present || section.size == 0) {
            continue;
        }
        if (address >= section.address && address < section.address + section.size) {
            sectionOut = &section;
            offsetOut = address - section.address;
            return true;
        }
    }

    return false;
}

bool readRelocated32(std::vector<RelSection>& sections, u32 address, u32& value) {
    RelSection* section = nullptr;
    u32 offset = 0;
    if (!addressToSectionOffset(sections, address, section, offset) ||
        offset > section->data.size() ||
        section->data.size() - offset < 4u) {
        return false;
    }

    value = read_be32(section->data.data() + offset);
    return true;
}

bool writeRelocated16(std::vector<RelSection>& sections, u32 address, u16 value) {
    RelSection* section = nullptr;
    u32 offset = 0;
    if (!addressToSectionOffset(sections, address, section, offset) ||
        offset > section->data.size() ||
        section->data.size() - offset < 2u) {
        return false;
    }

    section->data[offset + 0] = static_cast<u8>(value >> 8);
    section->data[offset + 1] = static_cast<u8>(value);
    return true;
}

bool writeRelocated32(std::vector<RelSection>& sections, u32 address, u32 value) {
    RelSection* section = nullptr;
    u32 offset = 0;
    if (!addressToSectionOffset(sections, address, section, offset) ||
        offset > section->data.size() ||
        section->data.size() - offset < 4u) {
        return false;
    }

    section->data[offset + 0] = static_cast<u8>(value >> 24);
    section->data[offset + 1] = static_cast<u8>(value >> 16);
    section->data[offset + 2] = static_cast<u8>(value >> 8);
    section->data[offset + 3] = static_cast<u8>(value);
    return true;
}

u32 sectionAddress(const std::vector<RelSection>& sections, u8 index) {
    if (index >= sections.size() || !sections[index].present) {
        return 0;
    }
    return sections[index].address;
}

bool applyRelocation(std::vector<RelSection>& sections,
                     u32 patchAddress,
                     u32 targetBase,
                     u8 type,
                     u32 addend) {
    u32 word = 0;
    u32 value = 0;

    switch (type) {
    case 0:
        return true;
    case R_PPC_ADDR32:
        return writeRelocated32(sections, patchAddress, targetBase + addend);
    case R_PPC_ADDR24:
        if (!readRelocated32(sections, patchAddress, word)) {
            return false;
        }
        value = targetBase + addend;
        return writeRelocated32(sections, patchAddress, (word & ~0x03FFFFFCu) | (value & 0x03FFFFFCu));
    case R_PPC_ADDR16:
    case R_PPC_ADDR16_LO:
        value = targetBase + addend;
        return writeRelocated16(sections, patchAddress, static_cast<u16>(value));
    case R_PPC_ADDR16_HI:
        value = targetBase + addend;
        return writeRelocated16(sections, patchAddress, static_cast<u16>(value >> 16));
    case R_PPC_ADDR16_HA:
        value = targetBase + addend;
        return writeRelocated16(sections, patchAddress,
                                static_cast<u16>((value >> 16) + ((value & 0x8000u) ? 1u : 0u)));
    case R_PPC_ADDR14:
    case R_PPC_ADDR14_BRTAKEN:
    case R_PPC_ADDR14_BRNTAKEN:
        if (!readRelocated32(sections, patchAddress, word)) {
            return false;
        }
        value = targetBase + addend;
        return writeRelocated32(sections, patchAddress, (word & ~0x0000FFFCu) | (value & 0x0000FFFCu));
    case R_PPC_REL24:
        if (!readRelocated32(sections, patchAddress, word)) {
            return false;
        }
        value = targetBase + addend - patchAddress;
        return writeRelocated32(sections, patchAddress, (word & ~0x03FFFFFCu) | (value & 0x03FFFFFCu));
    case R_PPC_REL14:
    case R_PPC_REL14_BRTAKEN:
    case R_PPC_REL14_BRNTAKEN:
        if (!readRelocated32(sections, patchAddress, word)) {
            return false;
        }
        value = targetBase + addend - patchAddress;
        return writeRelocated32(sections, patchAddress, (word & ~0x0000FFFCu) | (value & 0x0000FFFCu));
    case R_DOLPHIN_NOP:
        return true;
    default:
        return false;
    }
}

} // namespace

bool loadRelModuleIntoBinary(Binary& binary,
                             const std::string& path,
                             const RelLoadOptions& options) {
    std::vector<u8> fileData;
    RelHeader header;
    std::vector<RelSection> sections;
    u32 moduleBase = options.moduleBase;
    u32 bssCursor = 0;
    u32 appliedRelocations = 0;
    u32 skippedRelocations = 0;

    if (!readFile(path, fileData) || !parseHeader(fileData, header)) {
        return false;
    }

    if (moduleBase == 0 && options.textSectionBase.has_value()) {
        for (u32 i = 0; i < header.numSections; ++i) {
            const u32 entryOffset = header.sectionInfoOffset + (i * 8u);
            const u32 rawOffset = read_be32(fileData.data() + entryOffset);
            const u32 size = read_be32(fileData.data() + entryOffset + 4u);
            if ((rawOffset & 1u) != 0u && size != 0u) {
                moduleBase = *options.textSectionBase - (rawOffset & ~1u);
                break;
            }
        }
    }

    if (moduleBase == 0) {
        LOG_ERROR("REL module base must be non-zero: %s", path.c_str());
        return false;
    }

    sections.resize(header.numSections);
    bssCursor = options.bssBase.value_or(
        alignUp(moduleBase + static_cast<u32>(fileData.size()),
                std::max(header.bssAlign, 1u)));

    for (u32 i = 0; i < header.numSections; ++i) {
        const u32 entryOffset = header.sectionInfoOffset + (i * 8u);
        const u32 rawOffset = read_be32(fileData.data() + entryOffset);
        const u32 size = read_be32(fileData.data() + entryOffset + 4u);
        RelSection section;

        section.index = i;
        section.size = size;
        section.executable = (rawOffset & 1u) != 0u;

        if (rawOffset != 0u) {
            const u32 cleanOffset = rawOffset & ~1u;
            if (!checkedRange(fileData, cleanOffset, size)) {
                LOG_ERROR("REL section %u exceeds file bounds in %s", i, path.c_str());
                return false;
            }

            section.fileOffset = cleanOffset;
            section.address = moduleBase + cleanOffset;
            section.present = size != 0u;
            section.data.assign(fileData.begin() + cleanOffset,
                                fileData.begin() + cleanOffset + size);
        } else if (size != 0u) {
            const u32 align = std::max(header.bssAlign, 1u);
            section.fileOffset = 0;
            section.address = alignUp(bssCursor, align);
            section.present = true;
            section.data.resize(size, 0);
            bssCursor = section.address + size;
        }

        sections[i] = std::move(section);
    }

    for (u32 impOffset = header.impOffset;
         impOffset < header.impOffset + header.impSize;
         impOffset += 8u) {
        const u32 importId = read_be32(fileData.data() + impOffset);
        const u32 relListOffset = read_be32(fileData.data() + impOffset + 4u);
        const bool supportedImport = importId == 0u || importId == header.id;
        u32 patchAddress = 0;

        if (!checkedRange(fileData, relListOffset, 8u)) {
            LOG_ERROR("REL import relocation list exceeds file bounds in %s", path.c_str());
            return false;
        }

        for (u32 relOffset = relListOffset; checkedRange(fileData, relOffset, 8u); relOffset += 8u) {
            const u16 offsetDelta = read_be16(fileData.data() + relOffset);
            const u8 type = fileData[relOffset + 2u];
            const u8 sectionIndex = fileData[relOffset + 3u];
            const u32 addend = read_be32(fileData.data() + relOffset + 4u);

            patchAddress += offsetDelta;

            if (type == R_DOLPHIN_END) {
                break;
            }
            if (type == R_DOLPHIN_SECTION) {
                patchAddress = sectionAddress(sections, sectionIndex);
                if (patchAddress == 0u) {
                    LOG_ERROR("REL relocation references missing section %u in %s",
                              sectionIndex,
                              path.c_str());
                    return false;
                }
                continue;
            }

            if (!supportedImport) {
                ++skippedRelocations;
                continue;
            }

            const u32 targetBase = importId == 0u ? 0u : sectionAddress(sections, sectionIndex);
            if (importId != 0u && targetBase == 0u) {
                ++skippedRelocations;
                continue;
            }

            if (applyRelocation(sections, patchAddress, targetBase, type, addend)) {
                ++appliedRelocations;
            } else {
                ++skippedRelocations;
            }
        }
    }

    for (auto& section : sections) {
        if (!section.present || section.data.empty()) {
            continue;
        }

        binary.appendMappedSection(section.address,
                                   section.fileOffset,
                                   section.executable,
                                   std::move(section.data));
    }

    const std::string moduleLabel = options.moduleName.empty() ? "" : (options.moduleName + " ");
    LOG_INFO("Loaded REL %s%s at 0x%08X (%zu sections, %u relocations, %u skipped)",
             moduleLabel.c_str(),
             path.c_str(),
             moduleBase,
             sections.size(),
             appliedRelocations,
             skippedRelocations);
    return true;
}

} // namespace gcrecomp
