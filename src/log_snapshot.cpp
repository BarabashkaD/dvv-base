// The crash snapshot (spec §5.8): Logger::writeCrashSnapshot and dvv::decodeCrashSnapshot. The
// layout is described in log.h, above detail::LOG_SNAPSHOT_SITES.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <optional>
#include <ratio>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "dvv/base/log.h"
#include "log_ring.h"

namespace dvv {

namespace {

using detail::LogArgTag;
using detail::LogRecordHeader;
using detail::LogRecordKind;
using detail::LogSnapshotBuffer;
using detail::LogSnapshotSite;

constexpr std::array<char, 8> MAGIC{'D', 'V', 'V', 'L', 'O', 'G', 'S', '1'};
constexpr std::uint32_t END_OF_RECORDS = 0xFFFFFFFFU;
constexpr std::uint32_t NO_SITE = 0xFFFFFFFFU;
constexpr std::size_t LEVELS = 5;  // Trace to Error
// The longest path scanned for its file name.
constexpr std::size_t MAX_PATH_SCAN = 4 * detail::LOG_SNAPSHOT_MAX_STRING;

static_assert(detail::LOG_SNAPSHOT_HEADER_BYTES == MAGIC.size() + LEVELS * sizeof(std::uint64_t));
static_assert(detail::LOG_SNAPSHOT_RECORD_HEADER_BYTES ==
              sizeof(std::uint32_t) + 4 + sizeof(std::uint32_t) + sizeof(std::int64_t));
static_assert(detail::LOG_SNAPSHOT_SITE_HEADER_BYTES == 3 * sizeof(std::uint32_t));

// ---- Writing: async-signal-safe, so no allocation, no lock, no formatting, no exception. ----

class SnapshotOut {
public:
    SnapshotOut(CrashWriter writer, void* context) noexcept : writer_(writer), context_(context) {}

    void bytes(const void* data, std::size_t size) const noexcept {
        if (size != 0) {
            writer_(context_, data, size);
        }
    }

