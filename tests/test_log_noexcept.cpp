#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <memory>
#include <new>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/base/log.h"
#include "log_ring.h"
#include "log_test_support.h"

namespace {

using dvv::LogLevel;
using dvv_test::AsyncScope;

dvv::Logger& logger() {
    return dvv::Logger::instance();
}

// Not a deferrable log argument type, so a call formats it on the caller's thread. Its formatter
// throws std::format_error.
struct BadFormat {};

// The same, but its formatter throws std::bad_alloc, as when memory runs out while formatting.
struct OutOfMemory {};

class ThrowingSink final : public dvv::ILogSink {
public:
    void write(const dvv::LogRecord& /*record*/, std::string_view /*line*/) override {
        throw std::runtime_error("sink failed");
    }
};

bool anyContains(const std::vector<std::string>& lines, std::string_view part) {
    return std::ranges::any_of(
        lines, [&](const std::string& line) { return line.find(part) != std::string::npos; });
}

}  // namespace

template <>
struct std::formatter<BadFormat> : std::formatter<std::string_view> {
    auto format(BadFormat /*value*/, std::format_context& /*context*/) const
        -> std::format_context::iterator {
        throw std::format_error("cannot format");
    }
};

template <>
struct std::formatter<OutOfMemory> : std::formatter<std::string_view> {
    auto format(OutOfMemory /*value*/, std::format_context& /*context*/) const
        -> std::format_context::iterator {
        throw std::bad_alloc();
    }
};

static_assert(!dvv::detail::LOG_ALL_DEFERRABLE<BadFormat>);
static_assert(!dvv::detail::LOG_ALL_DEFERRABLE<OutOfMemory>);

TEST_CASE("a log call never throws: a throwing formatter gives the format text") {
    SUBCASE("synchronous") {
        const AsyncScope scope;
        auto sink = std::make_shared<dvv::MemorySink>();
        logger().addSink(sink);
        DVV_LOG_INFO("sync value {} here", BadFormat{});
        CHECK(anyContains(sink->lines(), "sync value {} here"));
    }
    SUBCASE("asynchronous") {
        const AsyncScope scope;
        auto sink = std::make_shared<dvv::MemorySink>();
        logger().addSink(sink);
        logger().start();
        DVV_LOG_INFO("async value {} here", BadFormat{});
        logger().stop();
        CHECK(anyContains(sink->lines(), "async value {} here"));
    }
}

TEST_CASE(
    "a log call never throws: a run-time format error in deferred arguments gives the format "
    "text") {
    // Deferrable arguments that pass the compile-time check; the negative width throws
    // std::format_error only when the line is formatted.
    const int width = -1;
    static_assert(dvv::detail::LOG_ALL_DEFERRABLE<int, int>);
    SUBCASE("synchronous") {
        const AsyncScope scope;
        auto sink = std::make_shared<dvv::MemorySink>();
        logger().addSink(sink);
        const std::uint64_t before = dvv::detail::droppedForTest(LogLevel::Info);
        DVV_LOG_INFO("sync w {:{}} here", 1, width);
        CHECK(anyContains(sink->lines(), "sync w {:{}} here"));
        CHECK(dvv::detail::droppedForTest(LogLevel::Info) == before);
    }
    SUBCASE("asynchronous") {
        const AsyncScope scope;
        auto sink = std::make_shared<dvv::MemorySink>();
        logger().addSink(sink);
        logger().start();
        DVV_LOG_INFO("async w {:{}} here", 1, width);
        logger().stop();
        const std::vector<std::string> lines = sink->lines();
        CHECK(anyContains(lines, "async w {:{}} here"));
        CHECK_FALSE(anyContains(lines, "log messages dropped ("));
        CHECK(dvv::detail::droppedForTest(LogLevel::Info) == 0);
    }
}

