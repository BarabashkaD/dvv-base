#pragma once

// Small std::format-based logger (spec §11). The Logger and its sinks are process infrastructure:
// like the LifetimeRegistry they must outlive the leak check, so they are not managed objects.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/log_encode.h"

namespace dvv {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error, Off };

[[nodiscard]] std::string_view logLevelName(LogLevel level) noexcept;

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::string_view message;
    std::source_location where;
    std::chrono::system_clock::time_point time;
    bool mark = false;  // a marker line from the app (the GUI "Mark" action), not a program message
};

// "2026-10-20T12:34:56.789Z INFO  message [file.cpp:42]"
[[nodiscard]] std::string formatLogRecord(const LogRecord& record);
// with_source false drops the " [file.cpp:42]" suffix; true is the one-argument form.
[[nodiscard]] std::string formatLogRecord(const LogRecord& record, bool with_source);

// Sinks are called one at a time: under the logger's lock in synchronous mode, on the log thread in
// asynchronous mode (Logger::start). An implementation needs a lock only for data it shares with
// other threads, and must not call back into the Logger (no logging, addSink, clearSinks, start,
// stop or flush from inside its functions).
class ILogSink {
public:
    virtual ~ILogSink() = default;
    // line is formatLogRecord(record) without a newline.
    virtual void write(const LogRecord& record, std::string_view line) = 0;
    // Asynchronous mode: after every pass of the log thread that called write().
    virtual void batchEnd() {}
    virtual void flush() {}
};

class StderrSink final : public ILogSink {
public:
    void write(const LogRecord& record, std::string_view line) override;
    void flush() override;
};

// The OS log: unified logging on macOS, syslog on Linux, OutputDebugStringW on Windows. A mark
// (LogRecord::mark) goes at the OS's default level: OS_LOG_TYPE_DEFAULT on macOS, LOG_NOTICE on
// Linux.
class SystemLogSink final : public ILogSink {
public:
    explicit SystemLogSink(std::string identity);
    ~SystemLogSink() override;
    SystemLogSink(const SystemLogSink&) = delete;
    SystemLogSink& operator=(const SystemLogSink&) = delete;
    void write(const LogRecord& record, std::string_view line) override;

private:
    std::string identity_;
#if defined(__APPLE__)
    void* log_ = nullptr;  // os_log_t
#endif
};

// Keeps lines in memory; for tests and the GUI log panel.
class MemorySink final : public ILogSink {
public:
    void write(const LogRecord& record, std::string_view line) override;
    [[nodiscard]] std::vector<std::string> lines() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

// Writes <path>; when it would exceed max_bytes it is renamed to <path>.1 (older files shift up to
// <path>.<max_files>) and a new file is started.
//
// Flushing: records of level Warn or higher and marks are flushed to the file immediately; Info and
// below stay buffered until the buffer fills, Logger::flush() or destruction, and in asynchronous
// mode until the end of the log thread's pass (batchEnd).
//
// Rotation goes through a staging file: <path> is first renamed to <path>.rotating, the backups are
// shifted, and the staging file becomes <path>.1.
//
// Backups are shifted from the oldest slot down by renaming, which replaces an existing file, so
// the oldest backup is replaced only once the shift into its slot has succeeded.
//
// Failure handling: if <path>.rotating already exists it is never touched and no rotation happens.
// If any step of a rotation fails, the rotation is cancelled. A failed rotation may leave some
// backups shifted by one slot, and the oldest backup may then already be replaced. The current log
// is never truncated, overwritten or deleted. Normally the staged file is moved back and appended
// to. If moving it back fails too, its data stays in <path>.rotating, a new <path> is started, and
// rotation stays blocked until <path>.rotating is removed (best effort). After a cancelled rotation
// the next attempt waits until max_bytes more have been written, and a successful rotation clears
// this wait. If the file cannot be (re)opened, the lines are dropped and the open is retried on
// every write.
class RotatingFileSink final : public ILogSink {
public:
    [[nodiscard]] static Result<std::shared_ptr<RotatingFileSink>> open(std::filesystem::path path,
                                                                        std::uintmax_t max_bytes,
                                                                        int max_files);