    template <class T>
    void value(const T& value) const noexcept {
        bytes(&value, sizeof(T));
    }

private:
    CrashWriter writer_;
    void* context_;
};

std::size_t boundedLength(const char* text, std::size_t limit) noexcept {
    std::size_t length = 0;
    while (length < limit && text[length] != '\0') {
        ++length;
    }
    return length;
}

std::size_t slotOf(const void* key, std::uint32_t line) noexcept {
    std::uint64_t hash = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(key)) ^
                         (std::uint64_t{line} * 0x9E3779B97F4A7C15ULL);
    hash ^= hash >> 31U;
    hash *= 0xBF58476D1CE4E5B9ULL;
    hash ^= hash >> 29U;
    return static_cast<std::size_t>(hash) & (LogSnapshotBuffer::SLOTS - 1);
}

// What names a site in the table: a LogSite, or the file name pointer and line of a location.
struct SiteKey {
    const void* key = nullptr;
    bool location = false;
    std::uint32_t line = 0;
    const char* format = nullptr;
    std::size_t formatSize = 0;
    const char* path = nullptr;
};

// Adds a site and points `slot` at it; NO_SITE when the table or its text budget is full.
std::uint32_t addSite(LogSnapshotBuffer& buffer, std::uint32_t& slot, const SiteKey& key) noexcept {
    if (buffer.siteCount >= buffer.sites.size()) {
        return NO_SITE;
    }
    const std::size_t path_size = boundedLength(key.path, MAX_PATH_SCAN);
    std::size_t name_start = path_size;
    while (name_start > 0 && key.path[name_start - 1] != '/' && key.path[name_start - 1] != '\\') {
        --name_start;
    }
    const std::size_t file_size = std::min(path_size - name_start, detail::LOG_SNAPSHOT_MAX_STRING);
    const std::size_t format_size = std::min(key.formatSize, detail::LOG_SNAPSHOT_MAX_STRING);
    if (format_size + file_size > detail::LOG_SNAPSHOT_TEXT_BYTES - buffer.textBytes) {
        return NO_SITE;
    }
    const std::uint32_t index = buffer.siteCount++;
    LogSnapshotSite& site = buffer.sites[index];
    site.key = key.key;
    site.location = key.location;
    site.line = key.line;
    site.format = key.format;
    site.formatSize = static_cast<std::uint32_t>(format_size);
    site.file = key.path + name_start;
    site.fileSize = static_cast<std::uint32_t>(file_size);
    buffer.textBytes += format_size + file_size;
    slot = index + 1;
    return index;
}

// The table index of the site a record names, added on first use; NO_SITE when it has none or
// the table is full. Reads the LogSite and the file name, which are static data.
std::uint32_t siteIndex(LogSnapshotBuffer& buffer, const LogRecordHeader& header) noexcept {
    SiteKey key;
    if (header.kind == LogRecordKind::LocationText) {
        const std::source_location where = detail::locationOf(header);
        key.path = where.file_name();
        key.key = key.path;
        key.location = true;
        key.line = where.line();
    } else {
        const detail::LogSite* site = detail::siteOf(header);
        if (site == nullptr) {
            return NO_SITE;
        }
        key.key = site;
        key.line = site->where.line();
        key.format = site->format.data();
        key.formatSize = site->format.size();
        key.path = site->where.file_name();
    }
    if (key.key == nullptr || key.path == nullptr) {
        return NO_SITE;
    }
    // A site's line is part of its key only for a location: one LogSite is one site.
    std::size_t slot = slotOf(key.key, key.location ? key.line : 0);
    // Never more sites than half the slots, so a free slot ends the probe.
    for (std::size_t probe = 0; probe < LogSnapshotBuffer::SLOTS; ++probe) {
        const std::uint32_t stored = buffer.slots[slot];
        if (stored == 0) {
            return addSite(buffer, buffer.slots[slot], key);
        }
        const LogSnapshotSite& entry = buffer.sites[stored - 1];
        if (entry.key == key.key && entry.location == key.location &&
            (!key.location || entry.line == key.line)) {
            return stored - 1;
        }
        slot = (slot + 1) & (LogSnapshotBuffer::SLOTS - 1);
    }
    return NO_SITE;
}

// Writes the record copied into buffer.record (`size` bytes); false once the record budget is
// spent. A record whose header does not check out is skipped.
bool writeRecord(LogSnapshotBuffer& buffer, std::uint32_t size, std::size_t& budget,
                 const SnapshotOut& out) noexcept {
    LogRecordHeader header;
    if (size < sizeof(header)) {
        return true;
    }
    std::memcpy(&header, buffer.record.data(), sizeof(header));
    const auto body_size = static_cast<std::uint32_t>(size - sizeof(header));
    if (header.kind > LogRecordKind::LocationText || header.level >= LogLevel::Off) {
        return true;
    }
    const std::uint32_t kept =
        header.kind == LogRecordKind::Arguments ? body_size : header.textSize;
    if (kept > body_size) {
        return true;
    }
    const std::size_t total = detail::LOG_SNAPSHOT_RECORD_HEADER_BYTES + kept;
    if (total > budget) {
        return false;
    }
    budget -= total;
    const std::uint32_t site = siteIndex(buffer, header);
    // The snapshot's record header goes right before the body, over the copied ring header, so
    // the record is one writer call.
    std::byte* start =
        buffer.record.data() + sizeof(header) - detail::LOG_SNAPSHOT_RECORD_HEADER_BYTES;
    const std::array<std::uint8_t, 4> small{static_cast<std::uint8_t>(header.kind),
                                            static_cast<std::uint8_t>(header.level), header.flags,
                                            header.argumentCount};
    std::memcpy(start, &kept, sizeof(kept));
    std::memcpy(start + 4, small.data(), small.size());
    std::memcpy(start + 8, &site, sizeof(site));
    std::memcpy(start + 12, &header.time, sizeof(header.time));
    out.bytes(start, total);
    return true;
}

// Writes the published records from the release position up to the first unpublished one and
// fills the site table; returns the number of sites.
std::uint32_t writeRecords(const detail::LogRing& ring, LogSnapshotBuffer& buffer,
                           const SnapshotOut& out) noexcept {
    buffer.siteCount = 0;
    buffer.textBytes = 0;
    buffer.slots.fill(0);
    std::uint64_t position = ring.releasePosition();
    // Bounded even while the log thread keeps releasing and writers keep logging.
    const std::uint64_t walk_end = position + 4 * std::uint64_t{ring.bytes()};
    std::size_t budget = detail::LOG_SNAPSHOT_RECORD_BYTES;
    while (position < walk_end) {
        // read() checks the header: the size is within the record limit and the ring.
        const std::optional<detail::LogRing::Record> record = ring.read(position);
        if (!record || record->size > buffer.record.size()) {
            // Either the first unpublished record, or the log thread released past `position`
            // meanwhile and a writer reused the slot on the next lap. Only the release tells.
            const std::uint64_t released = ring.releasePosition();
            if (released > position) {
                position = released;
                continue;
            }
            break;
        }
        std::memcpy(buffer.record.data(), record->payload, record->size);
        // The copy's loads come before the load of the release position: while the log thread
        // has not passed the record, no writer can reuse its bytes, so the copy is whole.
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint64_t released = ring.releasePosition();
        if (released > record->position) {
            // Handled and given back meanwhile; the copy may be torn. Go on from the release.
            position = std::max(released, record->next);
            continue;
        }
        position = record->next;
        if (!writeRecord(buffer, record->size, budget, out)) {
            break;
        }
    }
    return buffer.siteCount;
}

}  // namespace

