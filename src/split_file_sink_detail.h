#pragma once

// SplitFileSink internals, private to dvv-base and its tests.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dvv::detail {

// A strictly matching log file name: stamp is YYYYMMDDHHMMSS as a number, number the -N suffix
// (1 without one).
struct SplitLogName {
    std::uint64_t stamp = 0;
    std::uint32_t number = 1;
};

// Parses "<prefix>-YYYYMMDD-HHMMSS.log" or "<prefix>-YYYYMMDD-HHMMSS-N.log", where the prefix
// matches exactly (case-sensitive), the date and time are exactly 8 and 6 digits and N has 1 to 9
// digits without a leading zero. Anything else is nullopt.
[[nodiscard]] std::optional<SplitLogName> parseSplitLogName(std::string_view name,
                                                            std::string_view prefix);

// "<prefix>-YYYYMMDD-HHMMSS" of `time` in local time.
[[nodiscard]] std::string splitLogFileStem(std::string_view prefix,
                                           std::chrono::system_clock::time_point time);

// Test seam: the clock that names new files. nullptr restores std::chrono::system_clock::now.
using SplitFileClock = std::chrono::system_clock::time_point (*)() noexcept;
void setSplitFileClock(SplitFileClock clock) noexcept;

}  // namespace dvv::detail