    void write(const LogRecord& record, std::string_view line) override;
    void batchEnd() override;
    void flush() override;

private:
    RotatingFileSink(std::filesystem::path path, std::uintmax_t max_bytes, int max_files,
                     std::ofstream stream, std::uintmax_t size);
    void rotate();
    // Opens path_ in append mode and takes size_ from the real file size. False if it fails.
    bool openAppend();

    std::filesystem::path path_;
    std::uintmax_t max_bytes_;
    int max_files_;
    std::ofstream stream_;
    std::uintmax_t size_;
    std::uintmax_t next_rotation_at_ = 0;  // no rotation before size_ reaches this
};

// Writes <folder>/<prefix>-YYYYMMDD-HHMMSS[-N].log, named by the local time of its creation. A
// file is created exclusively (it never opens an existing one); a taken name moves on to the
// suffix -2, -3, ... The newest file is found by parsing names strictly as that pattern and
// comparing (timestamp, N), no suffix counting as 1; a name that does not match is never touched.
//
// open(append_newest = true) appends to the newest matching file below max_bytes, else (or when
// that file cannot be opened for appending) creates a new file; open(false) always creates one.
// A write that would pass max_bytes starts a new file first, unless the current file holds no line
// yet (only its header, or nothing), so a line is never divided and a line larger than max_bytes
// gets a file of its own. Never renames or deletes a file.
//
// Flushing: Warn and above and marks at once; Info and below buffered until the buffer fills,
// flush(), batchEnd() in asynchronous mode, or destruction.
//
// Failure handling (notes go to stderr only, never to the Logger): if creating a new file fails,
// the current file stays in use (over the limit) and the next attempt waits until another
// max_bytes / 4 bytes were written. If a write or flush fails, the file is closed; a new file is
// tried at once when the failed one had been started at least max_bytes / 4 bytes before,
// otherwise once that much has been offered since it started. While no file is open the lines are
// dropped. Bytes count from the lines passed to write(), dropped or not.
class SplitFileSink final : public ILogSink {
public:
    // Creates the folder if needed. Fails for max_bytes 0, an empty prefix, or when no file can
    // be opened.
    [[nodiscard]] static Result<std::shared_ptr<SplitFileSink>> open(std::filesystem::path folder,
                                                                     std::string prefix,
                                                                     std::uintmax_t max_bytes,
                                                                     bool append_newest);

    ~SplitFileSink() override;
    SplitFileSink(const SplitFileSink&) = delete;
    SplitFileSink& operator=(const SplitFileSink&) = delete;
    SplitFileSink(SplitFileSink&&) = delete;
    SplitFileSink& operator=(SplitFileSink&&) = delete;

    // The header of each new file: called under the sink's mutex when the sink creates a file,
    // it returns the message of the file's first line, which the sink writes as an Info line
    // (without a source location) and flushes. `previous` is the file the new one continues,
    // empty for the file created by open(). Must not log or call back into this sink. If the
    // file created by open() has no line yet, setHeader writes its header at once; an appended
    // file gets none.
    void setHeader(std::function<std::string(const std::filesystem::path& previous)> make);
    // The file in use, or the last one if a failure closed it. Thread-safe.
    [[nodiscard]] std::filesystem::path currentFile() const;
    // The sizes of the matching files found by open(), plus every byte written since. Thread-safe.
    [[nodiscard]] std::uintmax_t totalBytes() const;
    // The files started since open() (open's own file not counted). Thread-safe.
    [[nodiscard]] std::uint64_t splits() const;

    void write(const LogRecord& record, std::string_view line) override;
    void batchEnd() override;
    void flush() override;

private:
    SplitFileSink(std::filesystem::path folder, std::string prefix, std::uintmax_t max_bytes);
    // Creates a new file and makes it current, writing its header; true when a file is open
    // afterwards. On failure the current file (if any) stays in use and the next attempt waits for
    // max_bytes / 4. mutex_ held.
    bool startFile();
    // Writes make_header_(previous) as a flushed Info line. mutex_ held.
    bool writeHeader(const std::filesystem::path& previous);
    // Writes the line; on failure closes the file and returns false. mutex_ held.
    bool writeLine(std::string_view line, bool flush_now);
    void flushStream();
    // Closes the file after a failed write or flush and sets the wait. mutex_ held.
    void failFile();
    [[nodiscard]] std::uintmax_t backoff() const noexcept;

