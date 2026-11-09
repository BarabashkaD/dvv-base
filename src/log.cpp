#include "dvv/base/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <optional>
#include <system_error>

#include "log_ring.h"

#if defined(_WIN32)
// windows.h must come first.
// clang-format off
#include <windows.h>
// clang-format on
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

namespace dvv {

namespace detail {

void appendStandardLine(std::chrono::sys_time<std::chrono::milliseconds> time, LogLevel level,
                        std::string_view message, std::string_view file, std::uint_least32_t line,
                        std::string& out) {
    std::format_to(std::back_inserter(out), "{:%Y-%m-%dT%H:%M:%S}Z {:<5} {} [{}:{}]", time,
                   logLevelName(level), message, file, line);
}

std::string bareWarningText(std::string_view file, std::uint_least32_t line) {
    return std::format(
        "log call at {}:{} formats only \"{{}}\": use DVV_LOG_TEXT or pass the arguments", file,
        line);
}

std::string droppedText(const std::array<std::uint64_t, 5>& dropped) {
    static constexpr std::array<std::string_view, 5> NAMES{"trace", "debug", "info", "warn",
                                                           "error"};
    std::uint64_t total = 0;
    for (const std::uint64_t count : dropped) {
        total += count;
    }
    std::string text = std::format("{} log messages dropped (", total);
    bool first = true;
    for (std::size_t index = dropped.size(); index-- > 0;) {
        if (dropped[index] != 0) {
            std::format_to(std::back_inserter(text), "{}{} {}", first ? "" : ", ", dropped[index],
                           NAMES[index]);
            first = false;
        }
    }
    text += ')';
    return text;
}

}  // namespace detail

namespace {

// formatLogRecord into `out`; the log thread keeps `out` between records.
void appendLogLine(const LogRecord& record, std::string& out) {
    detail::appendStandardLine(std::chrono::time_point_cast<std::chrono::milliseconds>(record.time),
                               record.level, record.message, sourceFileName(record.where),
                               record.where.line(), out);
}

}  // namespace

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
    std::string line;
    appendLogLine(record, line);
    return line;
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

void RotatingFileSink::batchEnd() {
    stream_.flush();
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

namespace {

// True on the log thread, and on the thread of stop() while it hands the last records to the sinks:
// start, stop and flush then return at once instead of waiting for themselves.
thread_local bool in_log_drain = false;

class DrainScope {
public:
    DrainScope() noexcept { in_log_drain = true; }
    ~DrainScope() { in_log_drain = false; }
    DrainScope(const DrainScope&) = delete;
    DrainScope& operator=(const DrainScope&) = delete;
    DrainScope(DrainScope&&) = delete;
    DrainScope& operator=(DrainScope&&) = delete;
};

void nameLogThread() noexcept {
#if defined(_WIN32)
    SetThreadDescription(GetCurrentThread(), L"dvv-log");
#elif defined(__APPLE__)
    pthread_setname_np("dvv-log");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "dvv-log");
#endif
}

std::string bareWarningText(const detail::LogSite& site) {
    return detail::bareWarningText(sourceFileName(site.where), site.where.line());
}

constexpr std::size_t levelIndex(LogLevel level) noexcept {
    return std::min(static_cast<std::size_t>(level), static_cast<std::size_t>(LogLevel::Error));
}

// The text a record may hold: the same budget as the encoded arguments.
constexpr std::size_t TEXT_BUDGET = detail::LOG_MAX_RECORD_BYTES - detail::LOG_RECORD_HEADER_BYTES;

}  // namespace

Logger& Logger::instance() noexcept {
    // Intentionally leaked; out of memory at start-up is fatal anyway.
    static auto* logger = new Logger();  // NOLINT(bugprone-unhandled-exception-at-new)
    return *logger;
}

Logger::Logger() = default;

Logger::~Logger() = default;

void Logger::addSink(std::shared_ptr<ILogSink> sink, LogLevel level) {
    if (!sink) {
        return;
    }
    {
        const std::lock_guard lock(mutex_);
        if (!async_.load(std::memory_order_relaxed)) {
            sinks_.push_back(std::make_unique<SinkEntry>(std::move(sink), level));
            updateGate();
            return;
        }
    }
    write(LogLevel::Error, "Logger::addSink ignored: the log thread is running");
}

void Logger::setSinkLevel(const ILogSink& sink, LogLevel level) {
    const std::lock_guard lock(mutex_);
    for (const auto& entry : sinks_) {
        if (entry->sink.get() == &sink) {
            entry->level.store(level, std::memory_order_relaxed);
        }
    }
    updateGate();
}

void Logger::clearSinks() {
    {
        const std::lock_guard lock(mutex_);
        if (!async_.load(std::memory_order_relaxed)) {
            sinks_.clear();
            updateGate();
            return;
        }
    }
    write(LogLevel::Error, "Logger::clearSinks ignored: the log thread is running");
}

void Logger::setLevel(LogLevel level) noexcept {
    std::lock_guard lock(mutex_);
    level_.store(level, std::memory_order_relaxed);
    updateGate();
}

LogLevel Logger::level() const noexcept {
    return level_.load(std::memory_order_relaxed);
}

bool Logger::enabled(LogLevel level) const noexcept {
    return level != LogLevel::Off && level >= gate_.load(std::memory_order_relaxed);
}

void Logger::updateGate() noexcept {
    auto lowest = LogLevel::Off;
    for (const auto& entry : sinks_) {
        lowest = std::min(lowest, entry->level.load(std::memory_order_relaxed));
    }
    gate_.store(std::max(level(), lowest), std::memory_order_relaxed);
}

void Logger::write(LogLevel level, std::string_view message, std::source_location where) {
    if (!enabled(level)) {
        return;
    }
    detail::LogPayload payload;
    payload.text = message;
    route(level, nullptr, where, payload, false);
}

void Logger::submit(LogLevel level, const detail::LogSite& site, std::atomic<bool>& bare_warned,
                    const detail::LogPayload& payload) {
    const bool warn_bare =
        site.bareBraces && !bare_warned.exchange(true, std::memory_order_relaxed);
    route(level, &site, site.where, payload, warn_bare);
}

void Logger::route(LogLevel level, const detail::LogSite* site, std::source_location where,
                   const detail::LogPayload& payload, bool warn_bare) {
    // start() and stop() switch the mode under mutex_; each path re-checks it and hands over to
    // the other when it changed meanwhile.
    for (;;) {
        if (async_.load(std::memory_order_relaxed) &&
            tryQueue(level, site, where, payload, warn_bare)) {
            return;
        }
        if (deliverNow(level, site, where, payload, warn_bare)) {
            return;
        }
    }
}

bool Logger::tryQueue(LogLevel level, const detail::LogSite* site, std::source_location where,
                      const detail::LogPayload& payload, bool warn_bare) noexcept {
    // Counted before the mode is read (both seq_cst, as stop() stores the mode and then reads the
    // count): stop() either sees this writer or this writer sees synchronous mode.
    in_flight_.fetch_add(1);
    if (!async_.load()) {
        leaveFlight();
        return false;
    }
    if (queueRecord(level, site, where, payload, warn_bare)) {
        // §5.5: the record's commit word (a release store), then the exchange on asleep_. The
        // fence pairs with the log thread's fence between its store of asleep_ and its re-check.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        wakeLogThread();
    }
    leaveFlight();
    return true;
}

bool Logger::queueRecord(LogLevel level, const detail::LogSite* site, std::source_location where,
                         const detail::LogPayload& payload, bool warn_bare) noexcept {
    using detail::LogRecordKind;
    detail::LogRecordHeader header;
    header.level = level;
    header.flags = warn_bare ? detail::LOG_RECORD_WARN_BARE : 0;
    header.time = std::chrono::system_clock::now().time_since_epoch().count();
    std::string_view text = payload.text;
    bool cut = false;
    std::size_t body = 0;
    if (payload.format != nullptr) {
        header.kind = LogRecordKind::Arguments;
        header.argumentCount = payload.count;
        detail::setOrigin(header, site, payload.format);
        body = payload.size;
    } else {
        if (site != nullptr) {
            header.kind = LogRecordKind::SiteText;
            detail::setOrigin(header, site, nullptr);
        } else {
            header.kind = LogRecordKind::LocationText;
            detail::setOrigin(header, where);
        }
        if (text.size() > TEXT_BUDGET) {
            // Not substr(): it can throw, and this function is noexcept.
            text.remove_suffix(
                text.size() -
                detail::utf8CutPoint(text, TEXT_BUDGET - detail::LOG_CUT_MARKER.size()));
            cut = true;
        }
        body = text.size() + (cut ? detail::LOG_CUT_MARKER.size() : 0);
        header.textSize = static_cast<std::uint32_t>(body);
    }
    const std::size_t payload_bytes = sizeof(header) + body;
    std::optional<detail::LogRing::Reservation> reservation;
    if (payload_bytes <= detail::LOG_MAX_RECORD_BYTES) {
        reservation =
            ring_->reserve(static_cast<std::uint32_t>(payload_bytes), level >= LogLevel::Warn);
    }
    if (!reservation) {
        dropped_[levelIndex(level)].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::byte* out = reservation->payload;
    std::memcpy(out, &header, sizeof(header));
    out += sizeof(header);
    if (payload.format != nullptr) {
        payload.encode(out, payload.context);
    } else {
        std::memcpy(out, text.data(), text.size());
        if (cut) {
            std::memcpy(out + text.size(), detail::LOG_CUT_MARKER.data(),
                        detail::LOG_CUT_MARKER.size());
        }
    }
    ring_->publish(*reservation);
    return true;
}

void Logger::wakeLogThread() noexcept {
    if (asleep_.exchange(0) == 1) {
        asleep_.notify_one();
    }
}

void Logger::leaveFlight() noexcept {
    // Only stop() waits for the count, and only after it stored async_ = false (seq_cst) and then
    // loaded the count: a writer that still reads true here decremented before that load, so the
    // common path makes no notify call.
    if (in_flight_.fetch_sub(1) == 1 && !async_.load()) {
        in_flight_.notify_all();
    }
}

bool Logger::deliverNow(LogLevel level, const detail::LogSite* site, std::source_location where,
                        const detail::LogPayload& payload, bool warn_bare) {
    const detail::LogSite* bare_site = warn_bare ? site : nullptr;
    if (payload.format == nullptr) {
        return deliver(level, payload.text, where, bare_site);
    }
    // Never the ring: a per-thread scratch buffer sized from the encoder's own result.
    thread_local std::vector<std::byte> scratch;
    scratch.resize(payload.size);
    payload.encode(scratch.data(), payload.context);
    std::string text;
    payload.format(scratch.data(), site->format, text);
    return deliver(level, text, where, bare_site);
}

bool Logger::deliver(LogLevel level, std::string_view message, std::source_location where,
                     const detail::LogSite* bare_site) {
    const LogRecord record{level, message, where, std::chrono::system_clock::now()};
    const std::string line = formatLogRecord(record);
    std::string warning_text;
    if (bare_site != nullptr) {
        warning_text = bareWarningText(*bare_site);
    }
    const LogRecord warning{LogLevel::Warn, warning_text,
                            bare_site != nullptr ? bare_site->where : where, record.time};
    const std::string warning_line = bare_site != nullptr ? formatLogRecord(warning) : "";
    const std::lock_guard lock(mutex_);
    if (async_.load(std::memory_order_relaxed)) {
        return false;  // start() switched the mode meanwhile: the log thread owns the sinks
    }
    for (const auto& entry : sinks_) {
        if (level >= entry->level.load(std::memory_order_relaxed)) {
            entry->sink->write(record, line);
        }
    }
    if (bare_site != nullptr) {
        for (const auto& entry : sinks_) {
            if (LogLevel::Warn >= entry->level.load(std::memory_order_relaxed)) {
                entry->sink->write(warning, warning_line);
            }
        }
    }
    return true;
}

void Logger::start() {
    if (in_log_drain) {
        return;
    }
    const std::lock_guard control(control_mutex_);
    if (running_) {
        return;
    }
    if (!snapshot_buffer_) {
        snapshot_buffer_ = std::make_unique<detail::LogSnapshotBuffer>();
    }
    if (!ring_) {
        ring_ = std::make_unique<detail::LogRing>();
    }
    const std::lock_guard lock(mutex_);
    stop_requested_.store(false);
    thread_ = std::thread([this] { run(); });
    async_.store(true);
    running_ = true;
}

void Logger::stop() {
    if (in_log_drain) {
        return;
    }
    const std::lock_guard control(control_mutex_);
    // Held until the sinks are flushed: synchronous writers wait behind it, so no sink runs on two
    // threads at once and one thread's lines keep their order.
    const std::lock_guard lock(mutex_);
    if (running_) {
        async_.store(false);
        for (std::uint32_t count = in_flight_.load(); count != 0; count = in_flight_.load()) {
            in_flight_.wait(count);
        }
        stop_requested_.store(true);
        wakeLogThread();
        thread_.join();
        running_ = false;
        // Normally nothing is left: the thread handled every record published before the stop
        // request. This catches what it could not, and reports drops no later record carried.
        const DrainScope scope;
        drain();
        // A sink that fails in the drain's batchEnd() queues its report after the drain's last
        // read: drain again for it. Each sink reports once, so this ends.
        while (ring_->read(read_position_)) {
            drain();
        }
        reportDropped();
    }
    for (const auto& entry : sinks_) {
        entry->sink->flush();
    }
}

void Logger::flush() {
    if (in_log_drain) {
        return;
    }
    const std::lock_guard control(control_mutex_);
    if (running_) {
        const std::uint64_t ticket = flush_requested_.fetch_add(1) + 1;
        wakeLogThread();
        for (std::uint64_t done = flush_done_.load(); done < ticket; done = flush_done_.load()) {
            flush_done_.wait(done);
        }
        return;
    }
    const std::lock_guard lock(mutex_);
    for (const auto& entry : sinks_) {
        entry->sink->flush();
    }
}

void Logger::run() noexcept {
    in_log_drain = true;
    nameLogThread();
    for (;;) {
        // Read before the drain: every record published before the request is handled first.
        const bool stopping = stop_requested_.load();
        serviceFlush();
        drain();
        serviceFlush();
        if (stopping) {
            return;
        }
        // §5.5: announce the sleep, then check once more for a record or a new request. A flush
        // still waiting for a record below its target is not work: that record's writer wakes the
        // thread when it publishes.
        asleep_.store(1);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (ring_->read(read_position_) || stop_requested_.load() ||
            flush_requested_.load() != flush_seen_.load(std::memory_order_relaxed)) {
            asleep_.store(0);
            continue;
        }
        asleep_.wait(1);
    }
}

void Logger::drain() noexcept {
    detail::LogRing& ring = *ring_;
    // A pass ends at the first unpublished record, or after a quarter of the ring so a steady
    // stream of writers still gets its bytes back.
    const std::uint64_t pass_bytes = ring.bytes() / 4;
    std::optional<detail::LogRing::Record> record = ring.read(read_position_);
    while (record) {
        const std::uint64_t pass_end = record->position + pass_bytes;
        for (;;) {
            reportDropped();
            handleRecord(record->payload, record->size);
            read_position_ = record->next;
            record = ring.read(read_position_);
            if (!record || read_position_ >= pass_end) {
                break;
            }
        }
        endBatch();
        // After batchEnd(): a crash snapshot still covers what a sink may not have made durable.
        ring.release(read_position_);
        // Under a steady stream the drain may not end soon; a flush need not wait for it.
        serviceFlush();
    }
}

void Logger::serviceFlush() noexcept {
    // The ticket first: a record reserved before flush() was called is then within the target.
    const std::uint64_t ticket = flush_requested_.load();
    std::uint64_t seen = flush_seen_.load(std::memory_order_relaxed);
    if (ticket != seen) {
        seen = ticket;
        flush_target_ = ring_->writePosition();
        // Once per flush request, so the notify costs nothing on the logging path.
        flush_seen_.store(seen);
        flush_seen_.notify_all();
    }
    if (seen == flush_done_.load(std::memory_order_relaxed) || read_position_ < flush_target_) {
        return;
    }
    reportDropped();
    flushSinksCaught();
    flush_done_.store(seen);
    flush_done_.notify_all();
}

void Logger::handleRecord(const std::byte* payload, std::uint32_t size) noexcept {
    using detail::LogRecordKind;
    detail::LogRecordHeader header;
    if (size < sizeof(header)) {
        return;
    }
    std::memcpy(&header, payload, sizeof(header));
    const std::byte* body = payload + sizeof(header);
    const std::size_t body_size = size - sizeof(header);
    if (header.level >= LogLevel::Off || header.kind > LogRecordKind::LocationText ||
        (header.kind != LogRecordKind::Arguments && header.textSize > body_size)) {
        return;
    }
    const detail::LogSite* site =
        header.kind == LogRecordKind::LocationText ? nullptr : detail::siteOf(header);
    if (header.kind != LogRecordKind::LocationText && site == nullptr) {
        return;
    }
    const std::source_location where = site != nullptr ? site->where : detail::locationOf(header);
    const std::chrono::system_clock::time_point time{
        std::chrono::system_clock::duration{header.time}};
    const bool accepted = std::ranges::any_of(sinks_, [&](const auto& entry) {
        return header.level >= entry->level.load(std::memory_order_relaxed);
    });
    if (accepted) {
        try {
            std::string_view message;
            if (header.kind == LogRecordKind::Arguments) {
                message_buffer_.clear();
                detail::formatOf(header)(body, site->format, message_buffer_);
                message = message_buffer_;
            } else {
                message = {reinterpret_cast<const char*>(body), header.textSize};
            }
            emitLine(header.level, message, where, time);
        } catch (...) {
            // Formatting failed (out of memory): the message is lost and counted.
            dropped_[levelIndex(header.level)].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if ((header.flags & detail::LOG_RECORD_WARN_BARE) != 0 && site != nullptr) {
        try {
            emitLine(LogLevel::Warn, bareWarningText(*site), site->where, time);
        } catch (...) {
            // Out of memory: the warning is lost and counted.
            dropped_[levelIndex(LogLevel::Warn)].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void Logger::emitLine(LogLevel level, std::string_view message, std::source_location where,
                      std::chrono::system_clock::time_point time) noexcept {
    const LogRecord record{level, message, where, time};
    try {
        line_buffer_.clear();
        appendLogLine(record, line_buffer_);
    } catch (...) {
        dropped_[levelIndex(level)].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    writeToSinks(record, line_buffer_);
}

void Logger::writeToSinks(const LogRecord& record, std::string_view line) noexcept {
    for (const auto& entry : sinks_) {
        if (record.level < entry->level.load(std::memory_order_relaxed)) {
            continue;
        }
        try {
            entry->sink->write(record, line);
        } catch (const std::exception& error) {
            reportSinkFailure(*entry, error.what());
        } catch (...) {
            reportSinkFailure(*entry, "unknown exception");
        }
    }
}

void Logger::endBatch() noexcept {
    for (const auto& entry : sinks_) {
        try {
            entry->sink->batchEnd();
        } catch (const std::exception& error) {
            reportSinkFailure(*entry, error.what());
        } catch (...) {
            reportSinkFailure(*entry, "unknown exception");
        }
    }
}

void Logger::flushSinksCaught() noexcept {
    for (const auto& entry : sinks_) {
        try {
            entry->sink->flush();
        } catch (const std::exception& error) {
            reportSinkFailure(*entry, error.what());
        } catch (...) {
            reportSinkFailure(*entry, "unknown exception");
        }
    }
}

void Logger::reportSinkFailure(SinkEntry& entry, std::string_view what) noexcept {
    if (entry.failed) {
        return;
    }
    entry.failed = true;
    // Queued like any message; the drain in progress reaches it after the current record.
    try {
        const std::string text = std::format("a log sink failed: {}", what);
        detail::LogPayload payload;
        payload.text = text;
        queueRecord(LogLevel::Warn, nullptr, std::source_location::current(), payload, false);
    } catch (...) {
        // Out of memory: the report is lost and counted.
        dropped_[levelIndex(LogLevel::Warn)].fetch_add(1, std::memory_order_relaxed);
    }
}

void Logger::reportDropped() noexcept {
    const bool any = std::ranges::any_of(dropped_, [](const std::atomic<std::uint64_t>& count) {
        return count.load(std::memory_order_relaxed) != 0;
    });
    if (!any) {
        return;
    }
    std::array<std::uint64_t, 5> counts{};
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < counts.size(); ++index) {
        counts[index] = dropped_[index].exchange(0, std::memory_order_relaxed);
        total += counts[index];
    }
    if (total == 0) {
        return;
    }
    try {
        emitLine(LogLevel::Warn, detail::droppedText(counts), std::source_location::current(),
                 std::chrono::system_clock::now());
    } catch (...) {
        // Out of memory: the counts stay for the next report.
        for (std::size_t index = 0; index < counts.size(); ++index) {
            dropped_[index].fetch_add(counts[index], std::memory_order_relaxed);
        }
    }
}

}  // namespace dvv
