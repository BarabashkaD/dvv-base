#pragma once

// Small std::format-based logger (spec §11). The Logger and its sinks are process infrastructure:
// like the LifetimeRegistry they must outlive the leak check, so they are not managed objects.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/base/error.h"

namespace dvv {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error, Off };

[[nodiscard]] std::string_view logLevelName(LogLevel level) noexcept;

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::string_view message;
    std::source_location where;
    std::chrono::system_clock::time_point time;
};

// "2026-10-20T12:34:56.789Z INFO  message [file.cpp:42]"
[[nodiscard]] std::string formatLogRecord(const LogRecord& record);

class ILogSink {
public:
    virtual ~ILogSink() = default;
    // Called with the logger's lock held, so an implementation must not call back into the Logger
    // (no logging, addSink, clearSinks or flush from inside write() or flush()), or it deadlocks.
    // line is formatLogRecord(record) without a newline.
    virtual void write(const LogRecord& record, std::string_view line) = 0;
    virtual void flush() {}
};

class StderrSink final : public ILogSink {
public:
    void write(const LogRecord& record, std::string_view line) override;
    void flush() override;
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
// Flushing: records of level Warn or higher are flushed to the file immediately; Info and below
// stay buffered until the buffer fills, Logger::flush() or destruction.
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

class Logger {
public:
    static Logger& instance() noexcept;  // intentionally never destroyed

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    // A null sink is ignored. Sinks are called with the logger's lock held (see ILogSink).
    void addSink(std::shared_ptr<ILogSink> sink);
    void clearSinks();
    void setLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;
    [[nodiscard]] bool enabled(LogLevel level) const noexcept;
    void write(LogLevel level, std::string_view message,
               std::source_location where = std::source_location::current());
    void flush();

private:
    Logger() = default;
    ~Logger() = default;

    std::mutex mutex_;
    std::vector<std::shared_ptr<ILogSink>> sinks_;
    std::atomic<LogLevel> level_{LogLevel::Info};
};

}  // namespace dvv

// The message is only formatted when the level is enabled.
#define DVV_LOG(level, ...)                                                      \
    do {                                                                         \
        if (::dvv::Logger::instance().enabled(level)) {                          \
            ::dvv::Logger::instance().write((level), ::std::format(__VA_ARGS__), \
                                            ::std::source_location::current());  \
        }                                                                        \
    } while (false)

#define DVV_LOG_TRACE(...) DVV_LOG(::dvv::LogLevel::Trace, __VA_ARGS__)
#define DVV_LOG_DEBUG(...) DVV_LOG(::dvv::LogLevel::Debug, __VA_ARGS__)
#define DVV_LOG_INFO(...) DVV_LOG(::dvv::LogLevel::Info, __VA_ARGS__)
#define DVV_LOG_WARN(...) DVV_LOG(::dvv::LogLevel::Warn, __VA_ARGS__)
#define DVV_LOG_ERROR(...) DVV_LOG(::dvv::LogLevel::Error, __VA_ARGS__)