void Logger::writeCrashSnapshot(CrashWriter writer, void* context) const noexcept {
    if (writer == nullptr || snapshot_claimed_.test_and_set(std::memory_order_acquire)) {
        return;
    }
    const SnapshotOut out{writer, context};
    out.bytes(MAGIC.data(), MAGIC.size());
    static_assert(std::tuple_size_v<decltype(dropped_)> == LEVELS);
    for (const auto& count : dropped_) {
        const std::uint64_t value = count.load(std::memory_order_relaxed);
        out.value(value);
    }
    // Both are set by the first start() and never freed.
    const detail::LogRing* ring = ring_.get();
    LogSnapshotBuffer* buffer = snapshot_buffer_.get();
    std::uint32_t site_count = 0;
    if (ring != nullptr && buffer != nullptr) {
        site_count = writeRecords(*ring, *buffer, out);
    }
    out.value(END_OF_RECORDS);
    out.value(site_count);
    for (std::uint32_t index = 0; index < site_count; ++index) {
        const LogSnapshotSite& site = buffer->sites[index];
        const std::array<std::uint32_t, 3> header{site.line, site.formatSize, site.fileSize};
        out.bytes(header.data(), sizeof(header));
        out.bytes(site.format, site.formatSize);
        out.bytes(site.file, site.fileSize);
    }
}

namespace {

// ---- Decoding: never trusts the input. ----

constexpr std::string_view SUFFIX = " (from crash snapshot)";
constexpr std::string_view UNKNOWN_SITE = "<unknown call site>";
constexpr std::string_view UNKNOWN_FILE = "?";
// A width or precision longer than this many digits is not formatted from a snapshot.
constexpr std::size_t MAX_SPEC_DIGITS = 4;
// The largest index a replacement field can name (a record has at most 255 arguments).
constexpr std::size_t MAX_INDEX_DIGITS = 3;
// A message formatted from tags longer than this gives the raw line instead.
constexpr std::size_t MAX_MESSAGE_BYTES = std::size_t{1} << 20U;
// All the text one decode produces or discards; the decode stops when it is spent.
constexpr std::size_t MAX_DECODE_BYTES = 4 * detail::LOG_SNAPSHOT_CAPACITY;
// Kept back from the records for the last two lines: the dropped counts and STOPPED_TEXT (each a
// few hundred bytes at most).
constexpr std::size_t FINAL_LINES_BYTES = 1024;
// The last line of a decode that ran out of budget.
constexpr std::string_view STOPPED_TEXT = "decode stopped: output limit reached";

// Bounds the decoder's output, so a hostile snapshot cannot make it format gigabytes.
class OutputBudget {
public:
    [[nodiscard]] std::size_t left() const noexcept { return left_; }

    // False (and spent) when `bytes` exceeds what is left.
    [[nodiscard]] bool take(std::size_t bytes) noexcept {
        if (bytes > left_) {
            left_ = 0;
            return false;
        }
        left_ -= bytes;
        return true;
    }

private:
    std::size_t left_ = MAX_DECODE_BYTES - FINAL_LINES_BYTES;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) noexcept : data_(data) {}

    template <class T>
    [[nodiscard]] bool take(T& value) noexcept {
        if (sizeof(T) > data_.size() - at_) {
            return false;
        }
        std::memcpy(static_cast<void*>(&value), data_.data() + at_, sizeof(T));
        at_ += sizeof(T);
        return true;
    }

