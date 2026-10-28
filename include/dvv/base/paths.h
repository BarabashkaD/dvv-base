#pragma once

// Per-user directories and the executable location, discovered at run time (spec 10.4). Nothing
// machine-specific is ever compiled in.
//   Windows: data = FOLDERID_RoamingAppData\<app>, cache = FOLDERID_LocalAppData\<app>\cache
//   Linux:   data = $XDG_DATA_HOME/<app> or ~/.local/share/<app>,
//            cache = $XDG_CACHE_HOME/<app> or ~/.cache/<app>

#include <filesystem>
#include <string_view>

#include "dvv/base/error.h"

namespace dvv::paths {

[[nodiscard]] Result<std::filesystem::path> userDataDir(std::string_view app);
[[nodiscard]] Result<std::filesystem::path> userCacheDir(std::string_view app);
[[nodiscard]] Result<std::filesystem::path> executablePath();

// Creates the directory and its parents if needed.
[[nodiscard]] Result<void> ensureDirectory(const std::filesystem::path& directory);

}  // namespace dvv::paths
