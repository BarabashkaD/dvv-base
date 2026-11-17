// Linux (and other Unix systems): the XDG base directories, else their defaults under HOME.

#include <array>

#include "paths_rules.h"

namespace dvv::paths::detail {

const DirRule& userDataRule() noexcept {
    static constexpr std::array<DirSource, 2> SOURCES{{
        {"XDG_DATA_HOME", ""},
        {"HOME", ".local/share"},
    }};
    static constexpr DirRule RULE{SOURCES, ""};
    return RULE;
}

const DirRule& userCacheRule() noexcept {
    static constexpr std::array<DirSource, 2> SOURCES{{
        {"XDG_CACHE_HOME", ""},
        {"HOME", ".cache"},
    }};
    static constexpr DirRule RULE{SOURCES, ""};
    return RULE;
}

}  // namespace dvv::paths::detail
