#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/base/log.h"
#include "log_ring.h"
#include "log_test_support.h"

#if defined(_WIN32)
#include <malloc.h>  // _aligned_malloc, _aligned_free
#endif

// ThreadSanitizer's runtime defines the global allocation functions itself, so replacements do
// not link: under TSan the allocation-counting test and its replacements are left out.
#if defined(__SANITIZE_THREAD__)
#define DVV_TEST_COUNTS_ALLOCATIONS 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DVV_TEST_COUNTS_ALLOCATIONS 0
#endif
#endif
#if !defined(DVV_TEST_COUNTS_ALLOCATIONS)
#define DVV_TEST_COUNTS_ALLOCATIONS 1
#endif

#if DVV_TEST_COUNTS_ALLOCATIONS
// Replacement allocation functions for the whole test binary, so the snapshot test can count the
// allocations of one call. Every form goes to malloc/free (aligned ones to the matching aligned
// functions), so a pointer is always freed by the family that allocated it. They count only on a
// thread whose counting flag is set.
namespace {

constinit thread_local bool counting_allocations = false;
constinit thread_local std::size_t allocation_count = 0;

void* allocateBytes(std::size_t size) noexcept {
    if (counting_allocations) {
        ++allocation_count;
    }
    return std::malloc(size == 0 ? 1 : size);  // NOLINT(*-no-malloc)
}

void* allocateAligned(std::size_t size, std::align_val_t alignment) noexcept {
    if (counting_allocations) {
        ++allocation_count;
    }
    const std::size_t align = std::max(static_cast<std::size_t>(alignment), sizeof(void*));
    const std::size_t rounded = (std::max<std::size_t>(size, 1) + align - 1) / align * align;
#if defined(_WIN32)
    return _aligned_malloc(rounded, align);
#else
    return std::aligned_alloc(align, rounded);  // NOLINT(*-no-malloc)
#endif
}

void freeBytes(void* pointer) noexcept {
    std::free(pointer);  // NOLINT(*-no-malloc)
}

void freeAligned(void* pointer) noexcept {
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);  // NOLINT(*-no-malloc)
#endif
}

