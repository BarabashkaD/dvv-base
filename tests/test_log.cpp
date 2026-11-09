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

// Returns the number NN of the first "line NN" in the text, or -1.
int firstLineNumber(const std::string& text) {
    const auto pos = text.find("line ");
    return pos == std::string::npos ? -1 : std::stoi(text.substr(pos + 5, 2));
}

int lastLineNumber(const std::string& text) {
    const auto pos = text.rfind("line ");
    return pos == std::string::npos ? -1 : std::stoi(text.substr(pos + 5, 2));
}

// Size in bytes (with newline) of the "line NN" records the rotation tests write. The tests rely on
// the sizes below, so they assert it instead of assuming it.
std::uintmax_t lineBytes() {
    dvv::Logger::instance().clearSinks();
    auto sink = std::make_shared<dvv::MemorySink>();
    dvv::Logger::instance().addSink(sink);
    DVV_LOG_INFO("line {:02}", 0);
    dvv::Logger::instance().clearSinks();
    return sink->lines().at(0).size() + 1;
}

// A 100-byte limit must hold one line but not two (rotation tests), and 3 lines must exceed it
// by the back-off margin; both follow from 51 <= bytes <= 99.
constexpr std::uintmax_t MIN_LINE_BYTES = 51;
constexpr std::uintmax_t MAX_LINE_BYTES = 99;

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

TEST_CASE("rotating file sink keeps at most max_files old files") {
    const TempDir temp("dvv-base-log-test");
    LoggerScope scope;
    REQUIRE(lineBytes() >= MIN_LINE_BYTES);
    REQUIRE(lineBytes() <= MAX_LINE_BYTES);
    const auto& dir = temp.path();
    const auto path = dir / "test.log";
    auto sink = dvv::RotatingFileSink::open(path, 100, 2).value();
    dvv::Logger::instance().addSink(sink);
    for (int i = 0; i < 20; ++i) {
        DVV_LOG_INFO("line {:02}", i);
    }
    dvv::Logger::instance().flush();
    dvv::Logger::instance().clearSinks();
    sink.reset();
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(std::filesystem::exists(dir / "test.log.1"));
    REQUIRE(std::filesystem::exists(dir / "test.log.2"));
    CHECK_FALSE(std::filesystem::exists(dir / "test.log.3"));
    for (const auto& file : {path, dir / "test.log.1", dir / "test.log.2"}) {
        CHECK(std::filesystem::file_size(file) <= 100);
    }

    // The newest line is in test.log, older ones in .1 and .2, the oldest were dropped.
    // Each file holds exactly one line (see the line size check at the top of this test).
    const auto current = readFile(path);
    const auto first = readFile(dir / "test.log.1");
    const auto second = readFile(dir / "test.log.2");
    CHECK(firstLineNumber(current) == 19);
    CHECK(lastLineNumber(current) == 19);
    CHECK(firstLineNumber(first) == 18);
    CHECK(lastLineNumber(first) == 18);
    CHECK(firstLineNumber(second) == 17);
    CHECK(lastLineNumber(second) == 17);
}

TEST_CASE("a blocked rotation keeps every line and is retried after max_bytes more") {
    const TempDir temp("dvv-base-log-blocked");
    LoggerScope scope;
    const auto& dir = temp.path();
    const auto path = dir / "test.log";
    auto sink = dvv::RotatingFileSink::open(path, 100, 2).value();
    dvv::Logger::instance().addSink(sink);
    // A directory in the way of the staging file makes every rotation fail.
    const auto blocker = dir / "test.log.rotating";
    REQUIRE(std::filesystem::create_directory(blocker));
    for (int i = 0; i < 10; ++i) {
        DVV_LOG_INFO("line {:02}", i);
    }
    dvv::Logger::instance().flush();
    CHECK_FALSE(std::filesystem::exists(dir / "test.log.1"));
    const auto text = readFile(path);
    CHECK(firstLineNumber(text) == 0);
    CHECK(lastLineNumber(text) == 9);
    std::size_t count = 0;
    for (auto pos = text.find("line "); pos != std::string::npos;
         pos = text.find("line ", pos + 1)) {
        ++count;
    }
    CHECK(count == 10);

    REQUIRE(std::filesystem::remove(blocker));
    for (int i = 10; i < 20; ++i) {
        DVV_LOG_INFO("line {:02}", i);
    }
    dvv::Logger::instance().flush();
    CHECK(std::filesystem::exists(dir / "test.log.1"));
    CHECK(lastLineNumber(readFile(path)) == 19);
}

