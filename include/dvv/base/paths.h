#pragma once

// Per-user directories and the executable location, discovered at run time (spec 10.4). Nothing
// machine-specific is ever compiled in.
//   Windows: data = %APPDATA%\<app> or %USERPROFILE%\AppData\Roaming\<app>,
//            cache = %LOCALAPPDATA%\<app>\cache or %USERPROFILE%\AppData\Local\<app>\cache
//            (environment variables only; no known-folder lookup). The USERPROFILE step, used only
//            when APPDATA / LOCALAPPDATA is unset, builds AppData\Roaming | AppData\Local under
//            the profile, so it does not follow a redirected or relocated folder (Folder
//            Redirection, "User Shell Folders"); normal logons and services always set APPDATA and
//            LOCALAPPDATA.
//   Linux:   data = $XDG_DATA_HOME/<app> or ~/.local/share/<app>,
//            cache = $XDG_CACHE_HOME/<app> or ~/.cache/<app>
//   macOS:   data = ~/Library/Application Support/<app>, cache = ~/Library/Caches/<app>
//            (XDG variables are ignored)
// A variable (HOME included) is used only when it holds an absolute path; otherwise the next
// source is tried. When none gives a folder, the Io error names every source tried.
// `app` is UTF-8: a plain directory name (no '/', '\\', ':', NUL, not "." or ".."), else an
// Internal error.

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
