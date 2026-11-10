#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "dvv/base/log.h"
#include "log_ring.h"
#include "log_test_support.h"

namespace {

using dvv::LogLevel;
using dvv_test::AsyncScope;
using dvv_test::WaitingSink;

dvv::Logger& logger() {
    return dvv::Logger::instance();
}

// Keeps every record with its line. Written only on the log thread; read after a wait that
// orders it (waitFor, flush).
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

std::size_t countContaining(const std::vector<std::string>& lines, std::string_view part) {
    return static_cast<std::size_t>(std::ranges::count_if(
        lines, [&](const std::string& line) { return line.find(part) != std::string::npos; }));
}

// The index of the first line containing `part`, or lines.size().
std::size_t indexOf(const std::vector<std::string>& lines, std::string_view part) {
    const auto found = std::ranges::find_if(
        lines, [&](const std::string& line) { return line.find(part) != std::string::npos; });
    return static_cast<std::size_t>(found - lines.begin());
}

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

TEST_CASE("async: mark is delivered in asynchronous mode in order") {
    const AsyncScope scope;
    auto records = std::make_shared<RecordSink>();
    auto errors = std::make_shared<RecordSink>();
    auto off = std::make_shared<RecordSink>();
    logger().addSink(records);
    logger().addSink(errors, LogLevel::Error);
    logger().addSink(off, LogLevel::Off);
    logger().setLevel(LogLevel::Warn);
    logger().start();
    DVV_LOG_WARN("a {}", 1);
    DVV_LOG_INFO("hidden {}", 2);  // below the logger level
    const auto where = std::source_location::current();
    logger().mark("m", where);
    logger().write(LogLevel::Warn, "b");
    logger().flush();

    REQUIRE(records->entries.size() == 3);
    CHECK(records->entries[0].message == "a 1");
    CHECK(records->entries[1].message == "m");
    CHECK(records->entries[2].message == "b");
    CHECK_FALSE(records->entries[0].record.mark);
    CHECK(records->entries[1].record.mark);
    CHECK_FALSE(records->entries[2].record.mark);
    CHECK(records->entries[1].record.level == LogLevel::Info);
    CHECK(records->entries[1].record.where.line() == where.line());
    CHECK(records->entries[1].line == dvv::formatLogRecord(records->entries[1].record));
    REQUIRE(errors->entries.size() == 1);
    CHECK(errors->entries[0].message == "m");
    CHECK(errors->entries[0].record.mark);
    CHECK(off->entries.empty());
}

TEST_CASE("async: mark with every sink at Off queues nothing") {
    const AsyncScope scope;
    auto off = std::make_shared<RecordSink>();
    logger().addSink(off, LogLevel::Off);
    logger().start();
    const std::uint64_t before = dvv::detail::loggerRingForTest().writePosition();
    logger().mark("nobody listens");
    CHECK(dvv::detail::loggerRingForTest().writePosition() == before);
    logger().stop();
    CHECK(off->entries.empty());
}

TEST_CASE("async: messages reach the sinks in order, lines identical to synchronous mode") {
    const AsyncScope scope;
    auto records = std::make_shared<RecordSink>();
    auto waiting = std::make_shared<WaitingSink>();
    logger().addSink(records);
    logger().addSink(waiting);
    logger().start();
    constexpr int COUNT = 1000;
    for (int i = 0; i < COUNT; ++i) {
        DVV_LOG_INFO("n {}", i);
    }
    DVV_LOG_INFO("{} {:>4}", 7, "ab");
    DVV_LOG_INFO("colour {}", dvv_test::Colour::Red);
    const auto where = std::source_location::current();
    logger().write(LogLevel::Warn, "x", where);
    waiting->waitFor(COUNT + 3);

    REQUIRE(records->entries.size() == COUNT + 3);
    for (int i = 0; i < COUNT; ++i) {
        CHECK(records->entries[static_cast<std::size_t>(i)].message == std::format("n {}", i));
    }
    CHECK(records->entries[COUNT].message == "7   ab");
    CHECK(records->entries[COUNT + 1].message == "colour red");
    CHECK(records->entries[COUNT + 2].message == "x");
    CHECK(records->entries[COUNT + 2].record.level == LogLevel::Warn);
    CHECK(records->entries[COUNT + 2].line.ends_with(
        std::format("[test_log_async.cpp:{}]", where.line())));
    for (const auto& entry : records->entries) {
        CHECK(entry.line == dvv::formatLogRecord(entry.record));
    }
    CHECK(waiting->lines().size() == COUNT + 3);
}

TEST_CASE("async: bursts with idle gaps from 8 threads lose no wake-up") {
    const AsyncScope scope;
    auto waiting = std::make_shared<WaitingSink>();
    logger().addSink(waiting);
    logger().start();
    constexpr int THREADS = 8;
    constexpr int PER_ROUND = 20;
    constexpr int ROUNDS = 50;
    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int id = 0; id < THREADS; ++id) {
        threads.emplace_back([&waiting, id] {
            for (int round = 0; round < ROUNDS; ++round) {
                for (int i = 0; i < PER_ROUND; ++i) {
                    DVV_LOG_INFO("thread {} round {} message {}", id, round, i);
                }
                // The barrier: every thread waits until the whole round reached the sink, so the
                // log thread goes idle before the next burst. A lost wake-up hangs here.
                waiting->waitFor(static_cast<std::uint32_t>((round + 1) * THREADS * PER_ROUND));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(waiting->lines().size() == std::size_t{THREADS} * PER_ROUND * ROUNDS);
}

TEST_CASE("async: bare {} warns once per call site") {
    const AsyncScope scope;
    auto sink = std::make_shared<dvv::MemorySink>();
    logger().addSink(sink);
    logger().start();
    const std::string text = "bare async text";
    for (int i = 0; i < 2; ++i) {
        DVV_LOG_INFO("{}", text);
    }
    logger().flush();
    const auto lines = sink->lines();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].find("INFO  bare async text") != std::string::npos);
    CHECK(lines[1].find("WARN") != std::string::npos);
    CHECK(lines[1].find("formats only \"{}\"") != std::string::npos);
    CHECK(lines[1].find("test_log_async.cpp") != std::string::npos);
    CHECK(lines[2].find("INFO  bare async text") != std::string::npos);
}

namespace {

// Counts lines and remembers how many it had when flush() ran.
class FlushCountSink final : public dvv::ILogSink {
public:
    void write(const dvv::LogRecord& /*record*/, std::string_view /*line*/) override { ++lines; }
    void flush() override { flushedAt = lines.load(); }

    std::atomic<int> lines{0};
    std::atomic<int> flushedAt{-1};
};

}  // namespace

TEST_CASE("async: flush returns after earlier messages are in every sink and flushed") {
    const AsyncScope scope;
    auto first = std::make_shared<FlushCountSink>();
    auto second = std::make_shared<FlushCountSink>();
    logger().addSink(first);
    logger().addSink(second);
    logger().start();
    constexpr int COUNT = 100;
    for (int i = 0; i < COUNT; ++i) {
        DVV_LOG_INFO("flush me {}", i);
    }
    logger().flush();
    CHECK(first->lines == COUNT);
    CHECK(second->lines == COUNT);
    CHECK(first->flushedAt == COUNT);
    CHECK(second->flushedAt == COUNT);
}

TEST_CASE("async: flush waits for a record published after an unfinished earlier one") {
    const AsyncScope scope;
    auto sink = std::make_shared<dvv::MemorySink>();
    logger().addSink(sink);
    logger().start();
    // A writer still encoding: a reservation before A that is not published yet.
    auto& ring = dvv::detail::loggerRingForTest();
    const auto held = ring.reserve(8, true);
    if (!held) {
        FAIL("the ring refused a small reservation");
        return;
    }
    DVV_LOG_INFO("logged before flush");
    const std::uint64_t ticket = dvv::detail::flushRequestsForTest() + 1;
    std::atomic<bool> returned{false};
    std::vector<std::string> lines_at_return;
    std::thread flusher([&] {
        dvv::Logger::instance().flush();
        lines_at_return = sink->lines();
        returned.store(true);
    });
    // The log thread has taken the request and fixed its target; the
    // flush must not complete while a record below its target is unpublished.
    dvv::detail::waitFlushSeenForTest(ticket);
    CHECK_FALSE(returned.load());
    ring.publishAsPadding(*held);
    // A hand-published record wakes nobody; the next message wakes the log thread.
    DVV_LOG_INFO("wake");
    flusher.join();
    CHECK(countContaining(lines_at_return, "INFO  logged before flush") == 1);
}

namespace {

// Blocks every write() until open() is called.
class GateSink final : public dvv::ILogSink {
public:
    void write(const dvv::LogRecord& /*record*/, std::string_view line) override {
        gate_.wait(false);
        const std::lock_guard lock(mutex_);
        lines_.emplace_back(line);
    }

    void open() {
        gate_.store(true);
        gate_.notify_all();
    }

    [[nodiscard]] std::vector<std::string> lines() const {
        const std::lock_guard lock(mutex_);
        return lines_;
    }

private:
    std::atomic<bool> gate_{false};
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

// Opens the gate when a check fails, so the scope's stop() does not wait for ever.
struct GateOpener {
    GateSink& sink;
    ~GateOpener() { sink.open(); }
};

}  // namespace

TEST_CASE("async: a blocked sink makes Info drop at 3/4 while Warn still queues") {
    const AsyncScope scope;
    auto sink = std::make_shared<GateSink>();
    logger().addSink(sink);
    logger().start();
    const GateOpener opener{*sink};
    constexpr std::uint64_t MAX_CALLS = 1'000'000;
    std::uint64_t calls = 0;
    while (dvv::detail::droppedForTest(LogLevel::Info) == 0 && calls < MAX_CALLS) {
        DVV_LOG_INFO("fill {}", calls);
        ++calls;
    }
    const std::uint64_t dropped = dvv::detail::droppedForTest(LogLevel::Info);
    REQUIRE(dropped > 0);
    // Info stopped at 3/4 of the ring; a warning still fits.
    CHECK(dvv::detail::loggerRingForTest().bytes() == dvv::detail::LOG_RING_BYTES);
    DVV_LOG_WARN("after the drops");
    CHECK(dvv::detail::droppedForTest(LogLevel::Warn) == 0);
    CHECK(dvv::detail::droppedForTest(LogLevel::Info) == dropped);
    sink->open();
    logger().flush();

    const auto lines = sink->lines();
    const std::string dropped_text =
        std::format("WARN  {} log messages dropped ({} info) [", dropped, dropped);
    CHECK(countContaining(lines, "log messages dropped (") == 1);
    const std::size_t dropped_index = indexOf(lines, dropped_text);
    const std::size_t warn_index = indexOf(lines, "WARN  after the drops");
    REQUIRE(dropped_index < lines.size());
    REQUIRE(warn_index < lines.size());
    CHECK(dropped_index < warn_index);
    CHECK(countContaining(lines, "INFO  fill ") == calls - dropped);
    CHECK(dvv::detail::droppedForTest(LogLevel::Info) == 0);
}

TEST_CASE("async: mark survives a ring filled past 3/4") {
    const AsyncScope scope;
    auto sink = std::make_shared<GateSink>();
    logger().addSink(sink);
    logger().start();
    const GateOpener opener{*sink};
    constexpr std::uint64_t MAX_CALLS = 1'000'000;
    std::uint64_t calls = 0;
    while (dvv::detail::droppedForTest(LogLevel::Info) == 0 && calls < MAX_CALLS) {
        DVV_LOG_INFO("fill {}", calls);
        ++calls;
    }
    const std::uint64_t dropped = dvv::detail::droppedForTest(LogLevel::Info);
    REQUIRE(dropped > 0);
    // A plain Info is still refused, a mark (Info with Warn's priority) is not.
    logger().write(LogLevel::Info, "plain after the drops");
    CHECK(dvv::detail::droppedForTest(LogLevel::Info) == dropped + 1);
    logger().mark("===== survivor =====");
    CHECK(dvv::detail::droppedForTest(LogLevel::Info) == dropped + 1);
    CHECK(dvv::detail::droppedForTest(LogLevel::Warn) == 0);
    sink->open();
    logger().flush();

    const auto lines = sink->lines();
    CHECK(countContaining(lines, "INFO  ===== survivor =====") == 1);
    CHECK(countContaining(lines, "plain after the drops") == 0);
    CHECK(countContaining(lines, "INFO  fill ") == calls - dropped);
}

namespace {

// No lock of its own: two threads in write() at once would be a data race (TSan).
class UnlockedSink final : public dvv::ILogSink {
public:
    void write(const dvv::LogRecord& record, std::string_view /*line*/) override {
        messages.emplace_back(record.message);
    }

    std::vector<std::string> messages;
};

// Reads "t <id> s <seq>"; false for any other text.
bool parseThreadMessage(std::string_view message, int& id, int& seq) {
    if (!message.starts_with("t ")) {
        return false;
    }
    const char* const end = message.data() + message.size();
    const auto [after_id, id_error] = std::from_chars(message.data() + 2, end, id);
    if (id_error != std::errc{} || !std::string_view(after_id, end).starts_with(" s ")) {
        return false;
    }
    const auto [after_seq, seq_error] = std::from_chars(after_id + 3, end, seq);
    return seq_error == std::errc{} && after_seq == end;
}

}  // namespace

TEST_CASE("async: pool threads logging during stop lose nothing and keep their order") {
    const AsyncScope scope;
    auto sink = std::make_shared<UnlockedSink>();
    logger().addSink(sink);
    logger().start();
    constexpr int THREADS = 8;
    constexpr int PER_THREAD = 2000;
    constexpr int MIDWAY = THREADS * PER_THREAD / 2;
    std::atomic<int> logged{0};
    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int id = 0; id < THREADS; ++id) {
        threads.emplace_back([&logged, id] {
            for (int seq = 0; seq < PER_THREAD; ++seq) {
                DVV_LOG_WARN("t {} s {}", id, seq);
                if (logged.fetch_add(1) + 1 == MIDWAY) {
                    logged.notify_all();
                }
            }
        });
    }
    for (int seen = logged.load(); seen < MIDWAY; seen = logged.load()) {
        logged.wait(seen);
    }
    logger().stop();
    for (auto& thread : threads) {
        thread.join();
    }

    REQUIRE(sink->messages.size() == std::size_t{THREADS} * PER_THREAD);
    std::vector<int> next(THREADS, 0);
    int out_of_order = 0;
    for (const auto& message : sink->messages) {
        int id = -1;
        int seq = -1;
        REQUIRE(parseThreadMessage(message, id, seq));
        REQUIRE(id >= 0);
        REQUIRE(id < THREADS);
        if (seq != next[static_cast<std::size_t>(id)]) {
            ++out_of_order;
        }
        next[static_cast<std::size_t>(id)] = seq + 1;
    }
    CHECK(out_of_order == 0);
    CHECK(std::ranges::all_of(next, [](int value) { return value == PER_THREAD; }));
    CHECK(dvv::detail::droppedForTest(LogLevel::Warn) == 0);

    // Synchronous again: handled before the call returns.
    DVV_LOG_WARN("after stop");
    REQUIRE(sink->messages.size() == std::size_t{THREADS} * PER_THREAD + 1);
    CHECK(sink->messages.back() == "after stop");
}

TEST_CASE("async: flush concurrent with stop returns") {
    const AsyncScope scope;
    auto sink = std::make_shared<dvv::MemorySink>();
    logger().addSink(sink);
    logger().start();
    constexpr int COUNT = 200;
    std::atomic<int> flushes{0};
    std::thread worker([&flushes] {
        for (int i = 0; i < COUNT; ++i) {
            DVV_LOG_INFO("worker {}", i);
            dvv::Logger::instance().flush();
            if (flushes.fetch_add(1) == 0) {
                flushes.notify_all();
            }
        }
    });
    flushes.wait(0);
    logger().stop();
    worker.join();
    CHECK(sink->lines().size() == COUNT);
}

TEST_CASE("async: start and stop twice are harmless; addSink while running is ignored and logged") {
    const AsyncScope scope;
    auto sink = std::make_shared<dvv::MemorySink>();
    logger().addSink(sink);
    logger().start();
    logger().start();
    auto other = std::make_shared<dvv::MemorySink>();
    logger().addSink(other);
    logger().clearSinks();
    DVV_LOG_INFO("still here");
    logger().flush();
    auto lines = sink->lines();
    CHECK(countContaining(lines, "ERROR Logger::addSink ignored: the log thread is running") == 1);
    CHECK(countContaining(lines, "ERROR Logger::clearSinks ignored: the log thread is running") ==
          1);
    CHECK(countContaining(lines, "INFO  still here") == 1);
    CHECK(other->lines().empty());

    logger().stop();
    logger().stop();
    DVV_LOG_INFO("synchronous again");
    lines = sink->lines();
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.back().find("INFO  synchronous again") != std::string::npos);

    // A restart works too.
    logger().start();
    DVV_LOG_INFO("started again");
    logger().flush();
    CHECK(sink->lines().back().find("INFO  started again") != std::string::npos);
}

TEST_CASE("async: stop without start still flushes the file sink") {
    const TempDir temp("dvv-base-log-async-stop");
    const AsyncScope scope;
    const auto path = temp.path() / "stop.log";
    auto sink = dvv::RotatingFileSink::open(path, 100000, 1).value();
    logger().addSink(sink);
    DVV_LOG_INFO("buffered until stop");
    logger().stop();
    // The sink is still open; only stop() flushed it.
    CHECK(readFile(path).find("INFO  buffered until stop") != std::string::npos);
}

namespace {

class ThrowingSink final : public dvv::ILogSink {
public:
    void write(const dvv::LogRecord& /*record*/, std::string_view /*line*/) override {
        throw std::runtime_error("boom");
    }
};

}  // namespace

TEST_CASE("async: a throwing sink loses only its own line and is reported once") {
    const AsyncScope scope;
    auto thrower = std::make_shared<ThrowingSink>();
    auto sink = std::make_shared<dvv::MemorySink>();
    logger().addSink(thrower);
    logger().addSink(sink);
    logger().start();
    DVV_LOG_INFO("first {}", 1);
    DVV_LOG_INFO("second {}", 2);
    logger().flush();
    DVV_LOG_INFO("third {}", 3);
    logger().flush();
    const auto lines = sink->lines();
    CHECK(countContaining(lines, "INFO  first 1") == 1);
    CHECK(countContaining(lines, "INFO  second 2") == 1);
    CHECK(countContaining(lines, "INFO  third 3") == 1);
    CHECK(countContaining(lines, "WARN  a log sink failed: boom") == 1);
    CHECK(lines.size() == 4);
}

namespace {

// Fails in batchEnd() once it has written `trigger`: a chain of them fails pass after pass, each on
// the pass that carries the previous one's failure report.
class ChainFailingSink final : public dvv::ILogSink {
public:
    ChainFailingSink(std::string trigger, std::string name)
        : trigger_(std::move(trigger)), name_(std::move(name)) {}

