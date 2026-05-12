#pragma once

#include <gcrecomp/types.h>
#include <gcrecomp/log.h>
#include <gcrecomp/common.h>
#include <vector>
#include <string>
#include <optional>
#include <utility>

namespace gcrecomp {

class Binary {
public:
    virtual ~Binary() = default;

    virtual bool load(const std::string& path) = 0;

    virtual u32 getEntryPoint() const = 0;
    virtual const std::vector<Section>& getSections() const = 0;

    void appendMappedSection(u32 address, u32 fileOffset, bool isText, std::vector<u8> data) {
        if (data.empty()) {
            return;
        }

        m_regions.push_back({address, static_cast<u32>(data.size()), std::move(data)});
        m_sections.push_back({address, m_regions.back().size, fileOffset, isText});
        m_lastRegion = nullptr;
    }

    bool isValidAddress(u32 addr) const {
        return findRegion(addr) != nullptr;
    }

    bool isExecutable(u32 addr) const {
        for (const auto& sec : getSections()) {
            if (sec.isText && addr >= sec.address && addr < sec.address + sec.size) {
                return true;
            }
        }
        return false;
    }

    bool read8(u32 addr, u8& out) const {
        const auto* region = findRegion(addr);
        if (!region || !region->contains(addr)) {
            LOG_ERROR("Invalid read8 at 0x%08X", addr);
            return false;
        }
        out = region->data[addr - region->start];
        return true;
    }

    bool read16(u32 addr, u16& out) const {
        if (addr % 2 != 0) {
            LOG_ERROR("Unaligned read16 at 0x%08X", addr);
            return false;
        }
        const auto* region = findRegion(addr);
        if (!region || !region->contains(addr) || !region->contains(addr + 1)) {
            LOG_ERROR("Invalid or cross-region read16 at 0x%08X", addr);
            return false;
        }
        out = read_be16(&region->data[addr - region->start]);
        return true;
    }

    bool read32(u32 addr, u32& out) const {
        if (addr % 4 != 0) {
            LOG_ERROR("Unaligned read32 at 0x%08X", addr);
            return false;
        }
        const auto* region = findRegion(addr);
        if (!region || !region->contains(addr) || !region->contains(addr + 3)) {
            LOG_ERROR("Invalid or cross-region read32 at 0x%08X", addr);
            return false;
        }
        out = read_be32(&region->data[addr - region->start]);
        return true;
    }

protected:
    const MemoryRegion* findRegion(u32 addr) const {
        if (m_lastRegion && m_lastRegion->contains(addr)) {
            return m_lastRegion;
        }

        for (const auto& region : m_regions) {
            if (region.contains(addr)) {
                m_lastRegion = &region;
                return m_lastRegion;
            }
        }
        return nullptr;
    }

    std::vector<MemoryRegion> m_regions;
    mutable const MemoryRegion* m_lastRegion = nullptr;
    std::vector<Section> m_sections;
    u32 m_entryPoint = 0;
};

} // namespace gcrecomp
