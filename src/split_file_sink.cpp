#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <format>
#include <optional>
#include <source_location>
#include <system_error>
#include <utility>

#include "dvv/base/local_time.h"
#include "dvv/base/log.h"
#include "dvv/base/utf8.h"
#include "split_file_sink_detail.h"

namespace dvv {

namespace {

constexpr std::string_view EXTENSION = ".log";
// "-YYYYMMDD-HHMMSS"
constexpr std::size_t STAMP_CHARS = 16;
constexpr std::size_t MAX_SUFFIX_DIGITS = 9;

bool allDigits(std::string_view text) noexcept {
    return std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

std::uint64_t digitsValue(std::string_view text) noexcept {
    std::uint64_t value = 0;
    for (const char c : text) {
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return value;
}

char asciiLower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// text[pos, pos + count), unchecked; substr would be a throwing path in a noexcept parser.
std::string_view slice(std::string_view text, std::size_t pos, std::size_t count) noexcept {
    return {text.data() + pos, count};
}

bool sameText(std::string_view a, std::string_view b, LetterCase letters) noexcept {
    if (letters == LetterCase::Exact) {
        return a == b;
    }
    return std::ranges::equal(a, b, [](char x, char y) { return asciiLower(x) == asciiLower(y); });
}

}  // namespace

std::optional<SplitLogName> parseSplitLogName(std::string_view name, std::string_view prefix,
                                              LetterCase letters) noexcept {
    if (prefix.empty() || name.size() < prefix.size() + STAMP_CHARS + EXTENSION.size() ||
        !sameText(slice(name, 0, prefix.size()), prefix, letters) ||
        !sameText(slice(name, name.size() - EXTENSION.size(), EXTENSION.size()), EXTENSION,
                  letters)) {
        return std::nullopt;
    }
    const std::string_view rest =
        slice(name, prefix.size(), name.size() - prefix.size() - EXTENSION.size());
    const std::string_view date = slice(rest, 1, 8);
    const std::string_view time = slice(rest, 10, 6);
    if (rest[0] != '-' || rest[9] != '-' || !allDigits(date) || !allDigits(time)) {
        return std::nullopt;
    }
    SplitLogName parsed;
    parsed.stamp = digitsValue(date) * 1'000'000U + digitsValue(time);
    const std::string_view suffix = slice(rest, STAMP_CHARS, rest.size() - STAMP_CHARS);
    if (!suffix.empty()) {
        const std::string_view number = slice(suffix, 1, suffix.size() - 1);
        if (suffix[0] != '-' || number.empty() || number.size() > MAX_SUFFIX_DIGITS ||
            number[0] == '0' || !allDigits(number)) {
            return std::nullopt;
        }
        parsed.number = static_cast<std::uint32_t>(digitsValue(number));
    }
    return parsed;
}

namespace detail {

namespace {

std::atomic<SplitFileClock> split_file_clock{nullptr};

}  // namespace

std::string splitLogFileStem(std::string_view prefix, std::chrono::system_clock::time_point time) {
    const std::optional<std::tm> local = localTime(time);
    if (!local) {
        // Outside the platform's local time range: UTC rather than no name.
        return std::format("{}-{:%Y%m%d-%H%M%S}", prefix,
                           std::chrono::floor<std::chrono::seconds>(time));
    }
    return std::format("{}-{:04}{:02}{:02}-{:02}{:02}{:02}", prefix, local->tm_year + 1900,
                       local->tm_mon + 1, local->tm_mday, local->tm_hour, local->tm_min,
                       local->tm_sec);
}

void setSplitFileClock(SplitFileClock clock) noexcept {
    split_file_clock.store(clock);
}

}  // namespace detail

namespace {

// A clash moves on to the next suffix at most this far.
constexpr std::uint32_t MAX_SUFFIX = 1000;

// A failure note: stderr only, never the Logger (the sink runs under its lock).
void note(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fputc('\n', stderr);
}

struct CreatedFile {
    std::filesystem::path path;
    std::ofstream stream;
};

// Creates <folder>/<prefix>-YYYYMMDD-HHMMSS[-N].log exclusively, named by the clock's local time.
// A name held by anything (a file, a folder, a link) moves on to the next suffix; any other
// failure ends the attempt.
std::optional<CreatedFile> createExclusive(const std::filesystem::path& folder,
                                           std::string_view prefix) {
    const detail::SplitFileClock clock = detail::split_file_clock.load();
    const auto now = clock != nullptr ? clock() : std::chrono::system_clock::now();
    const std::string stem = detail::splitLogFileStem(prefix, now);
    for (std::uint32_t number = 1; number <= MAX_SUFFIX; ++number) {
        const std::string name =
            number == 1 ? std::format("{}.log", stem) : std::format("{}-{}.log", stem, number);
        CreatedFile created{.path = folder / pathFromText(name), .stream = {}};
        created.stream.open(created.path, std::ios::binary | std::ios::out | std::ios::noreplace);
        if (created.stream.is_open()) {
            return created;
        }
        std::error_code error;
        const auto type = std::filesystem::symlink_status(created.path, error).type();
        if (type == std::filesystem::file_type::not_found ||
            type == std::filesystem::file_type::none ||
            type == std::filesystem::file_type::unknown) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}  // namespace

Result<std::shared_ptr<SplitFileSink>> SplitFileSink::open(std::filesystem::path folder,
                                                           std::string prefix,
                                                           std::uintmax_t max_bytes,
                                                           SplitOpen mode) {
    if (max_bytes == 0 || prefix.empty()) {
        return makeError(Errc::Internal, "split log needs max_bytes > 0 and a prefix");
    }
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        return makeError(Errc::Io, std::format("cannot create log folder '{}': {}",
                                               pathDisplayText(folder), error.message()));
    }
    std::shared_ptr<SplitFileSink> sink(
        new SplitFileSink(std::move(folder), std::move(prefix), max_bytes));
    // No other thread has the sink yet: its state is set here without the mutex.

    // The matching files: their total size and the newest by (timestamp, N).
    std::optional<SplitLogName> newest_name;
    std::filesystem::path newest;
    std::uintmax_t newest_size = 0;
    std::filesystem::directory_iterator entry(sink->folder_, error);
    for (; !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
        const std::string file_name = pathText(entry->path().filename());
        const auto name = parseSplitLogName(file_name, sink->prefix_);
        std::error_code entry_error;
        if (!name || !entry->is_regular_file(entry_error)) {
            continue;
        }
        const std::uintmax_t size = entry->file_size(entry_error);
        if (entry_error) {
            continue;
        }
        sink->total_ += size;
        if (!newest_name || *name > *newest_name) {
            newest_name = name;
            newest = entry->path();
            newest_size = size;
        }
    }

    if (mode == SplitOpen::AppendNewest && newest_name && newest_size < max_bytes) {
        sink->stream_.open(newest, std::ios::binary | std::ios::app);
        if (sink->stream_.is_open()) {
            sink->current_ = std::move(newest);
            sink->size_ = newest_size;
            sink->has_lines_ = newest_size > 0;
            return sink;
        }
        sink->stream_.clear();
        note(std::format("dvv: cannot append to log file '{}'; starting a new one",
                         pathDisplayText(newest)));
    }
    auto created = createExclusive(sink->folder_, sink->prefix_);
    if (!created) {
        return makeError(Errc::Io, std::format("cannot create a log file in '{}'",
                                               pathDisplayText(sink->folder_)));
    }
    sink->stream_ = std::move(created->stream);
    sink->current_ = std::move(created->path);
    sink->header_pending_ = true;
    return sink;
}

Result<std::shared_ptr<SplitFileSink>> SplitFileSink::open(std::filesystem::path folder,
                                                           std::string prefix,
                                                           std::uintmax_t max_bytes,
                                                           bool append_newest) {
    return open(std::move(folder), std::move(prefix), max_bytes,
                append_newest ? SplitOpen::AppendNewest : SplitOpen::NewFile);
}

SplitFileSink::SplitFileSink(std::filesystem::path folder, std::string prefix,
                             std::uintmax_t max_bytes)
    : folder_(std::move(folder)), prefix_(std::move(prefix)), max_bytes_(max_bytes) {}

SplitFileSink::~SplitFileSink() = default;

void SplitFileSink::setHeader(MoveOnlyFunction<std::string(const std::filesystem::path&)> make) {
    const std::lock_guard lock(mutex_);
    make_header_ = std::move(make);
    if (header_pending_ && make_header_ && stream_.is_open()) {
        header_pending_ = false;
        writeHeader({});
    }
}

std::filesystem::path SplitFileSink::currentFile() const {
    const std::lock_guard lock(mutex_);
    return current_;
}

std::uintmax_t SplitFileSink::totalBytes() const {
    const std::lock_guard lock(mutex_);
    return total_;
}

std::uint64_t SplitFileSink::splits() const {
    const std::lock_guard lock(mutex_);
    return splits_;
}

void SplitFileSink::write(const LogRecord& record, std::string_view line) {
    const std::lock_guard lock(mutex_);
    const std::uintmax_t needed = line.size() + 1;
    const bool flush_now = record.level >= LogLevel::Warn || record.mark;
    if (stream_.is_open() && has_lines_ && size_ + needed > max_bytes_ &&
        offered_ >= next_attempt_at_) {
        startFile();  // on failure the current file stays in use
    }
    // No file open (a failure closed it): try a new one once the wait is over, else drop the line.
    if (!stream_.is_open() && (offered_ < next_attempt_at_ || !startFile())) {
        offered_ += needed;
        return;
    }
    header_pending_ = false;
    // A failed write closed the file; a new one is tried at once unless the wait says otherwise.
    if (writeLine(line, flush_now) ||
        (offered_ >= next_attempt_at_ && startFile() && writeLine(line, flush_now))) {
        has_lines_ = true;
    }
    offered_ += needed;
}

void SplitFileSink::batchEnd() {
    const std::lock_guard lock(mutex_);
    flushStream();
}

void SplitFileSink::flush() {
    const std::lock_guard lock(mutex_);
    flushStream();
}

bool SplitFileSink::startFile() {
    auto created = createExclusive(folder_, prefix_);
    if (!created) {
        next_attempt_at_ = offered_ + backoff();
        note(stream_.is_open()
                 ? std::format("dvv: cannot create a log file in '{}'; still writing '{}'",
                               pathDisplayText(folder_), pathDisplayText(current_))
                 : std::format("dvv: cannot create a log file in '{}'; lines are dropped",
                               pathDisplayText(folder_)));
        return false;
    }
    const std::filesystem::path previous = std::move(current_);
    // The old file's last buffered lines: a failure is noted; the new file is already open, so the
    // back-off stays as it is.
    stream_.flush();
    if (!stream_.good()) {
        note(std::format("dvv: writing log file '{}' failed; the file is closed",
                         pathDisplayText(previous)));
    }
    stream_.close();
    stream_ = std::move(created->stream);
    current_ = std::move(created->path);
    size_ = 0;
    has_lines_ = false;
    header_pending_ = false;
    started_at_ = offered_;
    ++splits_;
    if (make_header_) {
        writeHeader(previous);
    }
    return stream_.is_open();
}

bool SplitFileSink::writeHeader(const std::filesystem::path& previous) {
    const std::string message = make_header_(previous);
    LogRecord record;
    record.level = LogLevel::Info;
    record.message = message;
    record.where = std::source_location::current();
    record.time = std::chrono::system_clock::now();
    return writeLine(formatLogRecord(record, SourceSuffix::Without), true);
}

bool SplitFileSink::writeLine(std::string_view line, bool flush_now) {
    stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
    stream_.put('\n');
    if (flush_now) {
        stream_.flush();
    }
    if (!stream_.good()) {
        failFile();
        return false;
    }
    size_ += line.size() + 1;
    total_ += line.size() + 1;
    return true;
}

void SplitFileSink::flushStream() {
    if (stream_.is_open()) {
        stream_.flush();
        if (!stream_.good()) {
            failFile();
        }
    }
}

void SplitFileSink::failFile() {
    note(std::format("dvv: writing log file '{}' failed; the file is closed",
                     pathDisplayText(current_)));
    stream_.close();
    stream_.clear();
    next_attempt_at_ = std::max(next_attempt_at_, started_at_ + backoff());
}

std::uintmax_t SplitFileSink::backoff() const noexcept {
    return std::max<std::uintmax_t>(max_bytes_ / 4, 1);
}

}  // namespace dvv
