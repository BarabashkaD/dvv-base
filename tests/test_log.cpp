#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <source_location>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "dvv/base/leak_check.h"
#include "dvv/base/log.h"
#include "log_test_support.h"

namespace {

// Restores the global logger after each test.
struct LoggerScope {
    LoggerScope() { dvv::Logger::instance().clearSinks(); }
    ~LoggerScope() {
        dvv::Logger::instance().clearSinks();
        dvv::Logger::instance().setLevel(dvv::LogLevel::Info);
    }
    LoggerScope(const LoggerScope&) = delete;
    LoggerScope& operator=(const LoggerScope&) = delete;
};

// Unique temporary directory per instance, removed on destruction even when a check fails.
class TempDir {
public:
    explicit TempDir(const std::string& prefix) {
        std::random_device random;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                std::format("{}-{}-{:08x}", prefix, stamp, random());
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::string readFile(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("log lines carry time, level, message and file name") {
    LoggerScope scope;
    auto sink = std::make_shared<dvv::MemorySink>();
    dvv::Logger::instance().addSink(sink);
    DVV_LOG_INFO("opened {} frames", 3);
    const auto lines = sink->lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].starts_with("20"));
    CHECK(lines[0].find("Z INFO  opened 3 frames [test_log.cpp:") != std::string::npos);
}

TEST_CASE("formatLogRecord: the default is SourceSuffix::With") {
    const dvv::LogRecord record{dvv::LogLevel::Warn, "hello world", std::source_location::current(),
                                std::chrono::system_clock::now()};
    CHECK(dvv::formatLogRecord(record, dvv::SourceSuffix::With) == dvv::formatLogRecord(record));
}

TEST_CASE("formatLogRecord: SourceSuffix::Without drops the location") {
    const dvv::LogRecord record{dvv::LogLevel::Warn, "hello world", std::source_location::current(),
                                std::chrono::system_clock::now()};
    const std::string without = dvv::formatLogRecord(record, dvv::SourceSuffix::Without);
    CHECK(without.find(" [") == std::string::npos);
}

TEST_CASE("formatLogRecord: Without is the full line up to the location") {
    const dvv::LogRecord record{dvv::LogLevel::Warn, "hello world", std::source_location::current(),
                                std::chrono::system_clock::now()};
    const std::string full = dvv::formatLogRecord(record);
    const std::string without = dvv::formatLogRecord(record, dvv::SourceSuffix::Without);
    const std::size_t cut = full.rfind(" [");
    REQUIRE(cut != std::string::npos);
    CHECK(full.ends_with("]"));
    CHECK(without == full.substr(0, cut));
    CHECK(without.find("test_log.cpp") == std::string::npos);
    CHECK(without.ends_with("WARN  hello world"));
}

TEST_CASE("LogRecord::mark defaults to false") {
    const dvv::LogRecord record;
    CHECK_FALSE(record.mark);
}
TEST_CASE("messages below the level are dropped") {
    LoggerScope scope;
    auto sink = std::make_shared<dvv::MemorySink>();
    dvv::Logger::instance().addSink(sink);
    dvv::Logger::instance().setLevel(dvv::LogLevel::Warn);
    DVV_LOG_INFO("hidden");
    DVV_LOG_WARN("shown");
    const auto lines = sink->lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("WARN  shown") != std::string::npos);
}

TEST_CASE("a null sink is ignored") {
    LoggerScope scope;
    auto sink = std::make_shared<dvv::MemorySink>();
    dvv::Logger::instance().addSink(nullptr);
    dvv::Logger::instance().addSink(sink);
    DVV_LOG_INFO("still works");
    dvv::Logger::instance().flush();
    CHECK(sink->lines().size() == 1);
}

TEST_CASE("the leak check flushes buffered lines to disk") {
    const TempDir temp("dvv-base-log-leak-check");
    LoggerScope scope;
    auto sink =
        dvv::SplitFileSink::open(temp.path(), "p", 1000000, dvv::SplitOpen::NewFile).value();
    const auto path = sink->currentFile();
    dvv::Logger::instance().addSink(sink);
    DVV_LOG_INFO("last words");
    CHECK(dvv::finishWithLeakCheck(0, dvv::LeakPolicy::Warn) == 0);
    // The sink is still open; only the leak check flushed it.
    CHECK(readFile(path).find("INFO  last words") != std::string::npos);
    dvv::Logger::instance().clearSinks();
}

namespace {

// Keeps the records with their lines, to compare a line with formatLogRecord.
class RecordSink final : public dvv::ILogSink {
public:
    struct Entry {
        dvv::LogRecord record;
        std::string message;
        std::string line;
    };

    void write(const dvv::LogRecord& record, std::string_view line) override {
        entries.push_back({record, std::string(record.message), std::string(line)});
        entries.back().record.message = entries.back().message;
    }