    [[nodiscard]] bool bytes(std::size_t size, std::span<const std::byte>& out) noexcept {
        if (size > data_.size() - at_) {
            return false;
        }
        out = data_.subspan(at_, size);
        at_ += size;
        return true;
    }

private:
    std::span<const std::byte> data_;
    std::size_t at_ = 0;
};

std::string_view asText(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

using Value = std::variant<bool, char, signed char, unsigned char, std::int16_t, std::uint16_t,
                           std::int32_t, std::uint32_t, std::int64_t, std::uint64_t, float, double,
                           long double, std::string_view, const void*>;

template <class T>
bool takeValue(Reader& in, std::vector<Value>& values) {
    T value{};
    if (!in.take(value)) {
        return false;
    }
    values.emplace_back(value);
    return true;
}

bool takeTagged(Reader& in, LogArgTag tag, std::vector<Value>& values) {
    switch (tag) {
        case LogArgTag::Bool: {
            std::uint8_t value = 0;  // any byte, so never an invalid bool
            if (!in.take(value)) {
                return false;
            }
            values.emplace_back(value != 0);
            return true;
        }
        case LogArgTag::Char: return takeValue<char>(in, values);
        case LogArgTag::I8: return takeValue<signed char>(in, values);
        case LogArgTag::U8: return takeValue<unsigned char>(in, values);
        case LogArgTag::I16: return takeValue<std::int16_t>(in, values);
        case LogArgTag::U16: return takeValue<std::uint16_t>(in, values);
        case LogArgTag::I32: return takeValue<std::int32_t>(in, values);
        case LogArgTag::U32: return takeValue<std::uint32_t>(in, values);
        case LogArgTag::I64: return takeValue<std::int64_t>(in, values);
        case LogArgTag::U64: return takeValue<std::uint64_t>(in, values);
        case LogArgTag::F32: return takeValue<float>(in, values);
        case LogArgTag::F64: return takeValue<double>(in, values);
        case LogArgTag::F80: return takeValue<long double>(in, values);
        case LogArgTag::String: {
            std::uint32_t length = 0;
            std::span<const std::byte> text;
            if (!in.take(length) || !in.bytes(length, text)) {
                return false;
            }
            values.emplace_back(asText(text));
            return true;
        }
        case LogArgTag::Pointer: return takeValue<const void*>(in, values);
    }
    return false;  // an unknown tag
}

// The values of an Arguments body; false (with the values before it) when one does not decode.
bool decodeValues(std::span<const std::byte> body, std::size_t count, std::vector<Value>& values) {
    Reader in{body};
    for (std::size_t index = 0; index < count; ++index) {
        std::uint8_t tag = 0;
        if (!in.take(tag) || !takeTagged(in, static_cast<LogArgTag>(tag), values)) {
            return false;
        }
    }
    return true;
}

bool parseIndex(std::string_view id, std::size_t& index) noexcept {
    if (id.size() > MAX_INDEX_DIGITS) {
        return false;
    }
    index = 0;
    for (const char digit : id) {
        if (digit < '0' || digit > '9') {
            return false;
        }
        index = index * 10 + static_cast<std::size_t>(digit - '0');
    }
    return true;
}

// False for a width or precision too large to format from untrusted data.
bool safeSpec(std::string_view spec) noexcept {
    std::size_t digits = 0;
    for (const char character : spec) {
        digits = character >= '0' && character <= '9' ? digits + 1 : 0;
        if (digits > MAX_SPEC_DIGITS) {
            return false;
        }
    }
    return true;
}

void formatValue(std::string_view spec, const Value& value, std::string& out) {
    std::string pattern = "{:";
    pattern += spec;
    pattern += '}';
    std::visit(
        [&](auto copy) {
            std::vformat_to(std::back_inserter(out), pattern, std::make_format_args(copy));
        },
        value);
}

// Formats `format` with the values as std::vformat would, one replacement field at a time;
// false when that cannot be done from the tags (a nested field in a spec, an index out of range,
// a stray brace, an oversized width, an output longer than `limit`). Throws std::format_error when
// a spec does not suit a value.
bool formatFromTags(std::string_view format, const std::vector<Value>& values, std::size_t limit,
                    std::string& out) {
    std::size_t next_index = 0;
    std::size_t at = 0;
    while (at < format.size()) {
        const std::size_t brace = format.find_first_of("{}", at);
        if (brace == std::string_view::npos) {
            out.append(format.substr(at));
            break;
        }
        out.append(format.substr(at, brace - at));
        at = brace;
        if (at + 1 < format.size() && format[at + 1] == format[at]) {
            out += format[at];  // "{{" or "}}"
            at += 2;
            continue;
        }
        if (format[at] == '}') {
            return false;
        }
        const std::size_t close = format.find('}', at + 1);
        if (close == std::string_view::npos) {
            return false;
        }
        const std::string_view field = format.substr(at + 1, close - at - 1);
        if (field.find('{') != std::string_view::npos) {
            return false;
        }
        const std::size_t colon = field.find(':');
        const std::string_view id = field.substr(0, colon);
        const std::string_view spec =
            colon == std::string_view::npos ? std::string_view{} : field.substr(colon + 1);
        std::size_t index = 0;
        if (id.empty()) {
            index = next_index++;
        } else if (!parseIndex(id, index)) {
            return false;
        }
        if (index >= values.size() || !safeSpec(spec)) {
            return false;
        }
        formatValue(spec, values[index], out);
        // At most one field past the limit: a width of 4 digits and a value from the record.
        if (out.size() > limit) {
            return false;
        }
        at = close + 1;
    }
    return out.size() <= limit;
}

// The message of an Arguments record; nullopt once the output budget is spent. A message that is
// kept is paid for with its line; a discarded attempt is paid for here, so that formatting it is
// never free.
std::optional<std::string> argumentsMessage(std::string_view format, bool known_site,
                                            std::span<const std::byte> body, std::size_t count,
                                            OutputBudget& budget) {
    std::vector<Value> values;
    const bool decoded = decodeValues(body, count, values);
    if (decoded && known_site) {
        std::string message;
        bool formatted = false;
        try {
            formatted =
                formatFromTags(format, values, std::min(MAX_MESSAGE_BYTES, budget.left()), message);
        } catch (const std::format_error&) {
            formatted = false;  // a spec that does not suit its value: the raw values below
        }
        if (formatted) {
            return message;
        }
        if (!budget.take(message.size())) {
            return std::nullopt;
        }
    }
    std::string message{known_site ? format : UNKNOWN_SITE};
    message += " | ";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            message += ", ";
        }
        std::visit([&](auto copy) { std::format_to(std::back_inserter(message), "{}", copy); },
                   values[index]);
    }
    return message;
}

