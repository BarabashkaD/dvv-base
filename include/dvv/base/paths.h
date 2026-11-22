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
// Also, on every OS: the base name of a path text and the drive-letter test.

#include <cstddef>
#include <filesystem>
#include <string_view>

#include "dvv/base/ascii.h"
#include "dvv/base/error.h"

namespace dvv::paths {

[[nodiscard]] Result<std::filesystem::path> userDataDir(std::string_view app);
[[nodiscard]] Result<std::filesystem::path> userCacheDir(std::string_view app);
[[nodiscard]] Result<std::filesystem::path> executablePath();

// Creates the directory and its parents if needed.
[[nodiscard]] Result<void> ensureDirectory(const std::filesystem::path& directory);

}  // namespace dvv::paths

namespace dvv {

// The text after the last '/' or '\\' of `text`, or all of it when it has neither. Both separators
// count on every OS, for file paths and URI paths alike: "a/b\\c.yuv" -> "c.yuv", "a/" -> "".
[[nodiscard]] constexpr std::string_view baseNameOf(std::string_view text) noexcept {
    const std::size_t cut = text.find_last_of("/\\");
    return cut == std::string_view::npos ? text : text.substr(cut + 1);
}

// True when `text` starts with a Windows drive: an ASCII letter, then ':'. What may follow (a
// separator, the end) is the caller's rule.
[[nodiscard]] constexpr bool isDriveLetterPath(std::string_view text) noexcept {
    return text.size() >= 2 && isAsciiLetter(text[0]) && text[1] == ':';
}

}  // namespace dvv