    mutable std::mutex mutex_;
    std::filesystem::path folder_;
    std::string prefix_;
    std::uintmax_t max_bytes_;
    std::ofstream stream_;
    std::filesystem::path current_;
    std::uintmax_t size_ = 0;             // bytes in the current file
    bool has_lines_ = false;              // the current file holds a line (not only its header)
    bool header_pending_ = false;         // the file open() created has neither header nor line yet
    std::uintmax_t total_ = 0;            // totalBytes()
    std::uint64_t splits_ = 0;            // splits()
    std::uintmax_t offered_ = 0;          // bytes of all lines passed to write()
    std::uintmax_t started_at_ = 0;       // offered_ when the current file was started
    std::uintmax_t next_attempt_at_ = 0;  // no new file before offered_ reaches this
    std::function<std::string(const std::filesystem::path&)> make_header_;
};

namespace detail {

// A call site: its literal format string and location, built once as a static constexpr.
struct LogSite {
    std::string_view format;
    std::source_location where;
    bool bareBraces;

    constexpr LogSite(std::string_view format_text, std::source_location location) noexcept
        : format(format_text), where(location), bareBraces(format_text == "{}") {}
};

// Room kept in a record for its header, taken off the argument budget.
inline constexpr std::size_t LOG_RECORD_HEADER_BYTES = 64;

// What a call hands to Logger::submit: either text already formatted by the caller (format is
// null) or `size` bytes of encoded arguments that encode(out, context) writes and `format` turns
// into text.
struct LogPayload {
    std::string_view text;
    LogFormatFn format = nullptr;
    std::size_t size = 0;
    void (*encode)(std::byte* out, const void* context) = nullptr;
    const void* context = nullptr;
    std::uint8_t count = 0;  // the number of encoded arguments
};

struct LoggerAccess;
struct LoggerInternals;
class LogRing;
struct LogSnapshotBuffer;

// Crash snapshot (Logger::writeCrashSnapshot), all integers in the writing machine's byte order:
//   "DVVLOGS1"                          8 bytes
//   dropped counts, Trace to Error      5 x u64
//   records, each:
//     body size                         u32 (0xFFFFFFFF ends the records instead)
//     kind, level, flags, arg count     4 x u8: kind 0 arguments, 1 text formatted by the
//                                       caller (with its call site), 2 Logger::write text;
//                                       LogLevel; flags bit 0 the bare "{}" warning,
//                                       bit 1 mark (Logger::mark, level Info)
//     site index                        u32 into the site table (0xFFFFFFFF: unknown)
//     time                              i64 std::chrono::system_clock ticks since the epoch
//     body                              encoded arguments (tag, value) or the message text
//   site count                          u32
//   sites, each:
//     line, format size, file size      3 x u32
//     format text, file name            (the file name without its directories)
// A site is a call site (its format and location) or a Logger::write location (empty format).
inline constexpr std::size_t LOG_SNAPSHOT_SITES = 4096;
inline constexpr std::size_t LOG_SNAPSHOT_TEXT_BYTES = std::size_t{1} << 20U;  // all sites' strings
inline constexpr std::size_t LOG_SNAPSHOT_MAX_STRING = 4096;  // one format text or file name
inline constexpr std::size_t LOG_SNAPSHOT_HEADER_BYTES = 8 + 5 * sizeof(std::uint64_t);
inline constexpr std::size_t LOG_SNAPSHOT_RECORD_HEADER_BYTES = 20;
inline constexpr std::size_t LOG_SNAPSHOT_SITE_HEADER_BYTES = 12;
// The records of a snapshot take at most this much (they are smaller than their ring records).
inline constexpr std::size_t LOG_SNAPSHOT_RECORD_BYTES = LOG_RING_BYTES;
inline constexpr std::size_t LOG_SNAPSHOT_CAPACITY =
    LOG_SNAPSHOT_HEADER_BYTES + LOG_SNAPSHOT_RECORD_BYTES + sizeof(std::uint32_t) +
    sizeof(std::uint32_t) + LOG_SNAPSHOT_SITES * LOG_SNAPSHOT_SITE_HEADER_BYTES +
    LOG_SNAPSHOT_TEXT_BYTES;

}  // namespace detail

// Receives a crash snapshot's bytes, in order; called from a crash handler.
using CrashWriter = void (*)(void* context, const void* data, std::size_t size);

class Logger {
public:
    static Logger& instance() noexcept;  // intentionally never destroyed

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    // A null sink is ignored. A sink gets the records at or above its own level. addSink and
    // clearSinks work only in synchronous mode: while the log thread runs they are ignored and log
    // an Error line.
    void addSink(std::shared_ptr<ILogSink> sink, LogLevel level = LogLevel::Trace);
    // Allowed at any time. In asynchronous mode a sink's level is read when the log thread drains
    // the queue, so a change also applies to records queued just before it.
    void setSinkLevel(const ILogSink& sink, LogLevel level);
    void clearSinks();
    void setLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;
    // True when a call at this level can reach a sink: not Off, at or above level() and at or
    // above the lowest sink level. Always false without sinks.
    [[nodiscard]] bool enabled(LogLevel level) const noexcept;
    void write(LogLevel level, std::string_view message,
               std::source_location where = std::source_location::current());
    // Delivers `text` as one record of level Info, flagged as a mark, to every sink whose level is
    // not Off, ignoring the logger's level and the sinks' thresholds. In order with the other
    // records: synchronous, under the same lock; asynchronous, through the queue with the priority
    // of Warn, so a full ring drops it only when it would drop a warning (it is counted as a
    // dropped Info message). Does nothing without sinks. Records the call site like a log macro.
    void mark(std::string_view text, std::source_location where = std::source_location::current());

