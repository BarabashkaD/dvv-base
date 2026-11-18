#include <doctest/doctest.h>

#include <chrono>

#include "dvv/base/local_time.h"

using dvv::localTime;
using std::chrono::system_clock;

TEST_CASE("localTime converts the epoch") {
    const auto r = localTime(system_clock::from_time_t(0));
    CHECK(r.has_value());
    if (r) {
        CHECK((r->tm_year == 69 || r->tm_year == 70));
    }
}

TEST_CASE("localTime of extreme times does not crash") {
    // Any result is fine; the case pins erratum E3 (no signed overflow) under the UBSan presets.
    const auto low = localTime(system_clock::time_point::min());
    const auto high = localTime(system_clock::time_point::max());
    CHECK((low.has_value() || !low.has_value()));
    CHECK((high.has_value() || !high.has_value()));
}

TEST_CASE("localTime before 1970 is nullopt on Windows") {
    const auto r = localTime(system_clock::from_time_t(-86400));
#if defined(_WIN32)
    CHECK_FALSE(r.has_value());
#else
    CHECK(r.has_value());
#endif
}
