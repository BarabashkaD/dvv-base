#pragma once

// The queued logger's bounded ring of bytes (spec §5.4): many writers, one reader; the layout of a
// log record in it and the Logger's private hooks. Private to dvv-base.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>

#include "dvv/base/log.h"
#include "dvv/base/log_encode.h"

namespace dvv::detail {

// A published record's commit word is its start position XOR this value. Positions are multiples of
// 8 and the value is odd, so a commit word is never 0: the value release() leaves at every byte.
inline constexpr std::uint64_t LOG_COMMIT_MIX = 0x9E3779B97F4A7C15ULL;
static_assert(LOG_COMMIT_MIX % 8 != 0);

// Positions count every byte ever reserved; a position's slot is `position % bytes()`. Each record
// is 8-byte aligned and never wraps:
//   offset 0   commit word (64-bit, accessed atomically): `position ^ LOG_COMMIT_MIX` once
//              published, else 0 (release() zeroes the bytes it gives back)
//   offset 8   payload size in bytes (32-bit)
//   offset 12  kind (32-bit): a record or padding
//   offset 16  payload, then up to 7 bytes to the next 8-byte boundary
// A padding record at the ring's end with only 8 bytes left is just a commit word.
// A record starts where the previous one ends, so until its writer commits, its commit word is the
// 0 from the release: an earlier lap's bytes (logged text included) never pass for a record.
class LogRing {
public:
    struct Reservation {
        std::byte* payload;
        std::uint64_t position;
        std::uint32_t size;
    };

    struct Record {
        const std::byte* payload;
        std::uint32_t size;
        std::uint64_t position;
        // The position after this record: the next record's position and what to release.
        std::uint64_t next;
    };

    static constexpr std::uint32_t HEADER_BYTES = 16;

    // `bytes` is a power of two of at least 4 KiB (std::invalid_argument otherwise). A record,
    // header included, is at most bytes / 16.
    explicit LogRing(std::size_t bytes = LOG_RING_BYTES);

    // Claims a record for `payload_bytes` with a compare-and-swap on the write position. An urgent
    // record (Warn, Error) may fill the whole ring, others only 3/4 of it. When the record does
    // not fit before the ring's end, the end is published as padding first. nullopt when the ring
    // has no room or the record is over the limit; never waits.
    [[nodiscard]] std::optional<Reservation> reserve(std::uint32_t payload_bytes,
                                                     bool urgent) noexcept;
    // Makes a reserved record visible to the reader (release ordering).
    void publish(const Reservation& reservation) noexcept;
    // Publishes a reserved record as padding, which the reader skips.
    void publishAsPadding(const Reservation& reservation) noexcept;

    // The published record at `position` or after the padding there; nullopt when that record is
    // not published yet. One reader at a time.
    [[nodiscard]] std::optional<Record> read(std::uint64_t position) const noexcept;
    // Zeroes the bytes from the last release up to `position` (both record boundaries the reader
    // reached), then gives them back to the writers (release ordering).
    void release(std::uint64_t position) noexcept;
    // After copying out a record read() returned while the log thread may still run (the crash
    // snapshot): true when the copy is whole, as release() has not zeroed or given back its bytes.
    [[nodiscard]] bool unchanged(const Record& record) const noexcept;
    [[nodiscard]] std::uint64_t releasePosition() const noexcept;
    // The end of everything reserved so far, published or not.
    [[nodiscard]] std::uint64_t writePosition() const noexcept;

    [[nodiscard]] const std::byte* base() const noexcept;
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

private:
    [[nodiscard]] std::uint64_t* wordAt(std::uint64_t position) const noexcept;
    // The end of the record at `position`, which read() has accepted.
    [[nodiscard]] std::uint64_t recordEnd(std::uint64_t position) const noexcept;
    void writeHeader(std::uint64_t position, std::uint32_t size, std::uint32_t kind) noexcept;
    void commit(std::uint64_t position) noexcept;