std::chrono::sys_time<std::chrono::milliseconds> timeOf(std::int64_t ticks) noexcept {
    using std::chrono::milliseconds;
    // Finer than milliseconds on every platform, so the cast divides and cannot overflow.
    static_assert(std::ratio_less_equal_v<std::chrono::system_clock::period, std::milli>);
    const auto time =
        std::chrono::duration_cast<milliseconds>(std::chrono::system_clock::duration{ticks});
    constexpr milliseconds LATEST{253'402'300'799'999};  // 9999-12-31T23:59:59.999Z
    if (time < milliseconds{0} || time > LATEST) {
        return {};
    }
    return std::chrono::sys_time<milliseconds>{time};
}

std::string snapshotLine(std::int64_t ticks, LogLevel level, std::string_view message,
                         std::string_view file, std::uint32_t line) {
    std::string out;
    detail::appendStandardLine(timeOf(ticks), level, message, file, line, out);
    out += SUFFIX;
    return out;
}

struct SnapshotSiteText {
    std::string_view format;
    std::string_view file;
    std::uint32_t line = 0;
};

struct SnapshotRecord {
    std::uint8_t kind = 0;
    std::uint8_t level = 0;
    std::uint8_t flags = 0;
    std::uint8_t count = 0;
    std::uint32_t site = 0;
    std::int64_t time = 0;
    std::span<const std::byte> body;
};

enum class Step : std::uint8_t { Record, End, Cut };

Step readRecord(Reader& in, SnapshotRecord& record) noexcept {
    std::uint32_t body_size = 0;
    if (!in.take(body_size)) {
        return Step::Cut;
    }
    if (body_size == END_OF_RECORDS) {
        return Step::End;
    }
    std::array<std::uint8_t, 4> small{};
    if (!in.take(small) || !in.take(record.site) || !in.take(record.time) ||
        !in.bytes(body_size, record.body)) {
        return Step::Cut;
    }
    record.kind = small[0];
    record.level = small[1];
    record.flags = small[2];
    record.count = small[3];
    return Step::Record;
}

