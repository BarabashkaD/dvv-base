#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "dvv/base/log.h"
#include "split_file_sink_detail.h"

#if !defined(_WIN32)
#include <csignal>

#include <sys/resource.h>
#include <unistd.h>
#endif

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
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, error);
        std::filesystem::remove_all(path_, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Pins the clock that names new files; restores the system clock on destruction.
struct FixedClock {
    // 2026-03-04 05:06:07 UTC; its local name depends on the time zone, so tests ask the sink's
    // own stem function for it.
    static std::chrono::system_clock::time_point now() noexcept {
        return std::chrono::system_clock::time_point{std::chrono::seconds{1772600767}};
    }
    FixedClock() { dvv::detail::setSplitFileClock(&FixedClock::now); }
    ~FixedClock() { dvv::detail::setSplitFileClock(nullptr); }
    FixedClock(const FixedClock&) = delete;
    FixedClock& operator=(const FixedClock&) = delete;
};

std::string readFile(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void writeFile(const std::filesystem::path& file, const std::string& content) {
    std::ofstream stream(file, std::ios::binary);
    stream << content;
}

std::string utf8Name(const std::filesystem::path& file) {
    const std::u8string name = file.filename().u8string();
    std::string text;
    std::ranges::transform(name, std::back_inserter(text),
                           [](char8_t unit) { return static_cast<char>(unit); });
    return text;
}

// The names of the regular files in `folder`, sorted.
std::vector<std::string> fileNames(const std::filesystem::path& folder) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.is_regular_file()) {
            names.push_back(utf8Name(entry.path()));
        }
    }
    std::ranges::sort(names);
    return names;
}

// A line of exactly `bytes` bytes with its newline: "<tag>" padded with '.'.
std::string lineOf(std::size_t bytes, const std::string& tag) {
    std::string line = tag;
    line.resize(bytes - 1, '.');
    return line;
}

void writeLine(dvv::SplitFileSink& sink, const std::string& line,
               dvv::LogLevel level = dvv::LogLevel::Info) {
    dvv::LogRecord record;
    record.level = level;
    record.message = line;
    record.time = std::chrono::system_clock::now();
    sink.write(record, line);
}

// Splits `text` into its lines (without the newlines).
std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (auto end = text.find('\n'); end != std::string::npos; end = text.find('\n', start)) {
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    if (start < text.size()) {
        lines.push_back(text.substr(start));
    }
    return lines;
}

}  // namespace

TEST_CASE("split sink: names a new file by local creation time") {
    const TempDir temp("dvv-base-split-name");
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, true);
        REQUIRE(sink.has_value());
        const auto names = fileNames(temp.path());
        REQUIRE(names.size() == 1);
        CHECK(std::regex_match(names[0], std::regex(R"(^p-\d{8}-\d{6}\.log$)")));
        CHECK((*sink)->currentFile() == temp.path() / names[0]);
        CHECK((*sink)->splits() == 0);
    }
}

TEST_CASE("split sink: the name is the local time of the clock") {
    const TempDir temp("dvv-base-split-clock");
    const FixedClock clock;
    const std::string stem = dvv::detail::splitLogFileStem("p", FixedClock::now());
    CHECK(std::regex_match(stem, std::regex(R"(^p-\d{8}-\d{6}$)")));
    auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, false).value();
    CHECK(utf8Name(sink->currentFile()) == stem + ".log");
}

TEST_CASE("split sink: exclusive creation skips a taken name") {
    const TempDir temp("dvv-base-split-exclusive");
    const FixedClock clock;
    const std::string stem = dvv::detail::splitLogFileStem("p", FixedClock::now());
    writeFile(temp.path() / (stem + ".log"), "taken 1\n");
    writeFile(temp.path() / (stem + "-2.log"), "taken 2\n");
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, false).value();
        CHECK(utf8Name(sink->currentFile()) == stem + "-3.log");
        writeLine(*sink, "mine");
    }
    CHECK(readFile(temp.path() / (stem + ".log")) == "taken 1\n");
    CHECK(readFile(temp.path() / (stem + "-2.log")) == "taken 2\n");
    CHECK(readFile(temp.path() / (stem + "-3.log")) == "mine\n");
}

