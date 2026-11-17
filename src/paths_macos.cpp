// macOS: the per-user directories live under ~/Library; XDG variables are not used.

#include <array>

#include "paths_rules.h"

namespace dvv::paths::detail {

const DirRule& userDataRule() noexcept {
    static constexpr std::array<DirSource, 1> SOURCES{{
        {"HOME", "Library/Application Support"},
    }};
    static constexpr DirRule RULE{SOURCES, ""};
    return RULE;
}

const DirRule& userCacheRule() noexcept {
    static constexpr std::array<DirSource, 1> SOURCES{{
        {"HOME", "Library/Caches"},
    }};
    static constexpr DirRule RULE{SOURCES, ""};
    return RULE;
}

}  // namespace dvv::paths::detail