TEST_CASE("a failed first shift deletes no backup and keeps every line") {
    const TempDir temp("dvv-base-log-shift");
    LoggerScope scope;
    REQUIRE(lineBytes() >= MIN_LINE_BYTES);
    REQUIRE(lineBytes() <= MAX_LINE_BYTES);
    const auto& dir = temp.path();
    const auto path = dir / "test.log";
    std::filesystem::create_directories(dir);
    auto put = [](const std::filesystem::path& file, const std::string& text) {
        std::ofstream out(file, std::ios::binary);
        out << text;
    };
    // .3 is a non-empty directory, so the first shift (.2 -> .3) fails on every platform.
    put(dir / "test.log.2", "older backup\n");
    put(dir / "test.log.1", "newest backup\n");
    const auto blocker = dir / "test.log.3";
    REQUIRE(std::filesystem::create_directory(blocker));
    put(blocker / "keep", "inside\n");

    auto sink = dvv::RotatingFileSink::open(path, 100, 3).value();
    dvv::Logger::instance().addSink(sink);
    for (int i = 0; i < 12; ++i) {
        DVV_LOG_INFO("line {:02}", i);
    }
    dvv::Logger::instance().flush();
    dvv::Logger::instance().clearSinks();
    sink.reset();

    CHECK(readFile(dir / "test.log.2") == "older backup\n");
    CHECK(readFile(dir / "test.log.1") == "newest backup\n");
    CHECK(readFile(blocker / "keep") == "inside\n");
    CHECK_FALSE(std::filesystem::exists(dir / "test.log.rotating"));
    const auto text = readFile(path);
    CHECK(firstLineNumber(text) == 0);
    CHECK(lastLineNumber(text) == 11);
    std::size_t count = 0;
    for (auto pos = text.find("line "); pos != std::string::npos;
         pos = text.find("line ", pos + 1)) {
        ++count;
    }
    CHECK(count == 12);
}

TEST_CASE("after a cancelled rotation the next attempt waits for max_bytes more") {
    const TempDir temp("dvv-base-log-backoff");
    LoggerScope scope;
    REQUIRE(lineBytes() >= MIN_LINE_BYTES);
    REQUIRE(lineBytes() <= MAX_LINE_BYTES);
    const auto& dir = temp.path();
    const auto path = dir / "test.log";
    auto sink = dvv::RotatingFileSink::open(path, 100, 2).value();
    dvv::Logger::instance().addSink(sink);
    const auto blocker = dir / "test.log.rotating";
    REQUIRE(std::filesystem::create_directory(blocker));
    // Line 1 triggers the failing attempt (wait until size + 100); line 0 and 1 stay in the file.
    DVV_LOG_INFO("line {:02}", 0);
    DVV_LOG_INFO("line {:02}", 1);
    dvv::Logger::instance().flush();
    CHECK_FALSE(std::filesystem::exists(dir / "test.log.1"));

    REQUIRE(std::filesystem::remove(blocker));
    // Less than max_bytes (one line) more: still no rotation.
    DVV_LOG_INFO("line {:02}", 2);
    dvv::Logger::instance().flush();
    CHECK_FALSE(std::filesystem::exists(dir / "test.log.1"));
    // Past the back-off threshold: the rotation happens now.
    DVV_LOG_INFO("line {:02}", 3);
    dvv::Logger::instance().flush();
    CHECK(std::filesystem::exists(dir / "test.log.1"));
    CHECK(firstLineNumber(readFile(path)) == 3);
    CHECK(lastLineNumber(readFile(dir / "test.log.1")) == 2);
    dvv::Logger::instance().clearSinks();
}