    void write(const dvv::LogRecord& /*record*/, std::string_view line) override {
        armed_ = armed_ || line.find(trigger_) != std::string_view::npos;
    }

    void batchEnd() override {
        if (armed_) {
            throw std::runtime_error(name_);
        }
    }

private:
    std::string trigger_;
    std::string name_;
    bool armed_ = false;
};

}  // namespace

TEST_CASE("async: sink failures reported during stop still reach the sinks") {
    // However the passes fall around the stop request, the last failure report can be queued by
    // stop()'s own drain; stop() handles it before it returns.
    constexpr int ROUNDS = 20;
    for (int round = 0; round < ROUNDS; ++round) {
        const AsyncScope scope;
        auto observer = std::make_shared<dvv::MemorySink>();
        logger().addSink(std::make_shared<ChainFailingSink>("chain start", "first"));
        logger().addSink(std::make_shared<ChainFailingSink>("failed: first", "second"));
        logger().addSink(std::make_shared<ChainFailingSink>("failed: second", "third"));
        logger().addSink(observer);
        logger().start();
        DVV_LOG_INFO("chain start {}", round);
        logger().stop();
        const auto lines = observer->lines();
        CHECK(countContaining(lines, "WARN  a log sink failed: first") == 1);
        CHECK(countContaining(lines, "WARN  a log sink failed: second") == 1);
        CHECK(countContaining(lines, "WARN  a log sink failed: third") == 1);
        CHECK(lines.size() == 4);
    }
}

namespace {

// Calls flush() from inside write(), which the logger must answer at once on the log thread.
class FlushingSink final : public dvv::ILogSink {
public:
    void write(const dvv::LogRecord& /*record*/, std::string_view /*line*/) override {
        dvv::Logger::instance().flush();
        returned.store(true);
    }

