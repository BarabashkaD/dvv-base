#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <system_error>

#include "dvv/base/leak_check.h"
#include "dvv/base/log.h"

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