void* allocateOrThrow(std::size_t size) {
    void* pointer = allocateBytes(size);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

void* allocateAlignedOrThrow(std::size_t size, std::align_val_t alignment) {
    void* pointer = allocateAligned(size, alignment);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

}  // namespace

void* operator new(std::size_t size) {
    return allocateOrThrow(size);
}
void* operator new[](std::size_t size) {
    return allocateOrThrow(size);
}
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return allocateBytes(size);
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return allocateBytes(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocateAlignedOrThrow(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocateAlignedOrThrow(size, alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t& /*tag*/) noexcept {
    return allocateAligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& /*tag*/) noexcept {
    return allocateAligned(size, alignment);
}
void operator delete(void* pointer) noexcept {
    freeBytes(pointer);
}
void operator delete[](void* pointer) noexcept {
    freeBytes(pointer);
}
void operator delete(void* pointer, std::size_t /*size*/) noexcept {
    freeBytes(pointer);
}
void operator delete[](void* pointer, std::size_t /*size*/) noexcept {
    freeBytes(pointer);
}
void operator delete(void* pointer, const std::nothrow_t& /*tag*/) noexcept {
    freeBytes(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t& /*tag*/) noexcept {
    freeBytes(pointer);
}
void operator delete(void* pointer, std::align_val_t /*alignment*/) noexcept {
    freeAligned(pointer);
}
void operator delete[](void* pointer, std::align_val_t /*alignment*/) noexcept {
    freeAligned(pointer);
}
void operator delete(void* pointer, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    freeAligned(pointer);
}
void operator delete[](void* pointer, std::size_t /*size*/,
                       std::align_val_t /*alignment*/) noexcept {
    freeAligned(pointer);
}
void operator delete(void* pointer, std::align_val_t /*alignment*/,
                     const std::nothrow_t& /*tag*/) noexcept {
    freeAligned(pointer);
}
void operator delete[](void* pointer, std::align_val_t /*alignment*/,
                       const std::nothrow_t& /*tag*/) noexcept {
    freeAligned(pointer);
}
#endif  // DVV_TEST_COUNTS_ALLOCATIONS

namespace {

using dvv::LogLevel;
using dvv_test::AsyncScope;

constexpr std::string_view SUFFIX = " (from crash snapshot)";

dvv::Logger& logger() {
    return dvv::Logger::instance();
}

// Blocks every write() until open() is called, so the log thread holds its first record and
// releases nothing.
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

// Opens the gate when a check fails, so stop() does not wait for ever.
struct GateOpener {
    GateSink& sink;
    ~GateOpener() { sink.open(); }
};

void appendTo(void* context, const void* data, std::size_t size) {
    auto& out = *static_cast<std::vector<std::byte>*>(context);
    const auto* bytes = static_cast<const std::byte*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

std::vector<std::byte> snapshot() {
    std::vector<std::byte> out;
    logger().writeCrashSnapshot(&appendTo, &out);
    return out;
}

#if DVV_TEST_COUNTS_ALLOCATIONS
// Copies into storage reserved beforehand: writing allocates nothing.
struct FixedBuffer {
    std::byte* data;
    std::size_t capacity;
    std::size_t size = 0;
    bool overflow = false;
};

void appendToFixed(void* context, const void* data, std::size_t size) {
    auto& out = *static_cast<FixedBuffer*>(context);
    if (size > out.capacity - out.size) {
        out.overflow = true;
        return;
    }
    std::memcpy(out.data + out.size, data, size);
    out.size += size;
}
#endif  // DVV_TEST_COUNTS_ALLOCATIONS

void logMixed() {
    DVV_LOG_INFO("numbers {:>6}|{:x}|{:.3f}", 42, 255U, 3.14159);
    DVV_LOG_WARN("string [{}] [{:>3}]", std::string("text"), std::string_view("q"));
    DVV_LOG_TEXT(LogLevel::Error, "plain text");
    DVV_LOG_INFO("colour {}", dvv_test::Colour::Red);
    DVV_LOG_INFO("mixed {1} {0} {{}} {2} {3} {4}", 'c', true, static_cast<signed char>(-5), -7LL,
                 static_cast<const void*>(nullptr));
}

}  // namespace

TEST_CASE("snapshot: unreleased records decode to the lines the sinks would have written") {
    const AsyncScope scope;
    dvv::detail::resetCrashSnapshotForTest();
    auto sink = std::make_shared<GateSink>();
    logger().addSink(sink);
    logger().start();
    const GateOpener opener{*sink};
    logMixed();

    const std::vector<std::byte> bytes = snapshot();
    REQUIRE(bytes.size() <= dvv::Logger::crashSnapshotCapacity());
    const std::vector<std::string> decoded = dvv::decodeCrashSnapshot(bytes);

    sink->open();
    logger().stop();
    const std::vector<std::string> lines = sink->lines();
    REQUIRE(lines.size() == 5);
    REQUIRE(decoded.size() == 5);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        CHECK(decoded[i] == lines[i] + std::string(SUFFIX));
    }
    CHECK(lines[0].find("INFO  numbers     42|ff|3.142 [test_log_snapshot.cpp:") !=
          std::string::npos);
    CHECK(lines[1].find("WARN  string [text] [  q]") != std::string::npos);
    CHECK(lines[2].find("ERROR plain text") != std::string::npos);
    CHECK(lines[3].find("INFO  colour red") != std::string::npos);
    CHECK(lines[4].find("INFO  mixed true c {} -5 -7 0x0") != std::string::npos);
}

TEST_CASE("snapshot: an unpublished record ends the snapshot") {
    const AsyncScope scope;
    dvv::detail::resetCrashSnapshotForTest();
    auto sink = std::make_shared<GateSink>();
    logger().addSink(sink);
    logger().start();
    const GateOpener opener{*sink};
    DVV_LOG_INFO("first");
    auto& ring = dvv::detail::loggerRingForTest();
    const auto held = ring.reserve(64, true);
    if (!held) {
        FAIL("the ring refused a small reservation");
        return;
    }
    DVV_LOG_INFO("second");
    DVV_LOG_INFO("third");

    const std::vector<std::string> decoded = dvv::decodeCrashSnapshot(snapshot());
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0].find("INFO  first [") != std::string::npos);
    CHECK(decoded[0].ends_with(SUFFIX));

    ring.publishAsPadding(*held);
    sink->open();
    logger().stop();
    CHECK(sink->lines().size() == 3);
}

#if DVV_TEST_COUNTS_ALLOCATIONS
TEST_CASE("snapshot: writing allocates nothing") {
    const AsyncScope scope;
    dvv::detail::resetCrashSnapshotForTest();
    auto sink = std::make_shared<GateSink>();
    logger().addSink(sink);
    logger().start();
    const GateOpener opener{*sink};
    logMixed();
    std::vector<std::byte> storage(dvv::Logger::crashSnapshotCapacity());
    FixedBuffer buffer{storage.data(), storage.size()};

    // The replacement counts: a direct call cannot be optimized away.
    allocation_count = 0;
    counting_allocations = true;
    ::operator delete(::operator new(16));
    counting_allocations = false;
    REQUIRE(allocation_count == 1);

    allocation_count = 0;
    counting_allocations = true;
    logger().writeCrashSnapshot(&appendToFixed, &buffer);
    counting_allocations = false;

    CHECK(allocation_count == 0);
    CHECK_FALSE(buffer.overflow);
    CHECK(dvv::decodeCrashSnapshot(std::span(storage.data(), buffer.size)).size() == 5);
}
#endif  // DVV_TEST_COUNTS_ALLOCATIONS

TEST_CASE("snapshot: a second call returns at once without writing") {
    const AsyncScope scope;
    dvv::detail::resetCrashSnapshotForTest();
    auto sink = std::make_shared<GateSink>();
    logger().addSink(sink);
    logger().start();
    const GateOpener opener{*sink};
    DVV_LOG_INFO("one");
    CHECK_FALSE(snapshot().empty());
    CHECK(snapshot().empty());
}

TEST_CASE("snapshot: truncated and random input never crashes the decoder") {
    const AsyncScope scope;
    dvv::detail::resetCrashSnapshotForTest();
    auto sink = std::make_shared<GateSink>();
    logger().addSink(sink);
    logger().start();
    const GateOpener opener{*sink};
    logMixed();
    const std::vector<std::byte> full = snapshot();
    REQUIRE(dvv::decodeCrashSnapshot(full).size() == 5);

    for (std::size_t length = 0; length < full.size(); ++length) {
        const auto lines = dvv::decodeCrashSnapshot(std::span(full.data(), length));
        CHECK(lines.size() <= 5);
    }

    std::mt19937 random(1);
    std::uniform_int_distribution<int> byte_value(0, 255);
    std::uniform_int_distribution<std::size_t> length_of(0, 512);
    for (int round = 0; round < 1000; ++round) {
        std::vector<std::byte> bytes(length_of(random));
        for (auto& byte : bytes) {
            byte = static_cast<std::byte>(byte_value(random));
        }
        // Half of them pass the magic, so the decoder reads further than the first check.
        if (round % 2 == 0 && bytes.size() >= 8) {
            std::memcpy(bytes.data(), "DVVLOGS1", 8);
        }
        static_cast<void>(dvv::decodeCrashSnapshot(bytes));
    }
}

namespace {

template <class T>
void put(std::vector<std::byte>& out, const T& value) {
    appendTo(&out, &value, sizeof(T));
}

void putText(std::vector<std::byte>& out, std::string_view text) {
    appendTo(&out, text.data(), text.size());
}

struct HandBuilt {
    std::size_t records = 1;
    std::array<std::uint64_t, 5> dropped{};  // Trace to Error
    std::chrono::system_clock::duration time{};
};

// A hand-built snapshot: `records` Arguments records (an int 7 and the string "ab") naming site
// `site`, and a site table holding one site with `format`.
std::vector<std::byte> handBuilt(std::string_view format, std::uint32_t site,
                                 const HandBuilt& options = {}) {
    std::vector<std::byte> out;
    putText(out, "DVVLOGS1");
    for (const std::uint64_t count : options.dropped) {
        put(out, count);
    }
    std::vector<std::byte> body;
    put(body, static_cast<std::uint8_t>(dvv::detail::LogArgTag::I32));
    put(body, std::int32_t{7});
    put(body, static_cast<std::uint8_t>(dvv::detail::LogArgTag::String));
    put(body, std::uint32_t{2});
    putText(body, "ab");
    for (std::size_t record = 0; record < options.records; ++record) {
        put(out, static_cast<std::uint32_t>(body.size()));
        put(out, std::array<std::uint8_t, 4>{0, static_cast<std::uint8_t>(LogLevel::Info), 0, 2});
        put(out, site);
        put(out, static_cast<std::int64_t>(options.time.count()));
        out.insert(out.end(), body.begin(), body.end());
    }
    put(out, std::uint32_t{0xFFFFFFFF});
    put(out, std::uint32_t{1});
    put(out, std::array<std::uint32_t, 3>{12, static_cast<std::uint32_t>(format.size()), 5});
    putText(out, format);
    putText(out, "a.cpp");
    return out;
}

}  // namespace

TEST_CASE("snapshot: a record the decoder cannot format gives its format text and values") {
    const std::string_view prefix = "1970-01-01T00:00:00.000Z INFO  ";
    CHECK(dvv::decodeCrashSnapshot(handBuilt("{:>3}{}", 0)) ==
          std::vector<std::string>{std::format("{}  7ab [a.cpp:12]{}", prefix, SUFFIX)});
    CHECK(dvv::decodeCrashSnapshot(handBuilt("{1}{0}", 0)) ==
          std::vector<std::string>{std::format("{}ab7 [a.cpp:12]{}", prefix, SUFFIX)});
    // A nested field, an index out of range and a spec that does not suit the value.
    CHECK(
        dvv::decodeCrashSnapshot(handBuilt("{:{}}", 0)) ==
        std::vector<std::string>{std::format("{}{{:{{}}}} | 7, ab [a.cpp:12]{}", prefix, SUFFIX)});
    CHECK(dvv::decodeCrashSnapshot(handBuilt("{} {} {}", 0)) ==
          std::vector<std::string>{
              std::format("{}{{}} {{}} {{}} | 7, ab [a.cpp:12]{}", prefix, SUFFIX)});
    CHECK(dvv::decodeCrashSnapshot(handBuilt("{:s}", 0)) ==
          std::vector<std::string>{std::format("{}{{:s}} | 7, ab [a.cpp:12]{}", prefix, SUFFIX)});
    // A site beyond the table.
    CHECK(dvv::decodeCrashSnapshot(handBuilt("{}", 1)) ==
          std::vector<std::string>{
              std::format("{}<unknown call site> | 7, ab [?:0]{}", prefix, SUFFIX)});
}

TEST_CASE("snapshot: dropped counts decode to the log thread's line after the records") {
    HandBuilt options;
    options.dropped = {0, 0, 3, 1, 0};
    options.time =
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::minutes{1});
    const std::vector<std::string> lines = dvv::decodeCrashSnapshot(handBuilt("{} {}", 0, options));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "1970-01-01T00:01:00.000Z INFO  7 ab [a.cpp:12] (from crash snapshot)");
    // The last record's time; the log thread's location is not in the snapshot.
    CHECK(lines[1] ==
          "1970-01-01T00:01:00.000Z WARN  4 log messages dropped (1 warn, 3 info) [?:0] (from "
          "crash snapshot)");
}