    std::atomic<bool> returned{false};
};

}  // namespace

TEST_CASE("async: flush called from a sink returns at once") {
    const AsyncScope scope;
    auto sink = std::make_shared<FlushingSink>();
    logger().addSink(sink);
    logger().start();
    DVV_LOG_INFO("calls flush");
    logger().flush();
    CHECK(sink->returned.load());
}

TEST_CASE("async: a text message over the record limit is cut and still appears") {
    const AsyncScope scope;
    auto records = std::make_shared<RecordSink>();
    auto waiting = std::make_shared<WaitingSink>();
    logger().addSink(records);
    logger().addSink(waiting);
    logger().start();
    std::string big;
    while (big.size() < 2 * dvv::detail::LOG_MAX_RECORD_BYTES) {
        big += "日";
    }
    DVV_LOG_TEXT(LogLevel::Info, big);
    DVV_LOG_INFO("s {}", big);
    logger().flush();

    REQUIRE(records->entries.size() == 2);
    constexpr std::size_t BUDGET =
        dvv::detail::LOG_MAX_RECORD_BYTES - dvv::detail::LOG_RECORD_HEADER_BYTES;
    const std::string_view marker = dvv::detail::LOG_CUT_MARKER;
    const std::string& text = records->entries[0].message;
    CHECK(text.size() <= BUDGET);
    CHECK(text.size() > BUDGET - 16);
    CHECK(text.starts_with("日"));
    CHECK(text.ends_with(marker));
    CHECK((text.size() - marker.size()) % 3 == 0);  // cut on a code-point boundary
    const std::string& argument = records->entries[1].message;
    CHECK(argument.size() <= BUDGET);
    CHECK(argument.starts_with("s 日"));
    CHECK(argument.ends_with(marker));
    CHECK((argument.size() - 2 - marker.size()) % 3 == 0);
}

TEST_CASE("async: the file sink has the lines on disk after a pass without flush") {
    const TempDir temp("dvv-base-log-async-pass");
    const AsyncScope scope;
    const auto path = temp.path() / "pass.log";
    auto file = dvv::RotatingFileSink::open(path, 1000000, 1).value();
    auto waiting = std::make_shared<WaitingSink>();
    logger().addSink(file);
    logger().addSink(waiting);
    logger().start();
    constexpr int COUNT = 10;
    for (int i = 0; i < COUNT; ++i) {
        DVV_LOG_INFO("on disk {}", i);
    }
    waiting->waitFor(COUNT);
    // No flush() and no stop(): the file sink's batchEnd() ran before the waiting sink's.
    const std::string text = readFile(path);
    for (int i = 0; i < COUNT; ++i) {
        CHECK(text.find(std::format("INFO  on disk {} [", i)) != std::string::npos);
    }
}
