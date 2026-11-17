// Windows: APPDATA and LOCALAPPDATA, else their defaults under USERPROFILE. Only environment
// variables, like macOS and Linux, so a test or a portable setup can move the folders. The cache is
// <LocalAppData>\<app>\cache.

#include <array>

#include "paths_rules.h"

namespace dvv::paths::detail {

const DirRule& userDataRule() noexcept {
    static constexpr std::array<DirSource, 2> SOURCES{{
        {"APPDATA", ""},
        {"USERPROFILE", "AppData\\Roaming"},
    }};
    static constexpr DirRule RULE{SOURCES, ""};
    return RULE;
}

const DirRule& userCacheRule() noexcept {
    static constexpr std::array<DirSource, 2> SOURCES{{
        {"LOCALAPPDATA", ""},
        {"USERPROFILE", "AppData\\Local"},
    }};
    static constexpr DirRule RULE{SOURCES, "cache"};
    return RULE;
}

}  // namespace dvv::paths::detail