TEST_CASE("an existing staging file is never modified or deleted") {
    const TempDir temp("dvv-base-log-stale");
    LoggerScope scope;
    const auto& dir = temp.path();
    const auto path = dir / "test.log";
    std::filesystem::create_directories(dir);
    const auto stale = dir / "test.log.rotating";
    const std::string content = "stale data that must survive\n";
    {
        std::ofstream out(stale, std::ios::binary);
        out << content;
    }
    auto sink = dvv::RotatingFileSink::open(path, 100, 2).value();
    dvv::Logger::instance().addSink(sink);
    for (int i = 0; i < 20; ++i) {
        DVV_LOG_INFO("line {:02}", i);
    }
    dvv::Logger::instance().flush();
    CHECK(readFile(stale) == content);
    CHECK_FALSE(std::filesystem::exists(dir / "test.log.1"));
    CHECK(firstLineNumber(readFile(path)) == 0);
    CHECK(lastLineNumber(readFile(path)) == 19);
}

TEST_CASE("rotating file sink rejects bad limits") {
    const TempDir temp("dvv-base-log-bad");
    CHECK_FALSE(dvv::RotatingFileSink::open(temp.path() / "x.log", 0, 1).has_value());
    CHECK_FALSE(dvv::RotatingFileSink::open(temp.path() / "x.log", 10, 0).has_value());
}

TEST_CASE("a warning is on disk without calling flush") {
    const TempDir temp("dvv-base-log-warn");
    LoggerScope scope;
    const auto path = temp.path() / "warn.log";
    auto sink = dvv::RotatingFileSink::open(path, 100000, 1).value();
    dvv::Logger::instance().addSink(sink);
    DVV_LOG_INFO("buffered");
    DVV_LOG_WARN("urgent");
    // The sink is still open and flush() was never called.
    const auto text = readFile(path);
    CHECK(text.find("WARN  urgent") != std::string::npos);
    dvv::Logger::instance().clearSinks();
}

TEST_CASE("the leak check flushes buffered lines to disk") {
    const TempDir temp("dvv-base-log-leak-check");
    LoggerScope scope;
    const auto path = temp.path() / "exit.log";
    auto sink = dvv::RotatingFileSink::open(path, 100000, 1).value();
    dvv::Logger::instance().addSink(sink);
    DVV_LOG_INFO("last words");
    CHECK(dvv::finishWithLeakCheck(0, dvv::LeakPolicy::Warn) == 0);
    // The sink is still open; only the leak check flushed it.
    CHECK(readFile(path).find("INFO  last words") != std::string::npos);
    dvv::Logger::instance().clearSinks();
}

TEST_CASE("reopening an existing log appends and accounts its size") {
    const TempDir temp("dvv-base-log-reopen");
    LoggerScope scope;
    const auto& dir = temp.path();
    const auto path = dir / "again.log";
    // Two lines fit into 150 bytes and a third forces a rotation.
    REQUIRE(lineBytes() * 2 <= 150);
    REQUIRE(lineBytes() * 3 > 150);
    {
        auto sink = dvv::RotatingFileSink::open(path, 150, 1).value();
        dvv::Logger::instance().addSink(sink);
        DVV_LOG_INFO("line {:02}", 1);
        dvv::Logger::instance().clearSinks();
    }
    {
        auto sink = dvv::RotatingFileSink::open(path, 150, 1).value();
        dvv::Logger::instance().addSink(sink);
        DVV_LOG_INFO("line {:02}", 2);
        CHECK_FALSE(std::filesystem::exists(dir / "again.log.1"));
        DVV_LOG_INFO("line {:02}", 3);
        dvv::Logger::instance().clearSinks();
    }
    const auto old_text = readFile(dir / "again.log.1");
    CHECK(firstLineNumber(old_text) == 1);
    CHECK(lastLineNumber(old_text) == 2);
    CHECK(firstLineNumber(readFile(path)) == 3);
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
    dvv::Logger::instance().clearSinks();
    CHECK(true);
}
