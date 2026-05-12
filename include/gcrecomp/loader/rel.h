#pragma once

#include <gcrecomp/loader/binary.h>
#include <gcrecomp/types.h>

#include <optional>
#include <string>

namespace gcrecomp {

struct RelLoadOptions {
    u32 moduleBase = 0;
    std::optional<u32> textSectionBase;
    std::optional<u32> bssBase;
    std::string moduleName;
};

bool loadRelModuleIntoBinary(Binary& binary,
                             const std::string& path,
                             const RelLoadOptions& options);

} // namespace gcrecomp