TEST_CASE("split sink: a clash in the same second gets the next suffix") {
    const TempDir temp("dvv-base-split-clash");
    const FixedClock clock;
    const std::string stem = dvv::detail::splitLogFileStem("p", FixedClock::now());
    auto first = dvv::SplitFileSink::open(temp.path(), "p", 1000, false).value();
    auto second = dvv::SplitFileSink::open(temp.path(), "p", 1000, false).value();
    CHECK(utf8Name(first->currentFile()) == stem + ".log");
    CHECK(utf8Name(second->currentFile()) == stem + "-2.log");
}

TEST_CASE("split sink: appends to the newest file under the limit") {
    const TempDir temp("dvv-base-split-append");
    writeFile(temp.path() / "p-20260101-000000.log", "012345678\n");
    writeFile(temp.path() / "p-20260102-000000.log", "abcdefghi\n");
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, true).value();
        CHECK(sink->currentFile() == temp.path() / "p-20260102-000000.log");
        writeLine(*sink, "appended");
    }
    CHECK(fileNames(temp.path()).size() == 2);
    CHECK(readFile(temp.path() / "p-20260101-000000.log") == "012345678\n");
    CHECK(readFile(temp.path() / "p-20260102-000000.log") == "abcdefghi\nappended\n");
}

TEST_CASE("split sink: a newest file at the limit is not appended to") {
    const TempDir temp("dvv-base-split-full");
    writeFile(temp.path() / "p-20260102-000000.log", "abcdefghi\n");
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 10, true).value();
        CHECK(sink->currentFile() != temp.path() / "p-20260102-000000.log");
        writeLine(*sink, "new");
    }
    CHECK(fileNames(temp.path()).size() == 2);
    CHECK(readFile(temp.path() / "p-20260102-000000.log") == "abcdefghi\n");
}

TEST_CASE("split sink: newest order with suffixes") {
    const TempDir temp("dvv-base-split-order");
    writeFile(temp.path() / "p-20260101-000000.log", "a\n");
    writeFile(temp.path() / "p-20260101-000000-2.log", "b\n");
    writeFile(temp.path() / "p-20260101-000000-10.log", "c\n");
    writeFile(temp.path() / "p-20251231-235959-99.log", "d\n");
    auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, true).value();
    CHECK(sink->currentFile() == temp.path() / "p-20260101-000000-10.log");
}

TEST_CASE("split sink: no suffix counts as 1, below -2") {
    const TempDir temp("dvv-base-split-order2");
    writeFile(temp.path() / "p-20260101-000000-2.log", "b\n");
    writeFile(temp.path() / "p-20260101-000000.log", "a\n");
    auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, true).value();
    CHECK(sink->currentFile() == temp.path() / "p-20260101-000000-2.log");
}