    std::deque<Entry> entries;  // a deque keeps the message views valid
};

bool contains(const std::vector<std::string>& lines, std::string_view part) {
    return std::ranges::any_of(
        lines, [&](const std::string& line) { return line.find(part) != std::string::npos; });
}

}  // namespace

TEST_CASE("sync: a disabled level does not evaluate its arguments") {
    LoggerScope scope;
    auto sink = std::make_shared<dvv::MemorySink>();
    dvv::Logger::instance().addSink(sink, dvv::LogLevel::Info);
    int evaluated = 0;
    DVV_LOG_DEBUG("n {}", ++evaluated);
    CHECK(evaluated == 0);
    DVV_LOG_INFO("n {}", ++evaluated);
    CHECK(evaluated == 1);
}

TEST_CASE("sync: the gate is closed without sinks and follows sink levels") {
    LoggerScope scope;
    auto& logger = dvv::Logger::instance();
    logger.setLevel(dvv::LogLevel::Trace);
    CHECK_FALSE(logger.enabled(dvv::LogLevel::Error));  // no sinks
    auto sink = std::make_shared<dvv::MemorySink>();
    logger.addSink(sink, dvv::LogLevel::Warn);
    CHECK_FALSE(logger.enabled(dvv::LogLevel::Info));
    CHECK(logger.enabled(dvv::LogLevel::Warn));
    logger.setSinkLevel(*sink, dvv::LogLevel::Debug);
    CHECK(logger.enabled(dvv::LogLevel::Debug));
    CHECK(logger.level() == dvv::LogLevel::Trace);  // sink changes leave level() alone
    CHECK_FALSE(logger.enabled(dvv::LogLevel::Off));
}

TEST_CASE("sync: per-sink levels") {
    LoggerScope scope;
    auto& logger = dvv::Logger::instance();
    logger.setLevel(dvv::LogLevel::Trace);
    auto all = std::make_shared<dvv::MemorySink>();
    auto warnings = std::make_shared<dvv::MemorySink>();
    logger.addSink(all, dvv::LogLevel::Trace);
    logger.addSink(warnings, dvv::LogLevel::Warn);
    DVV_LOG_INFO("info {}", 1);
    DVV_LOG_WARN("warn {}", 2);
    CHECK(all->lines().size() == 2);
    REQUIRE(warnings->lines().size() == 1);
    CHECK(warnings->lines()[0].find("warn 2") != std::string::npos);
}

TEST_CASE("sync: a message is in the sink before the call returns") {
    LoggerScope scope;
    auto sink = std::make_shared<dvv::MemorySink>();
    dvv::Logger::instance().addSink(sink);
    constexpr int THREADS = 8;
    std::atomic<int> missing{0};
    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int id = 0; id < THREADS; ++id) {
        threads.emplace_back([&sink, &missing, id] {
            DVV_LOG_INFO("thread {} hello", id);
            if (!contains(sink->lines(), std::format("thread {} hello", id))) {
                ++missing;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(missing == 0);
    CHECK(sink->lines().size() == THREADS);
}

TEST_CASE("sync: deferred, caller-formatted and text messages print as std::format") {
    LoggerScope scope;
    auto sink = std::make_shared<RecordSink>();
    dvv::Logger::instance().addSink(sink);
    DVV_LOG_INFO("{} {:>4}", 7, "ab");
    DVV_LOG_INFO("colour {}", dvv_test::Colour::Red);
    DVV_LOG_INFO("none");
    DVV_LOG_TEXT(dvv::LogLevel::Warn, "plain");
    REQUIRE(sink->entries.size() == 4);
    CHECK(sink->entries[0].message == "7   ab");
    CHECK(sink->entries[1].message == "colour red");
    CHECK(sink->entries[2].message == "none");
    CHECK(sink->entries[3].message == "plain");
    CHECK(sink->entries[3].record.level == dvv::LogLevel::Warn);
    for (const auto& entry : sink->entries) {
        CHECK(entry.line == dvv::formatLogRecord(entry.record));
    }
}

TEST_CASE("sync: bare {} warns once per call site, DVV_LOG_TEXT never") {
    LoggerScope scope;
    auto sink = std::make_shared<dvv::MemorySink>();
    dvv::Logger::instance().addSink(sink);
    const std::string text = "some text";
    for (int i = 0; i < 3; ++i) {
        DVV_LOG_INFO("{}", text);
    }
    auto lines = sink->lines();
    CHECK(std::ranges::count_if(lines, [](const std::string& line) {
              return line.find("some text") != std::string::npos;
          }) == 3);
    CHECK(std::ranges::count_if(lines, [](const std::string& line) {
              return line.find("formats only \"{}\"") != std::string::npos &&
                     line.find("test_log.cpp") != std::string::npos &&
                     line.find("WARN") != std::string::npos;
          }) == 1);
    const auto before = lines.size();
    DVV_LOG_TEXT(dvv::LogLevel::Info, "{}");
    DVV_LOG_TEXT(dvv::LogLevel::Info, "{}");
    lines = sink->lines();
    CHECK(lines.size() == before + 2);
    CHECK_FALSE(lines.back().find("formats only") != std::string::npos);
}

TEST_CASE("sync: Logger::write keeps its run-time location") {
    LoggerScope scope;
    auto sink = std::make_shared<dvv::MemorySink>();
    dvv::Logger::instance().addSink(sink);
    const auto where = std::source_location::current();
    dvv::Logger::instance().write(dvv::LogLevel::Info, "x", where);
    REQUIRE(sink->lines().size() == 1);
    const std::string expected = std::format("[test_log.cpp:{}]", where.line());
    CHECK(sink->lines()[0].ends_with(expected));
}

TEST_CASE("mark reaches a sink at Error but not a sink at Off") {
    LoggerScope scope;
    auto errors = std::make_shared<RecordSink>();
    auto off = std::make_shared<RecordSink>();
    dvv::Logger::instance().addSink(errors, dvv::LogLevel::Error);
    dvv::Logger::instance().addSink(off, dvv::LogLevel::Off);
    const auto where = std::source_location::current();
    dvv::Logger::instance().mark("===== marker =====", where);
    REQUIRE(errors->entries.size() == 1);
    const auto& entry = errors->entries[0];
    CHECK(entry.record.mark);
    CHECK(entry.record.level == dvv::LogLevel::Info);
    CHECK(entry.message == "===== marker =====");
    CHECK(entry.record.where.line() == where.line());
    CHECK(entry.line == dvv::formatLogRecord(entry.record));
    CHECK(entry.line.find("INFO  ===== marker ===== [test_log.cpp:") != std::string::npos);
    CHECK(off->entries.empty());
}

TEST_CASE("a plain Info record does not reach a sink at Warn") {
    LoggerScope scope;
    auto warnings = std::make_shared<RecordSink>();
    auto all = std::make_shared<RecordSink>();
    dvv::Logger::instance().addSink(warnings, dvv::LogLevel::Warn);
    dvv::Logger::instance().addSink(all, dvv::LogLevel::Trace);
    DVV_LOG_INFO("plain {}", 1);
    dvv::Logger::instance().write(dvv::LogLevel::Info, "plain 2");
    dvv::Logger::instance().mark("marked");
    REQUIRE(warnings->entries.size() == 1);
    CHECK(warnings->entries[0].message == "marked");
    REQUIRE(all->entries.size() == 3);
    CHECK_FALSE(all->entries[0].record.mark);
    CHECK_FALSE(all->entries[1].record.mark);
    CHECK(all->entries[2].record.mark);
}

TEST_CASE("mark ignores the logger level") {
    LoggerScope scope;
    auto sink = std::make_shared<RecordSink>();
    dvv::Logger::instance().addSink(sink);
    dvv::Logger::instance().setLevel(dvv::LogLevel::Error);
    DVV_LOG_INFO("hidden");
    dvv::Logger::instance().mark("at Error");
    dvv::Logger::instance().setLevel(dvv::LogLevel::Off);
    dvv::Logger::instance().mark("at Off");
    REQUIRE(sink->entries.size() == 2);
    CHECK(sink->entries[0].message == "at Error");
    CHECK(sink->entries[1].message == "at Off");
    CHECK(sink->entries[1].record.mark);
}

TEST_CASE("mark keeps order with records") {
    LoggerScope scope;
    auto sink = std::make_shared<RecordSink>();
    dvv::Logger::instance().addSink(sink);
    dvv::Logger::instance().write(dvv::LogLevel::Info, "a");
    dvv::Logger::instance().mark("m");
    dvv::Logger::instance().write(dvv::LogLevel::Info, "b");
    REQUIRE(sink->entries.size() == 3);
    CHECK(sink->entries[0].message == "a");
    CHECK(sink->entries[1].message == "m");
    CHECK(sink->entries[2].message == "b");
    CHECK_FALSE(sink->entries[0].record.mark);
    CHECK(sink->entries[1].record.mark);
    CHECK_FALSE(sink->entries[2].record.mark);
}

TEST_CASE("mark with no sinks does nothing") {
    LoggerScope scope;
    dvv::Logger::instance().mark("nobody listens");
    // A sink at Off is no listener either; one added later sees nothing of the earlier mark.
    auto off = std::make_shared<RecordSink>();
    dvv::Logger::instance().addSink(off, dvv::LogLevel::Off);
    dvv::Logger::instance().mark("still nobody");
    CHECK(off->entries.empty());
    auto later = std::make_shared<RecordSink>();
    dvv::Logger::instance().addSink(later);
    CHECK(later->entries.empty());
}

TEST_CASE("system log sink accepts every level and a message with % signs") {
    LoggerScope scope;
    dvv::Logger::instance().setLevel(dvv::LogLevel::Trace);
    dvv::Logger::instance().addSink(std::make_shared<dvv::SystemLogSink>("dvv-base-test"));
    DVV_LOG_TRACE("trace {}", 1);
    DVV_LOG_DEBUG("debug {}", 2);
    DVV_LOG_INFO("info {}", 3);
    DVV_LOG_WARN("warn {}", 4);
    DVV_LOG_ERROR("error {}", 5);
    DVV_LOG_INFO("100% {}", "%s %n %d");
    dvv::Logger::instance().mark("mark 100% %s %n %d");
    dvv::Logger::instance().clearSinks();
    CHECK(true);
}