TEST_CASE("a log call never throws: an allocation failure counts the line as dropped") {
    const AsyncScope scope;
    auto sink = std::make_shared<dvv::MemorySink>();
    logger().addSink(sink);
    logger().start();
    const std::uint64_t before = dvv::detail::droppedForTest(LogLevel::Info);
    DVV_LOG_INFO("lost value {} here", OutOfMemory{});
    CHECK(dvv::detail::droppedForTest(LogLevel::Info) == before + 1);
    logger().stop();
    const std::vector<std::string> lines = sink->lines();
    CHECK_FALSE(anyContains(lines, "lost value"));
    CHECK(anyContains(lines, "log messages dropped (1 info)"));
}

TEST_CASE("a log call never throws: a throwing sink in synchronous mode") {
    const AsyncScope scope;
    auto thrower = std::make_shared<ThrowingSink>();
    auto sink = std::make_shared<dvv::MemorySink>();
    logger().addSink(thrower);
    logger().addSink(sink);
    const std::uint64_t before = dvv::detail::droppedForTest(LogLevel::Info);
    DVV_LOG_INFO("after a throwing sink {}", 7);
    DVV_LOG_TEXT(LogLevel::Info, "text after a throwing sink");
    logger().mark("mark after a throwing sink");
    std::vector<std::string> lines = sink->lines();
    CHECK(anyContains(lines, "after a throwing sink 7"));
    CHECK(anyContains(lines, "text after a throwing sink"));
    CHECK(anyContains(lines, "mark after a throwing sink"));
    // Another sink took each line: nothing is counted.
    CHECK(dvv::detail::droppedForTest(LogLevel::Info) == before);

    // A line that no sink takes is counted once, however many sinks throw.
    logger().clearSinks();
    logger().addSink(thrower);
    logger().addSink(std::make_shared<ThrowingSink>());
    DVV_LOG_INFO("only throwing sinks {}", 8);
    CHECK(dvv::detail::droppedForTest(LogLevel::Info) == before + 1);

    // The synchronous mode reports the count only at the next stop() of an asynchronous run;
    // without the throwing sinks, which would fail again on the report.
    logger().clearSinks();
    logger().addSink(sink);
    logger().start();
    logger().stop();
    CHECK(dvv::detail::droppedForTest(LogLevel::Info) == 0);
    lines = sink->lines();
    CHECK(anyContains(lines, "log messages dropped ("));
}

TEST_CASE("a log call never throws: compile-time") {
    static constexpr dvv::detail::LogSite SITE{"value {}", std::source_location::current()};
    static constinit std::atomic<bool> bare_warned{false};
    // Built first: the standard does not mark the conversions from a string literal (to
    // std::string_view, the consteval one to std::format_string) noexcept, so noexcept() of a call
    // that converts a literal is false whatever the callee.
    const std::string_view message = "x";
    static constexpr std::format_string<std::string> TEXT_FORMAT{"value {}"};
    static constexpr std::format_string<int> NUMBER_FORMAT{"value {}"};
    static constexpr std::format_string<BadFormat> BAD_FORMAT{"value {}"};
    const std::string text = "text";
    const int number = 1;
    const BadFormat bad{};
    static_assert(noexcept(dvv::Logger::instance().write(LogLevel::Info, message)));
    static_assert(noexcept(dvv::Logger::instance().mark(message)));
    static_assert(
        noexcept(dvv::detail::logCall(LogLevel::Info, SITE, bare_warned, TEXT_FORMAT, text)));
    static_assert(
        noexcept(dvv::detail::logCall(LogLevel::Info, SITE, bare_warned, NUMBER_FORMAT, number)));
    static_assert(
        noexcept(dvv::detail::logCall(LogLevel::Info, SITE, bare_warned, BAD_FORMAT, bad)));
    static_assert(dvv::detail::LOG_ALL_DEFERRABLE<std::string>);
    static_assert(dvv::detail::LOG_ALL_DEFERRABLE<int>);
    CHECK_FALSE(bare_warned.load());
}