    // Asynchronous mode: starts the log thread "dvv-log"; from now on a call queues its message
    // and returns, and the log thread formats it and calls the sinks. A second call does nothing.
    void start();
    // Back to synchronous mode: hands every queued message to the sinks, joins the log thread and
    // flushes every sink. Without a log thread it only flushes the sinks. An app that called
    // start() must call stop() (or finishWithLeakCheck) before it exits.
    void stop();
    // Returns once everything logged before the call is in every sink and every sink's flush()
    // has run. Called from a sink it returns at once. Not for crash handlers in asynchronous mode.
    void flush();

    // For a crash handler: writes, through `writer`, the queued records the sinks may not have
    // made durable yet (from the release position up to the first unpublished one), the dropped
    // counts and the call sites those records name (the layout is described above
    // LOG_SNAPSHOT_SITES). Async-signal-safe: no allocation, no lock, no formatting. Only the
    // first call writes; a later or concurrent call returns at once. Without start() it writes
    // no records. dvv::decodeCrashSnapshot turns the bytes back into lines.
    void writeCrashSnapshot(CrashWriter writer, void* context) const noexcept;
    // The most bytes writeCrashSnapshot writes.
    [[nodiscard]] static constexpr std::size_t crashSnapshotCapacity() noexcept {
        return detail::LOG_SNAPSHOT_CAPACITY;
    }

private:
    friend struct detail::LoggerAccess;
    friend struct detail::LoggerInternals;

    struct SinkEntry {
        SinkEntry(std::shared_ptr<ILogSink> entry_sink, LogLevel entry_level)
            : sink(std::move(entry_sink)), level(entry_level) {}

        std::shared_ptr<ILogSink> sink;
        std::atomic<LogLevel> level;  // read by the log thread without a lock
        bool failed = false;          // its first failure was reported (log thread)
    };

    Logger();
    ~Logger();