TEST_CASE("split sink: strict name parsing") {
    using dvv::detail::parseSplitLogName;
    const auto plain = parseSplitLogName("p-20261110-153126.log", "p");
    REQUIRE(plain.has_value());
    CHECK(plain.value().stamp == 20261110153126U);
    CHECK(plain.value().number == 1U);
    const auto suffixed = parseSplitLogName("p-20261110-153126-12.log", "p");
    REQUIRE(suffixed.has_value());
    CHECK(suffixed.value().number == 12U);
    CHECK(parseSplitLogName("rawreel-20261110-153126.log", "rawreel").has_value());

    CHECK_FALSE(parseSplitLogName("p-old.log", "p").has_value());
    CHECK_FALSE(parseSplitLogName("p-20261110-1.log", "p").has_value());
    CHECK_FALSE(parseSplitLogName("pp-20261110-153126.log", "p").has_value());
    CHECK_FALSE(parseSplitLogName("P-20261110-153126.log", "p").has_value());
    CHECK_FALSE(parseSplitLogName("p-20261110-153126.log.1", "p").has_value());
    CHECK_FALSE(parseSplitLogName("p-20261110-153126.LOG", "p").has_value());
    CHECK_FALSE(parseSplitLogName("p-20261110-153126-.log", "p").has_value());
    CHECK_FALSE(parseSplitLogName("p-20261110-153126-0.log", "p").has_value());
    CHECK_FALSE(parseSplitLogName("p-20261110-153126-02.log", "p").has_value());
    CHECK_FALSE(parseSplitLogName("p-20261110-153126-x.log", "p").has_value());
    CHECK_FALSE(parseSplitLogName("p-2026111a-153126.log", "p").has_value());
    CHECK_FALSE(parseSplitLogName("p-20261110_153126.log", "p").has_value());
    CHECK_FALSE(parseSplitLogName("p-202611100-153126.log", "p").has_value());
    CHECK_FALSE(parseSplitLogName("p-20261110-153126-1234567890.log", "p").has_value());
    CHECK_FALSE(parseSplitLogName("", "p").has_value());
}

TEST_CASE("split sink: non-matching names are ignored") {
    const TempDir temp("dvv-base-split-ignore");
    const std::vector<std::string> foreign = {"p-old.log", "p-20261110-1.log",
                                              "pp-20991231-235959.log", "p-20991231-235959.log.bak",
                                              "p-20991231-235959-02.log"};
    for (const auto& name : foreign) {
        writeFile(temp.path() / name, name + "\n");
    }
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, true).value();
        const auto current = utf8Name(sink->currentFile());
        CHECK(std::ranges::find(foreign, current) == foreign.end());
        CHECK(std::regex_match(current, std::regex(R"(^p-\d{8}-\d{6}\.log$)")));
        CHECK(sink->totalBytes() == 0);
        writeLine(*sink, "line");
    }
    CHECK(fileNames(temp.path()).size() == foreign.size() + 1);
    for (const auto& name : foreign) {
        CHECK(readFile(temp.path() / name) == name + "\n");
    }
}

TEST_CASE("split sink: open(false) always creates") {
    const TempDir temp("dvv-base-split-create");
    writeFile(temp.path() / "p-20260102-000000.log", "abcdefghi\n");
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, false).value();
        CHECK(sink->currentFile() != temp.path() / "p-20260102-000000.log");
        CHECK(sink->splits() == 0);
        writeLine(*sink, "new");
    }
    CHECK(fileNames(temp.path()).size() == 2);
    CHECK(readFile(temp.path() / "p-20260102-000000.log") == "abcdefghi\n");
}

TEST_CASE("split sink: open rejects a zero limit or an empty prefix") {
    const TempDir temp("dvv-base-split-bad");
    CHECK_FALSE(dvv::SplitFileSink::open(temp.path(), "p", 0, true).has_value());
    CHECK_FALSE(dvv::SplitFileSink::open(temp.path(), "", 100, true).has_value());
    CHECK(fileNames(temp.path()).empty());
}

TEST_CASE("split sink: open creates the folder") {
    const TempDir temp("dvv-base-split-folder");
    const auto folder = temp.path() / "a" / "b";
    auto sink = dvv::SplitFileSink::open(folder, "p", 100, true).value();
    CHECK(fileNames(folder).size() == 1);
}

