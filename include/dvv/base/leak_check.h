#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "dvv/base/lifetime.h"

namespace dvv {

enum class LeakPolicy : std::uint8_t { Fail, Warn };

// DVV_LEAK_CHECK=fail|warn overrides; otherwise Fail in Debug builds and Warn in Release builds.
[[nodiscard]] LeakPolicy defaultLeakPolicy();

[[nodiscard]] std::string formatLeakReport(std::span<const LiveObjectInfo> leaks);

// Call at the end of main, after the Session, FrameStore and contexts are torn down. Flushes the
// Logger, prints the live managed objects to stderr and returns the exit code to use: unchanged
// when nothing leaked or the policy is Warn, otherwise non-zero.
[[nodiscard]] int finishWithLeakCheck(int exit_code, LeakPolicy policy = defaultLeakPolicy());

}  // namespace dvv