    // The one entry every DVV_LOG call reaches.
    void submit(LogLevel level, const detail::LogSite& site, std::atomic<bool>& bare_warned,
                const detail::LogPayload& payload);
    // Queues or delivers one message; `site` is null for text with its own location `where`.
    // `flags` are the ring record's flags (log_ring.h): the bare "{}" warning, a mark.
    void route(LogLevel level, const detail::LogSite* site, std::source_location where,
               const detail::LogPayload& payload, std::uint8_t flags);
    // The asynchronous path; false when the logger is synchronous (nothing queued).
    bool tryQueue(LogLevel level, const detail::LogSite* site, std::source_location where,
                  const detail::LogPayload& payload, std::uint8_t flags) noexcept;
    // Encodes one record into the ring; false (and counted as dropped) when it does not fit.
    bool queueRecord(LogLevel level, const detail::LogSite* site, std::source_location where,
                     const detail::LogPayload& payload, std::uint8_t flags) noexcept;
    void wakeLogThread() noexcept;
    void leaveFlight() noexcept;
    // The synchronous path; false when the logger became asynchronous (nothing delivered).
    bool deliverNow(LogLevel level, const detail::LogSite* site, std::source_location where,
                    const detail::LogPayload& payload, std::uint8_t flags);
    // Writes one record, then the bare-"{}" warning of `bare_site` if not null, under mutex_;
    // false when the logger became asynchronous. A mark goes to every sink not at Off.
    bool deliver(LogLevel level, std::string_view message, std::source_location where,
                 const detail::LogSite* bare_site, bool mark);
    // Recomputes gate_ and marks_open_; mutex_ held.
    void updateGate() noexcept;

    // The log thread, and stop() after joining it.
    void run() noexcept;
    void drain() noexcept;
    // Takes a new flush request's target, and completes the request once it is reached.
    void serviceFlush() noexcept;
    void handleRecord(const std::byte* payload, std::uint32_t size) noexcept;
    void writeToSinks(const LogRecord& record, std::string_view line) noexcept;
    void emitLine(LogLevel level, std::string_view message, std::source_location where,
                  std::chrono::system_clock::time_point time, bool mark = false) noexcept;
    void reportDropped() noexcept;
    void reportSinkFailure(SinkEntry& entry, std::string_view what) noexcept;
    void endBatch() noexcept;
    void flushSinksCaught() noexcept;

    std::mutex mutex_;  // the sinks in synchronous mode; stop() holds it until its final drain
    std::vector<std::unique_ptr<SinkEntry>> sinks_;  // changed only in synchronous mode
    std::atomic<LogLevel> level_{LogLevel::Info};
    // max(level_, lowest sink level); Off without sinks. enabled() reads only this.
    std::atomic<LogLevel> gate_{LogLevel::Off};
    // True when a sink's level is not Off; mark() reads only this.
    std::atomic<bool> marks_open_{false};