TEST_CASE("split sink: splits at the limit without dividing a line") {
    const TempDir temp("dvv-base-split-limit");
    std::vector<std::string> written;
    std::uint64_t splits = 0;
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 100, false).value();
        // 30-byte lines: three fit in 100 bytes, a fourth would pass the limit.
        for (int i = 0; i < 10; ++i) {
            written.push_back(lineOf(30, std::format("line {:02} ", i)));
            writeLine(*sink, written.back());
        }
        // A line larger than the limit gets a file of its own.
        written.push_back(lineOf(150, "large "));
        writeLine(*sink, written.back());
        written.push_back(lineOf(30, "after "));
        writeLine(*sink, written.back());
        splits = sink->splits();
    }
    const auto names = fileNames(temp.path());
    // 3 + 3 + 3 + 1 lines, the large line, the last line.
    CHECK(names.size() == 6);
    CHECK(splits == names.size() - 1);
    std::vector<std::string> read;
    for (const auto& name : names) {
        const auto text = readFile(temp.path() / name);
        const auto lines = linesOf(text);
        if (lines.size() > 1) {
            CHECK(text.size() <= 100);
        }
        CHECK((text.empty() || text.back() == '\n'));
        read.insert(read.end(), lines.begin(), lines.end());
    }
    // Every line whole, none lost (the files are read in name order, so only the multiset is
    // compared).
    std::ranges::sort(read);
    auto expected = written;
    std::ranges::sort(expected);
    CHECK(read == expected);
}

TEST_CASE("split sink: never splits an empty file") {
    const TempDir temp("dvv-base-split-empty");
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 100, false).value();
        writeLine(*sink, lineOf(500, "big "));
        CHECK(sink->splits() == 0);
    }
    CHECK(fileNames(temp.path()).size() == 1);
}

TEST_CASE("split sink: a file with only its header is not split") {
    const TempDir temp("dvv-base-split-header-only");
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 100, false).value();
        sink->setHeader(
            [](const std::filesystem::path& /*previous*/) { return std::string(90, 'h'); });
        writeLine(*sink, lineOf(500, "big "));
        CHECK(sink->splits() == 0);
        writeLine(*sink, "small");
        CHECK(sink->splits() == 1);
    }
    CHECK(fileNames(temp.path()).size() == 2);
}

TEST_CASE("split sink: header in each new file, not in an appended one") {
    const TempDir temp("dvv-base-split-header");
    writeFile(temp.path() / "p-20260101-000000.log", "old\n");
    std::vector<std::filesystem::path> previous;
    std::vector<std::filesystem::path> files;
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 200, true).value();
        files.push_back(sink->currentFile());
        sink->setHeader([&previous](const std::filesystem::path& file) {
            previous.push_back(file);
            return std::format("HEADER after '{}'", utf8Name(file));
        });
        CHECK(previous.empty());  // the appended file gets no header
        for (int i = 0; i < 12; ++i) {
            writeLine(*sink, lineOf(40, std::format("line {:02} ", i)));
            if (sink->currentFile() != files.back()) {
                files.push_back(sink->currentFile());
            }
        }
    }
    REQUIRE(files.size() >= 3);
    REQUIRE(previous.size() == files.size() - 1);
    for (std::size_t i = 0; i < previous.size(); ++i) {
        CHECK(previous[i] == files[i]);
    }
    const auto appended = linesOf(readFile(files[0]));
    REQUIRE_FALSE(appended.empty());
    CHECK(appended[0] == "old");
    CHECK(appended[1].starts_with("line 00"));
    for (std::size_t i = 1; i < files.size(); ++i) {
        const auto lines = linesOf(readFile(files[i]));
        REQUIRE(lines.size() >= 2);
        CHECK(lines[0].ends_with(std::format("Z INFO  HEADER after '{}'", utf8Name(files[i - 1]))));
        CHECK(lines[1].starts_with("line "));
    }
}

TEST_CASE("split sink: the header of a created file is written by setHeader, with no previous") {
    const TempDir temp("dvv-base-split-header-first");
    std::vector<std::filesystem::path> previous;
    std::filesystem::path file;
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, true).value();
        file = sink->currentFile();
        sink->setHeader([&previous](const std::filesystem::path& from) {
            previous.push_back(from);
            return std::string("HEADER");
        });
        // Flushed at once.
        CHECK(readFile(file).ends_with("Z INFO  HEADER\n"));
        writeLine(*sink, "first line");
    }
    REQUIRE(previous.size() == 1);
    CHECK(previous[0].empty());
    const auto lines = linesOf(readFile(file));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].ends_with("Z INFO  HEADER"));
    CHECK(lines[1] == "first line");
}

