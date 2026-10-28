#include "dvv/base/log.h"

#include <cstdio>
#include <system_error>

namespace dvv {

std::string_view logLevelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off: return "OFF";
    }
    return "?";
}

std::string formatLogRecord(const LogRecord& record) {
    const auto time = std::chrono::time_point_cast<std::chrono::milliseconds>(record.time);
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z {:<5} {} [{}:{}]", time, logLevelName(record.level),
                       record.message, sourceFileName(record.where), record.where.line());
}

void StderrSink::write(const LogRecord& /*record*/, std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
}

void StderrSink::flush() {
    std::fflush(stderr);
}

void MemorySink::write(const LogRecord& /*record*/, std::string_view line) {
    std::lock_guard lock(mutex_);
    lines_.emplace_back(line);
}

std::vector<std::string> MemorySink::lines() const {
    std::lock_guard lock(mutex_);
    return lines_;
}

Result<std::shared_ptr<RotatingFileSink>> RotatingFileSink::open(std::filesystem::path path,
                                                                 std::uintmax_t max_bytes,
                                                                 int max_files) {
    if (max_bytes == 0 || max_files < 1) {
        return makeError(Errc::Internal, "rotating log needs max_bytes > 0 and max_files >= 1");
    }
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return makeError(Errc::Io,
                             std::format("cannot create log directory: {}", error.message()));
        }
    }
    std::uintmax_t size =
        std::filesystem::exists(path, error) ? std::filesystem::file_size(path, error) : 0;
    if (error) {
        size = 0;
    }
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream) {
        return makeError(Errc::Io,
                         std::format("cannot open log file {}", path.filename().string()));
    }
    return std::shared_ptr<RotatingFileSink>(
        new RotatingFileSink(std::move(path), max_bytes, max_files, std::move(stream), size));
}

RotatingFileSink::RotatingFileSink(std::filesystem::path path, std::uintmax_t max_bytes,
                                   int max_files, std::ofstream stream, std::uintmax_t size)
    : path_(std::move(path)),
      max_bytes_(max_bytes),
      max_files_(max_files),
      stream_(std::move(stream)),
      size_(size) {}

void RotatingFileSink::write(const LogRecord& record, std::string_view line) {
    // A previous (re)open or write may have failed; reopen before writing and drop the line if that
    // still fails.
    if (!stream_.good() && !openAppend()) {
        return;
    }
    const std::uintmax_t needed = line.size() + 1;
    if (size_ > 0 && size_ + needed > max_bytes_ && size_ >= next_rotation_at_) {
        rotate();
        if (!stream_.good()) {
            return;
        }
    }
    stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
    stream_.put('\n');
    size_ += needed;
    if (record.level >= LogLevel::Warn) {
        stream_.flush();
    }
}

void RotatingFileSink::flush() {
    stream_.flush();
}

bool RotatingFileSink::openAppend() {
    stream_.close();
    stream_.clear();
    stream_.open(path_, std::ios::binary | std::ios::app);
    if (!stream_.is_open()) {
        return false;
    }
    std::error_code error;
    size_ = std::filesystem::file_size(path_, error);
    if (error) {
        size_ = 0;
    }
    return true;
}

void RotatingFileSink::rotate() {
    stream_.close();
    std::error_code error;
    auto numbered = [this](int index) {
        auto name = path_;
        name += std::format(".{}", index);
        return name;
    };
    // Give up on this rotation: put the staged file back (if it was moved), keep appending to the
    // current file and wait for max_bytes more before trying again.
    auto cancel = [this](const std::filesystem::path* staged) {
        if (staged != nullptr) {
            std::error_code ignored;
            std::filesystem::rename(*staged, path_, ignored);
        }
        openAppend();
        next_rotation_at_ = size_ + max_bytes_;
    };

    auto staged = path_;
    staged += ".rotating";
    // Never touch an existing staging file (it may hold data from a crash).
    if (std::filesystem::exists(staged, error) || error) {
        cancel(nullptr);
        return;
    }
    // Move the current file aside first: it is the step most likely to fail (for example a file
    // locked by another process on Windows).
    std::filesystem::rename(path_, staged, error);
    if (error) {
        cancel(nullptr);
        return;
    }
    // Shift from the oldest slot down. rename() replaces an existing regular file, so the oldest
    // backup is only replaced once the shift into its slot has succeeded; nothing is deleted first.
    for (int index = max_files_ - 1; index >= 1; --index) {
        const bool present = std::filesystem::exists(numbered(index), error);
        if (error) {
            cancel(&staged);
            return;
        }
        if (present) {
            std::filesystem::rename(numbered(index), numbered(index + 1), error);
            if (error) {
                cancel(&staged);
                return;
            }
        }
    }
    std::filesystem::rename(staged, numbered(1), error);
    if (error) {
        cancel(&staged);
        return;
    }
    // Start a fresh file (size 0); if the open fails, write() retries it.
    next_rotation_at_ = 0;
    openAppend();
}

Logger& Logger::instance() noexcept {
    // Intentionally leaked; out of memory at start-up is fatal anyway.
    static auto* logger = new Logger();  // NOLINT(bugprone-unhandled-exception-at-new)
    return *logger;
}

void Logger::addSink(std::shared_ptr<ILogSink> sink) {
    if (!sink) {
        return;
    }
    std::lock_guard lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::clearSinks() {
    std::lock_guard lock(mutex_);
    sinks_.clear();
}

void Logger::setLevel(LogLevel level) noexcept {
    level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::level() const noexcept {
    return level_.load(std::memory_order_relaxed);
}

bool Logger::enabled(LogLevel level) const noexcept {
    return level != LogLevel::Off && level >= this->level();
}

void Logger::write(LogLevel level, std::string_view message, std::source_location where) {
    if (!enabled(level)) {
        return;
    }
    const LogRecord record{level, message, where, std::chrono::system_clock::now()};
    const std::string line = formatLogRecord(record);
    std::lock_guard lock(mutex_);
    for (const auto& sink : sinks_) {
        sink->write(record, line);
    }
}

void Logger::flush() {
    std::lock_guard lock(mutex_);
    for (const auto& sink : sinks_) {
        sink->flush();
    }
}

}  // namespace dvv
