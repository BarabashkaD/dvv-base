#include <chrono>
#include <ctime>
#include <optional>

#include "dvv/base/local_time.h"

namespace dvv {

std::optional<std::tm> localTime(std::chrono::system_clock::time_point time) noexcept {
    // No conversion back to the clock's unit: that overflows at time_point::min().
    const auto seconds = std::chrono::floor<std::chrono::seconds>(time.time_since_epoch()).count();
    const auto t = static_cast<std::time_t>(seconds);
    std::tm local{};
    if (localtime_s(&local, &t) != 0) {
        return std::nullopt;
    }
    return local;
}

}  // namespace dvv