TEST_CASE("split sink: setHeader after the first line writes no header") {
    const TempDir temp("dvv-base-split-header-late");
    std::filesystem::path file;
    int calls = 0;
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, false).value();
        file = sink->currentFile();
        writeLine(*sink, "first line");
        sink->setHeader([&calls](const std::filesystem::path& /*previous*/) {
            ++calls;
            return std::string("HEADER");
        });
    }
    CHECK(calls == 0);
    CHECK(readFile(file) == "first line\n");
}

#if !defined(_WIN32)
TEST_CASE("split sink: a folder that cannot take a file fails the open") {
    if (::geteuid() == 0) {
        return;  // permissions do not stop root
    }
    const TempDir temp("dvv-base-split-readonly-open");
    std::filesystem::permissions(
        temp.path(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    CHECK_FALSE(dvv::SplitFileSink::open(temp.path(), "p", 100, false).has_value());
}

TEST_CASE("split sink: back-off after a failed creation") {
    if (::geteuid() == 0) {
        return;  // permissions do not stop root
    }
    const TempDir temp("dvv-base-split-backoff");
    {
        // max_bytes 400: back-off 100 bytes, a bit over three 30-byte lines.
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 400, false).value();
        const auto first = sink->currentFile();
        for (int i = 0; i < 13; ++i) {  // 390 bytes
            writeLine(*sink, lineOf(30, std::format("line {:02} ", i)));
        }
        std::filesystem::permissions(
            temp.path(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
        // Passes the limit: the creation fails, the line goes to the current file.
        writeLine(*sink, lineOf(30, "line 13 "));
        CHECK(sink->currentFile() == first);
        CHECK(sink->splits() == 0);
        std::filesystem::permissions(temp.path(), std::filesystem::perms::owner_all);
        // Less than max_bytes / 4 more: no new attempt, although it would now succeed.
        for (int i = 14; i < 17; ++i) {
            writeLine(*sink, lineOf(30, std::format("line {:02} ", i)));
        }
        CHECK(sink->currentFile() == first);
        CHECK(sink->splits() == 0);
        // max_bytes / 4 reached: the next line starts a new file.
        writeLine(*sink, lineOf(30, "line 17 "));
        CHECK(sink->currentFile() != first);
        CHECK(sink->splits() == 1);
        sink->flush();
        CHECK(readFile(first).size() == 17 * 30);
        CHECK(readFile(sink->currentFile()).starts_with("line 17 "));
    }
    CHECK(fileNames(temp.path()).size() == 2);
}

TEST_CASE("split sink: a newest file that cannot be appended to gets a new file") {
    if (::geteuid() == 0) {
        return;  // permissions do not stop root
    }
    const TempDir temp("dvv-base-split-locked");
    const auto locked = temp.path() / "p-20260101-000000.log";
    writeFile(locked, "locked\n");
    std::filesystem::permissions(locked, std::filesystem::perms::owner_read);
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, true).value();
        CHECK(sink->currentFile() != locked);
        writeLine(*sink, "line");
    }
    CHECK(readFile(locked) == "locked\n");
    CHECK(fileNames(temp.path()).size() == 2);
}

TEST_CASE("split sink: a failed write closes the file; a new one waits for the back-off") {
    const TempDir temp("dvv-base-split-write-fail");
    // A file size limit of 200 bytes makes the write past it fail (EFBIG, SIGXFSZ ignored).
    rlimit saved{};
    REQUIRE(::getrlimit(RLIMIT_FSIZE, &saved) == 0);
    const auto previous_handler = std::signal(SIGXFSZ, SIG_IGN);
    std::vector<std::filesystem::path> files;
    {
        // max_bytes 1000: back-off 250 bytes.
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, false).value();
        files.push_back(sink->currentFile());
        rlimit small = saved;
        small.rlim_cur = 200;
        REQUIRE(::setrlimit(RLIMIT_FSIZE, &small) == 0);
        // Warn lines are flushed at once, so the failure shows on the line that passes 200.
        for (int i = 0; i < 9; ++i) {
            writeLine(*sink, lineOf(30, std::format("line {:02} ", i)), dvv::LogLevel::Warn);
        }
        REQUIRE(::setrlimit(RLIMIT_FSIZE, &saved) == 0);
        // Line 6 failed after 180 bytes, 180 bytes after the file started: the file is closed
        // and the lines are dropped until 250 bytes were offered since it started (lines 6-8).
        CHECK(sink->splits() == 0);
        CHECK(sink->currentFile() == files[0]);
        writeLine(*sink, lineOf(30, "line 09 "), dvv::LogLevel::Warn);
        CHECK(sink->splits() == 1);
        files.push_back(sink->currentFile());
    }
    std::signal(SIGXFSZ, previous_handler);
    REQUIRE(files.size() == 2);
    CHECK(files[0] != files[1]);
    const auto first = readFile(files[0]);
    CHECK(first.starts_with("line 00 "));
    CHECK(first.find("line 05 ") != std::string::npos);
    CHECK(first.find("line 07 ") == std::string::npos);
    CHECK(first.find("line 08 ") == std::string::npos);
    CHECK(readFile(files[1]) == lineOf(30, "line 09 ") + "\n");
}

