#pragma once

// Where the per-user directories come from: one rule per directory, as data. The values live in one
// file per OS (paths_macos.cpp, paths_linux.cpp, paths_win32.cpp, chosen by CMake); paths.cpp
// holds the one algorithm for every OS. Private to dvv-base and its tests.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "dvv/base/environment.h"
#include "dvv/base/error.h"

namespace dvv::paths::detail {

// An environment variable (UTF-8, read with dvv::getEnvironment) and the path appended to its
// value.
struct DirSource {
    std::string_view variable;
    std::string_view relative;
};

// The sources are tried in order; a base is used only when it is a non-empty absolute path. `tail`
// is appended after the app name.
struct DirRule {
    std::span<const DirSource> sources;
    std::string_view tail;
};

// The rules of this OS: read-only data with static storage, no run-time initialisation.
[[nodiscard]] const DirRule& userDataRule() noexcept;
[[nodiscard]] const DirRule& userCacheRule() noexcept;

using EnvironmentReader = std::optional<std::string> (*)(std::string_view name);

// <base>/<relative>/<app>/<tail> of the first usable base (empty parts add nothing), else an Io
// error naming every source tried, joined by "; ". A rule without sources is an Internal error.
// `app` is not checked (the callers do); `read` is a test seam.
[[nodiscard]] Result<std::filesystem::path> resolveDir(const DirRule& rule, std::string_view app,
                                                       EnvironmentReader read = &getEnvironment);

}  // namespace dvv::paths::detail