TEST_CASE("snapshot: Logger::write with run-time text and a run-time location") {
    const AsyncScope scope;
    dvv::detail::resetCrashSnapshotForTest();
    auto sink = std::make_shared<GateSink>();
    logger().addSink(sink);
    logger().start();
    const GateOpener opener{*sink};
    const std::source_location where = std::source_location::current();
    const std::string text = std::format("run-time text {}", where.line());
    logger().write(LogLevel::Warn, text, where);

    const std::vector<std::string> decoded = dvv::decodeCrashSnapshot(snapshot());
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0].ends_with(std::format("WARN  run-time text {} [test_log_snapshot.cpp:{}]{}",
                                           where.line(), where.line(), SUFFIX)));
}

TEST_CASE("snapshot: a hostile snapshot cannot make the decoder's output explode") {
    // 512 fields of width 9999: about 5 MB per message, 10 GB for all records if uncapped.
    std::string format;
    while (format.size() + 8 <= dvv::detail::LOG_SNAPSHOT_MAX_STRING) {
        format += "{0:9999}";
    }
    HandBuilt options;
    options.records = 2000;
    const std::vector<std::string> lines = dvv::decodeCrashSnapshot(handBuilt(format, 0, options));
    std::size_t total = 0;
    for (const std::string& line : lines) {
        total += line.size();
    }
    CHECK(total <= 4 * dvv::Logger::crashSnapshotCapacity());
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.size() < options.records);
    // Over the message cap: the format text and the values.
    CHECK(lines[0].find(format + " | 7, ab [a.cpp:12]") != std::string::npos);
    CHECK(lines.back() == std::format("1970-01-01T00:00:00.000Z WARN  decode stopped: output limit "
                                      "reached [?:0]{}",
                                      SUFFIX));

    // A format longer than the writer stores ends the site table: the record's site is unknown.
    const std::vector<std::string> too_long = dvv::decodeCrashSnapshot(
        handBuilt(std::string(dvv::detail::LOG_SNAPSHOT_MAX_STRING + 1, 'x'), 0));
    REQUIRE(too_long.size() == 1);
    CHECK(too_long[0].find("<unknown call site> | 7, ab [?:0]") != std::string::npos);
}

