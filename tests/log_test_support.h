#pragma once

// Shared helpers of the logger tests.

#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/base/log.h"

namespace dvv_test {

// An enum: formatted by its std::formatter, not a deferrable log argument type.
enum class Colour { Red };

// Stops the log thread and removes the sinks around an asynchronous test, so a failed check never
// leaves the log thread running.
struct AsyncScope {
    AsyncScope() {
        dvv::Logger::instance().stop();
        dvv::Logger::instance().clearSinks();
    }
    ~AsyncScope() {
        dvv::Logger::instance().stop();
        dvv::Logger::instance().clearSinks();
        dvv::Logger::instance().setLevel(dvv::LogLevel::Info);
    }
    AsyncScope(const AsyncScope&) = delete;
    AsyncScope& operator=(const AsyncScope&) = delete;
    AsyncScope(AsyncScope&&) = delete;
    AsyncScope& operator=(AsyncScope&&) = delete;
};

// Keeps the lines and publishes their count in batchEnd(), so a test can wait for the log thread
// without flush(). Add it after any other sink: its batchEnd() then runs after theirs.
class WaitingSink final : public dvv::ILogSink {
public:
    void write(const dvv::LogRecord& /*record*/, std::string_view line) override {
        const std::lock_guard lock(mutex_);
        lines_.emplace_back(line);
    }

    void batchEnd() override {
        std::uint32_t count = 0;
        {
            const std::lock_guard lock(mutex_);
            count = static_cast<std::uint32_t>(lines_.size());
        }
        handled.store(count);
        handled.notify_all();
    }

    // Returns once at least `count` lines were handled and their pass has ended.
    void waitFor(std::uint32_t count) {
        for (std::uint32_t seen = handled.load(); seen < count; seen = handled.load()) {
            handled.wait(seen);
        }
    }

    [[nodiscard]] std::vector<std::string> lines() const {
        const std::lock_guard lock(mutex_);
        return lines_;
    }

    std::atomic<std::uint32_t> handled{0};

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

}  // namespace dvv_test

template <>
struct std::formatter<dvv_test::Colour> : std::formatter<std::string_view> {
    auto format(dvv_test::Colour, std::format_context& context) const {
        return std::formatter<std::string_view>::format("red", context);
    }
};