TEST_CASE("split sink: a failed write after max_bytes / 4 tries a new file at once") {
    const TempDir temp("dvv-base-split-write-retry");
    rlimit saved{};
    REQUIRE(::getrlimit(RLIMIT_FSIZE, &saved) == 0);
    const auto previous_handler = std::signal(SIGXFSZ, SIG_IGN);
    std::filesystem::path first;
    std::filesystem::path second;
    {
        // max_bytes 1000: back-off 250 bytes; the file fails at 400, after the back-off.
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, false).value();
        first = sink->currentFile();
        rlimit small = saved;
        small.rlim_cur = 400;
        REQUIRE(::setrlimit(RLIMIT_FSIZE, &small) == 0);
        for (int i = 0; i < 15; ++i) {
            writeLine(*sink, lineOf(30, std::format("line {:02} ", i)), dvv::LogLevel::Warn);
        }
        REQUIRE(::setrlimit(RLIMIT_FSIZE, &saved) == 0);
        CHECK(sink->splits() == 1);
        second = sink->currentFile();
    }
    std::signal(SIGXFSZ, previous_handler);
    CHECK(first != second);
    CHECK(readFile(first).find("line 12 ") != std::string::npos);
    // Line 13 failed in the first file and was written whole into the second.
    const auto lines = linesOf(readFile(second));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].starts_with("line 13 "));
    CHECK(lines[1].starts_with("line 14 "));
}

TEST_CASE("split sink: a split whose old file cannot be flushed notes it on stderr") {
    const TempDir temp("dvv-base-split-flush-fail");
    const auto note_path = temp.path() / "stderr.txt";
    rlimit saved{};
    REQUIRE(::getrlimit(RLIMIT_FSIZE, &saved) == 0);
    const auto previous_handler = std::signal(SIGXFSZ, SIG_IGN);
    std::fflush(stderr);
    const int saved_stderr = ::dup(STDERR_FILENO);
    REQUIRE(saved_stderr >= 0);
    REQUIRE(std::freopen(note_path.c_str(), "w", stderr) != nullptr);
    {
        // max_bytes 1000: ten 99-byte Info lines stay in the buffer, then the split flushes them
        // past the 300 byte file size limit.
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 1000, false).value();
        const auto first = sink->currentFile();
        rlimit small = saved;
        small.rlim_cur = 300;
        REQUIRE(::setrlimit(RLIMIT_FSIZE, &small) == 0);
        for (int i = 0; i < 11; ++i) {
            writeLine(*sink, lineOf(100, std::format("line {:02} ", i)));
        }
        REQUIRE(::setrlimit(RLIMIT_FSIZE, &saved) == 0);
        CHECK(sink->splits() == 1);
        CHECK(sink->currentFile() != first);
    }
    std::fflush(stderr);
    REQUIRE(::dup2(saved_stderr, STDERR_FILENO) >= 0);
    ::close(saved_stderr);
    std::signal(SIGXFSZ, previous_handler);
    CHECK(readFile(note_path).find("writing log file") != std::string::npos);
}
#endif