TEST_CASE("snapshot: a large legitimate snapshot decodes up to the output limit") {
    // Short records with a long format: the text is many times the snapshot's size.
    const std::string format = std::string(1000, 'x') + " {} {}";
    const std::size_t limit = 4 * dvv::Logger::crashSnapshotCapacity();
    const std::vector<std::string> one = dvv::decodeCrashSnapshot(handBuilt(format, 0));
    REQUIRE(one.size() == 1);
    const std::size_t line_size = one[0].size();
    const auto total_of = [](const std::vector<std::string>& lines) {
        std::size_t total = 0;
        for (const std::string& line : lines) {
            total += line.size();
        }
        return total;
    };

    SUBCASE("just under the limit: every record decodes") {
        HandBuilt options;
        options.records = (limit - 4096) / line_size;
        const std::vector<std::string> lines =
            dvv::decodeCrashSnapshot(handBuilt(format, 0, options));
        REQUIRE(lines.size() == options.records);
        CHECK(lines.back() == one[0]);
        CHECK(total_of(lines) <= limit);
    }

    SUBCASE("over the limit: the decode ends with a line that says so") {
        HandBuilt options;
        options.records = (limit / line_size) + 1;
        options.dropped = {0, 0, 2, 0, 0};
        const std::vector<std::string> lines =
            dvv::decodeCrashSnapshot(handBuilt(format, 0, options));
        REQUIRE(lines.size() >= 3);
        CHECK(lines.size() < options.records + 2);
        CHECK(total_of(lines) <= limit);
        CHECK(lines[lines.size() - 3] == one[0]);
        CHECK(lines[lines.size() - 2].find("WARN  2 log messages dropped (2 info) [?:0]") !=
              std::string::npos);
        CHECK(lines.back() == std::format("1970-01-01T00:00:00.000Z WARN  decode stopped: output "
                                          "limit reached [?:0]{}",
                                          SUFFIX));
    }
}

TEST_CASE("snapshot: after stop it holds no records") {
    const AsyncScope scope;
    dvv::detail::resetCrashSnapshotForTest();
    auto sink = std::make_shared<dvv::MemorySink>();
    logger().addSink(sink);
    logger().start();
    DVV_LOG_INFO("handled");
    logger().stop();
    const std::vector<std::byte> bytes = snapshot();
    REQUIRE(bytes.size() >= 8);
    CHECK(std::memcmp(bytes.data(), "DVVLOGS1", 8) == 0);
    CHECK(dvv::decodeCrashSnapshot(bytes).empty());
    CHECK(sink->lines().size() == 1);
}
