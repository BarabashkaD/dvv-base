#pragma once

// SplitFileSink internals, private to dvv-base and its tests.

#include <chrono>
#include <string>
#include <string_view>

namespace dvv::detail {

// "<prefix>-YYYYMMDD-HHMMSS" of `time` in local time.
[[nodiscard]] std::string splitLogFileStem(std::string_view prefix,
                                           std::chrono::system_clock::time_point time);

// Test seam: the clock that names new files. nullptr restores std::chrono::system_clock::now.
using SplitFileClock = std::chrono::system_clock::time_point (*)() noexcept;
void setSplitFileClock(SplitFileClock clock) noexcept;

}  // namespace dvv::detail
