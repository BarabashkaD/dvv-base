#pragma once
#include <chrono>
#include <ctime>
#include <optional>

namespace dvv {
// The local calendar time of `time`, to the second (localtime_s on Windows, localtime_r
// elsewhere); nullopt when the platform cannot convert it (outside its local-time range, e.g.
// before 1970 on Windows).
[[nodiscard]] std::optional<std::tm> localTime(std::chrono::system_clock::time_point time) noexcept;
}  // namespace dvv