void readSites(Reader& in, std::vector<SnapshotSiteText>& sites) {
    std::uint32_t count = 0;
    if (!in.take(count)) {
        return;
    }
    // Not trusted for a reserve: the entries end where the data does.
    for (std::uint32_t index = 0; index < count; ++index) {
        std::array<std::uint32_t, 3> header{};
        std::span<const std::byte> format;
        std::span<const std::byte> file;
        // The writer never stores a longer string: the table is damaged from here on.
        if (!in.take(header) || header[1] > detail::LOG_SNAPSHOT_MAX_STRING ||
            header[2] > detail::LOG_SNAPSHOT_MAX_STRING || !in.bytes(header[1], format) ||
            !in.bytes(header[2], file)) {
            return;
        }
        sites.push_back({asText(format), asText(file), header[0]});
    }
}

// Appends the record's lines; false once the output budget is spent (nothing more is decoded).
bool appendRecordLines(const SnapshotRecord& record, const std::vector<SnapshotSiteText>& sites,
                       OutputBudget& budget, std::vector<std::string>& lines) {
    if (record.kind > static_cast<std::uint8_t>(LogRecordKind::LocationText) ||
        record.level >= static_cast<std::uint8_t>(LogLevel::Off)) {
        return true;
    }
    const auto kind = static_cast<LogRecordKind>(record.kind);
    const auto level = static_cast<LogLevel>(record.level);
    const SnapshotSiteText* site = record.site < sites.size() ? &sites[record.site] : nullptr;
    const std::string_view file = site != nullptr ? site->file : UNKNOWN_FILE;
    const std::uint32_t line = site != nullptr ? site->line : 0;
    std::string text;
    if (kind == LogRecordKind::Arguments) {
        std::optional<std::string> message =
            argumentsMessage(site != nullptr ? site->format : UNKNOWN_SITE, site != nullptr,
                             record.body, record.count, budget);
        if (!message) {
            return false;
        }
        text = snapshotLine(record.time, level, *message, file, line);
    } else {
        text = snapshotLine(record.time, level, asText(record.body), file, line);
    }
    if (!budget.take(text.size())) {
        return false;
    }
    lines.push_back(std::move(text));
    if ((record.flags & detail::LOG_RECORD_WARN_BARE) != 0 && kind != LogRecordKind::LocationText &&
        site != nullptr) {
        std::string warning =
            snapshotLine(record.time, LogLevel::Warn,
                         detail::bareWarningText(site->file, site->line), site->file, site->line);
        if (!budget.take(warning.size())) {
            return false;
        }
        lines.push_back(std::move(warning));
    }
    return true;
}

void decodeInto(std::span<const std::byte> snapshot, std::vector<std::string>& lines) {
    Reader in{snapshot};
    std::array<char, MAGIC.size()> magic{};
    std::array<std::uint64_t, LEVELS> dropped{};
    if (!in.take(magic) || magic != MAGIC || !in.take(dropped)) {
        return;
    }
    // The site table follows the records: find it first. A snapshot cut within its records has
    // none, and its records decode as unknown call sites.
    Reader walk = in;
    SnapshotRecord record;
    Step step = Step::Record;
    while (step == Step::Record) {
        step = readRecord(walk, record);
    }
    std::vector<SnapshotSiteText> sites;
    if (step == Step::End) {
        readSites(walk, sites);
    }
    std::int64_t last_time = 0;
    OutputBudget budget;
    bool stopped = false;
    while (readRecord(in, record) == Step::Record) {
        if (!appendRecordLines(record, sites, budget, lines)) {
            stopped = true;  // the output budget is spent
            break;
        }
        last_time = record.time;
    }
    // Dropped messages come after the records that filled the queue; no location is known.
    if (std::ranges::any_of(dropped, [](std::uint64_t count) { return count != 0; })) {
        lines.push_back(
            snapshotLine(last_time, LogLevel::Warn, detail::droppedText(dropped), UNKNOWN_FILE, 0));
    }
    // Paid from FINAL_LINES_BYTES, so the output stays within MAX_DECODE_BYTES.
    if (stopped) {
        lines.push_back(snapshotLine(last_time, LogLevel::Warn, STOPPED_TEXT, UNKNOWN_FILE, 0));
    }
}

}  // namespace

std::vector<std::string> decodeCrashSnapshot(std::span<const std::byte> snapshot) noexcept {
    std::vector<std::string> lines;
    try {
        decodeInto(snapshot, lines);
    } catch (...) {
        return lines;  // an allocation failed: the lines decoded so far
    }
    return lines;
}

}  // namespace dvv