    // Asynchronous mode. async_ changes only with mutex_ and control_mutex_ held.
    std::mutex control_mutex_;  // serializes start, stop and flush
    bool running_ = false;      // control_mutex_
    std::atomic<bool> async_{false};
    std::atomic<std::uint32_t> in_flight_{0};  // writers inside the asynchronous path
    std::atomic<std::uint32_t> asleep_{0};     // the wake-up word (spec §5.5)
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint64_t> flush_requested_{0};
    std::atomic<std::uint64_t> flush_done_{0};
    // The latest flush ticket the log thread has taken (written by it only; tests wait on it).
    std::atomic<std::uint64_t> flush_seen_{0};
    std::array<std::atomic<std::uint64_t>, 5> dropped_{};  // per level, Trace to Error
    std::unique_ptr<detail::LogRing> ring_;                // allocated by the first start()
    // The crash snapshot's site table and record copy, reserved by the first start(); the first
    // writeCrashSnapshot call claims it.
    std::unique_ptr<detail::LogSnapshotBuffer> snapshot_buffer_;
    mutable std::atomic_flag snapshot_claimed_;
    std::thread thread_;
    // The log thread's (and stop()'s after the join).
    std::uint64_t read_position_ = 0;
    std::uint64_t flush_target_ = 0;  // its ring write position: done once read up to there
    std::string message_buffer_;
    std::string line_buffer_;
};

namespace detail {

struct LoggerAccess {
    static void submit(LogLevel level, const LogSite& site, std::atomic<bool>& bare_warned,
                       const LogPayload& payload) {
        Logger::instance().submit(level, site, bare_warned, payload);
    }
};

template <class... Args>
void logCall(LogLevel level, const LogSite& site, std::atomic<bool>& bare_warned,
             [[maybe_unused]] std::format_string<std::type_identity_t<Args>...> fmt,
             const Args&... args) {
    if constexpr (LOG_ALL_DEFERRABLE<Args...>) {
        constexpr std::size_t BUDGET = LOG_MAX_RECORD_BYTES - LOG_RECORD_HEADER_BYTES;
        const auto encode = [&args...](std::byte* out) noexcept {
            encodeArguments(out, BUDGET, args...);
        };
        static_assert(sizeof...(Args) <= 255, "a log call takes at most 255 arguments");
        LogPayload payload;
        payload.format = &decodeAndFormat<std::remove_cvref_t<Args>...>;
        payload.size = encodedSize(BUDGET, args...);
        payload.count = static_cast<std::uint8_t>(sizeof...(Args));
        payload.context = &encode;
        payload.encode = [](std::byte* out, const void* context) noexcept {
            (*static_cast<const decltype(encode)*>(context))(out);
        };
        LoggerAccess::submit(level, site, bare_warned, payload);
    } else {
        const std::string text = std::vformat(fmt.get(), std::make_format_args(args...));
        LogPayload payload;
        payload.text = text;
        LoggerAccess::submit(level, site, bare_warned, payload);
    }
}

}  // namespace detail

// The lines of a crash snapshot written by Logger::writeCrashSnapshot, each the standard line
// followed by " (from crash snapshot)". Formats from the stored tags and format texts, so it needs
// neither the program nor its symbols. Never trusts its input: a cut-short snapshot gives the
// lines before the cut, a record that does not decode or format gives its format text and values.
// Its work is bounded: a message whose text passes 1 MiB gives the format text and values instead,
// and once about 4 x crashSnapshotCapacity() bytes of text are produced (the lines, plus the text
// of such discarded messages) the decode stops. It then ends with the dropped counts, if any, and
// the line "decode stopped: output limit reached". If an allocation fails it returns the lines
// decoded so far.
[[nodiscard]] std::vector<std::string> decodeCrashSnapshot(
    std::span<const std::byte> snapshot) noexcept;

}  // namespace dvv

// The level gate comes first: a disabled call evaluates none of its arguments. The format must be
// a string literal.
#define DVV_LOG(level, fmt, ...)                                                     \
    do {                                                                             \
        const ::dvv::LogLevel dvv_log_level = (level);                               \
        if (::dvv::Logger::instance().enabled(dvv_log_level)) {                      \
            static constexpr ::dvv::detail::LogSite dvv_log_site{                    \
                fmt, ::std::source_location::current()};                             \
            static constinit ::std::atomic<bool> dvv_log_bare_warned{false};         \
            ::dvv::detail::logCall(dvv_log_level, dvv_log_site, dvv_log_bare_warned, \
                                   fmt __VA_OPT__(, ) __VA_ARGS__);                  \
        }                                                                            \
    } while (false)

// A message that is plain text, not a format.
#define DVV_LOG_TEXT(level, text)                                   \
    do {                                                            \
        const ::dvv::LogLevel dvv_log_level = (level);              \
        if (::dvv::Logger::instance().enabled(dvv_log_level)) {     \
            ::dvv::Logger::instance().write(dvv_log_level, (text)); \
        }                                                           \
    } while (false)

#define DVV_LOG_TRACE(...) DVV_LOG(::dvv::LogLevel::Trace, __VA_ARGS__)
#define DVV_LOG_DEBUG(...) DVV_LOG(::dvv::LogLevel::Debug, __VA_ARGS__)
#define DVV_LOG_INFO(...) DVV_LOG(::dvv::LogLevel::Info, __VA_ARGS__)
#define DVV_LOG_WARN(...) DVV_LOG(::dvv::LogLevel::Warn, __VA_ARGS__)
#define DVV_LOG_ERROR(...) DVV_LOG(::dvv::LogLevel::Error, __VA_ARGS__)