TEST_CASE("split sink: nothing renamed or deleted") {
    const TempDir temp("dvv-base-split-keep");
    writeFile(temp.path() / "p-20260101-000000.log", "old\n");
    writeFile(temp.path() / "p.log", "legacy\n");
    writeFile(temp.path() / "p.log.1", "legacy backup\n");
    std::set<std::filesystem::path> files;
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 100, true).value();
        for (int i = 0; i < 20; ++i) {
            files.insert(sink->currentFile());
            writeLine(*sink, lineOf(30, std::format("line {:02} ", i)));
            files.insert(sink->currentFile());
        }
        CHECK(sink->splits() == files.size() - 1);
    }
    CHECK(files.size() >= 5);
    for (const auto& file : files) {
        CHECK(std::filesystem::exists(file));
    }
    CHECK(readFile(temp.path() / "p-20260101-000000.log").starts_with("old\n"));
    CHECK(readFile(temp.path() / "p.log") == "legacy\n");
    CHECK(readFile(temp.path() / "p.log.1") == "legacy backup\n");
    CHECK(fileNames(temp.path()).size() == files.size() + 2);
}

TEST_CASE("split sink: totalBytes") {
    const TempDir temp("dvv-base-split-total");
    writeFile(temp.path() / "p-20260101-000000.log", std::string(10, 'a'));
    writeFile(temp.path() / "p-20260102-000000-2.log", std::string(20, 'b'));
    writeFile(temp.path() / "p-other.log", std::string(1000, 'c'));
    {
        auto sink = dvv::SplitFileSink::open(temp.path(), "p", 100, true).value();
        CHECK(sink->totalBytes() == 30);
        sink->setHeader(
            [](const std::filesystem::path& /*previous*/) { return std::string("HEADER"); });
        for (int i = 0; i < 8; ++i) {
            writeLine(*sink, lineOf(30, std::format("line {:02} ", i)));
        }
        sink->flush();
        CHECK(sink->splits() >= 2);
        std::uintmax_t on_disk = 0;
        for (const auto& name : fileNames(temp.path())) {
            if (dvv::detail::parseSplitLogName(name, "p").has_value()) {
                on_disk += std::filesystem::file_size(temp.path() / name);
            }
        }
        CHECK(sink->totalBytes() == on_disk);
        CHECK(sink->totalBytes() > 30 + 8 * 30);
    }
}

TEST_CASE("split sink: a mark is flushed at once") {
    const TempDir temp("dvv-base-split-mark");
    LoggerScope scope;
    auto sink = dvv::SplitFileSink::open(temp.path(), "p", 100000, false).value();
    dvv::Logger::instance().addSink(sink);
    DVV_LOG_INFO("buffered");
    CHECK(readFile(sink->currentFile()).empty());  // Info stays in the stream's buffer
    dvv::Logger::instance().mark("===== marker =====");
    // The sink is still open and flush() was never called.
    const auto text = readFile(sink->currentFile());
    CHECK(text.find("INFO  buffered") != std::string::npos);
    CHECK(text.find("INFO  ===== marker =====") != std::string::npos);
    dvv::Logger::instance().clearSinks();
}

TEST_CASE("split sink: a warning is flushed at once") {
    const TempDir temp("dvv-base-split-warn");
    LoggerScope scope;
    auto sink = dvv::SplitFileSink::open(temp.path(), "p", 100000, false).value();
    dvv::Logger::instance().addSink(sink);
    DVV_LOG_INFO("buffered");
    DVV_LOG_WARN("urgent");
    CHECK(readFile(sink->currentFile()).find("WARN  urgent") != std::string::npos);
    dvv::Logger::instance().clearSinks();
}