    std::size_t bytes_;
    std::uint64_t mask_;
    std::uint64_t record_limit_;
    // 64-bit words, so every commit word is aligned for std::atomic_ref.
    std::unique_ptr<std::uint64_t[]> words_;
    std::atomic<std::uint64_t> write_{0};
    std::atomic<std::uint64_t> release_{0};
};

// A log record's payload in the ring: a LogRecordHeader (copied in and out with memcpy), then the
// encoded arguments (Arguments, `argumentCount` of them) or `textSize` bytes of text (SiteText,
// LocationText).
enum class LogRecordKind : std::uint8_t {
    Arguments,     // formatted on the log thread: origin holds the LogSite*, then the LogFormatFn
    SiteText,      // text formatted by the caller: origin holds the LogSite* (its format unused)
    LocationText,  // Logger::write: origin holds the std::source_location
};

// Set in LogRecordHeader::flags: the log thread warns about the site's bare "{}" after the record.
inline constexpr std::uint8_t LOG_RECORD_WARN_BARE = 1;

struct LogRecordHeader {
    LogRecordKind kind = LogRecordKind::Arguments;
    LogLevel level = LogLevel::Info;
    std::uint8_t flags = 0;
    std::uint8_t argumentCount = 0;
    std::uint32_t textSize = 0;
    std::int64_t time = 0;  // std::chrono::system_clock ticks since the epoch
    std::array<std::byte, 24> origin{};
};

static_assert(sizeof(LogRecordHeader) == 40);
static_assert(std::is_trivially_copyable_v<LogRecordHeader>);
static_assert(std::is_trivially_copyable_v<std::source_location>);
static_assert(sizeof(std::source_location) <= sizeof(LogRecordHeader::origin));
static_assert(sizeof(const LogSite*) + sizeof(LogFormatFn) <= sizeof(LogRecordHeader::origin));
static_assert(sizeof(std::chrono::system_clock::rep) == sizeof(std::int64_t));
// The room logCall keeps for the header (encode budget = LOG_MAX_RECORD_BYTES - this).
static_assert(LogRing::HEADER_BYTES + sizeof(LogRecordHeader) <= LOG_RECORD_HEADER_BYTES);

inline constexpr std::size_t LOG_SITE_BYTES = sizeof(const LogSite*);

inline void setOrigin(LogRecordHeader& header, const LogSite* site, LogFormatFn format) noexcept {
    std::memcpy(header.origin.data(), static_cast<const void*>(&site), LOG_SITE_BYTES);
    std::memcpy(header.origin.data() + LOG_SITE_BYTES, static_cast<const void*>(&format),
                sizeof(LogFormatFn));
}

inline void setOrigin(LogRecordHeader& header, const std::source_location& where) noexcept {
    std::memcpy(header.origin.data(), static_cast<const void*>(&where), sizeof(where));
}

[[nodiscard]] inline const LogSite* siteOf(const LogRecordHeader& header) noexcept {
    const LogSite* site = nullptr;
    std::memcpy(static_cast<void*>(&site), header.origin.data(), LOG_SITE_BYTES);
    return site;
}

[[nodiscard]] inline LogFormatFn formatOf(const LogRecordHeader& header) noexcept {
    LogFormatFn format = nullptr;
    std::memcpy(static_cast<void*>(&format), header.origin.data() + LOG_SITE_BYTES,
                sizeof(LogFormatFn));
    return format;
}

[[nodiscard]] inline std::source_location locationOf(const LogRecordHeader& header) noexcept {
    std::source_location where;
    std::memcpy(static_cast<void*>(&where), header.origin.data(), sizeof(where));
    return where;
}

// One entry of the crash snapshot's site table: a call site (key is its LogSite) or a
// Logger::write location (key is its file name pointer, told apart by `line`).
struct LogSnapshotSite {
    const void* key = nullptr;
    bool location = false;
    std::uint32_t line = 0;
    const char* format = nullptr;  // static data
    std::uint32_t formatSize = 0;
    const char* file = nullptr;  // static data, the name without its directories
    std::uint32_t fileSize = 0;
};

// The crash snapshot's working memory, reserved by start() so the snapshot allocates nothing.
struct LogSnapshotBuffer {
    static constexpr std::size_t SLOTS = LOG_SNAPSHOT_SITES * 2;  // a power of two

    std::array<std::byte, LOG_MAX_RECORD_BYTES> record{};  // the copy of one ring record
    std::array<LogSnapshotSite, LOG_SNAPSHOT_SITES> sites{};
    // Open addressing over `sites`: an entry's index + 1, 0 when free.
    std::array<std::uint32_t, SLOTS> slots{};
    std::uint32_t siteCount = 0;
    std::size_t textBytes = 0;  // the strings of `sites`, against LOG_SNAPSHOT_TEXT_BYTES
};

static_assert((LogSnapshotBuffer::SLOTS & (LogSnapshotBuffer::SLOTS - 1)) == 0);
static_assert(LOG_SNAPSHOT_RECORD_HEADER_BYTES <= sizeof(LogRecordHeader));

// The standard line of formatLogRecord from its parts, appended to `out`.
void appendStandardLine(std::chrono::sys_time<std::chrono::milliseconds> time, LogLevel level,
                        std::string_view message, std::string_view file, std::uint_least32_t line,
                        std::string& out);
// The text of the warning about a call site that formats only "{}".
[[nodiscard]] std::string bareWarningText(std::string_view file, std::uint_least32_t line);
// The log thread's "N log messages dropped (E error, ...)" text from the counts Trace to Error.
[[nodiscard]] std::string droppedText(const std::array<std::uint64_t, 5>& dropped);

// Private access to the Logger for tests and the crash snapshot.
struct LoggerInternals {
    static void resetSnapshot(const Logger& logger) noexcept { logger.snapshot_claimed_.clear(); }
    [[nodiscard]] static LogRing* ring(const Logger& logger) noexcept { return logger.ring_.get(); }
    [[nodiscard]] static std::uint64_t dropped(const Logger& logger, LogLevel level) noexcept {
        const auto index = static_cast<std::size_t>(level);
        return index < logger.dropped_.size() ? logger.dropped_[index].load() : 0;
    }
    [[nodiscard]] static std::uint64_t flushRequests(const Logger& logger) noexcept {
        return logger.flush_requested_.load();
    }
    static void waitFlushSeen(const Logger& logger, std::uint64_t ticket) noexcept {
        for (std::uint64_t seen = logger.flush_seen_.load(); seen < ticket;
             seen = logger.flush_seen_.load()) {
            logger.flush_seen_.wait(seen);
        }
    }
};

// Test hooks: the Logger's ring (valid after start()) and its dropped count of a level, not yet
// reported by the log thread.
inline LogRing& loggerRingForTest() {
    return *LoggerInternals::ring(Logger::instance());
}

inline std::uint64_t droppedForTest(LogLevel level) {
    return LoggerInternals::dropped(Logger::instance(), level);
}

// Test hooks: the number of flush requests so far, and a wait until the log thread has taken
// request number `ticket` (and fixed its target) while asynchronous.
inline std::uint64_t flushRequestsForTest() {
    return LoggerInternals::flushRequests(Logger::instance());
}

inline void waitFlushSeenForTest(std::uint64_t ticket) {
    LoggerInternals::waitFlushSeen(Logger::instance(), ticket);
}

// Test hook: lets the next writeCrashSnapshot call write again.
inline void resetCrashSnapshotForTest() {
    LoggerInternals::resetSnapshot(Logger::instance());
}

}  // namespace dvv::detail
