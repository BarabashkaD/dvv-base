#include "log_ring.h"

#include <cstring>
#include <stdexcept>

namespace dvv::detail {

namespace {

constexpr std::uint32_t KIND_RECORD = 0;
constexpr std::uint32_t KIND_PADDING = 1;
constexpr std::uint64_t ALIGNMENT = 8;
constexpr std::size_t MIN_BYTES = 4096;

static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint64_t>::required_alignment <= alignof(std::uint64_t));

std::size_t checkedBytes(std::size_t bytes) {
    if (bytes < MIN_BYTES || (bytes & (bytes - 1)) != 0) {
        throw std::invalid_argument("LogRing: the size must be a power of two of at least 4 KiB");
    }
    return bytes;
}

constexpr std::uint64_t alignUp(std::uint64_t size) noexcept {
    return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

}  // namespace

LogRing::LogRing(std::size_t bytes)
    : bytes_(checkedBytes(bytes)),
      mask_(bytes - 1),
      record_limit_(bytes / 16),
      words_(std::make_unique<std::uint64_t[]>(bytes / sizeof(std::uint64_t))) {}

std::optional<LogRing::Reservation> LogRing::reserve(std::uint32_t payload_bytes,
                                                     bool urgent) noexcept {
    const std::uint64_t record = alignUp(std::uint64_t{HEADER_BYTES} + payload_bytes);
    if (record > record_limit_) {
        return std::nullopt;
    }
    const std::uint64_t capacity = urgent ? bytes_ : bytes_ / 4 * 3;
    for (;;) {
        // Acquire: the reader's reads of the released bytes happen before this writer reuses
        // them. Loaded before the write position, which is then at least this value.
        const std::uint64_t released = release_.load(std::memory_order_acquire);
        std::uint64_t position = write_.load(std::memory_order_relaxed);
        const std::uint64_t tail = bytes_ - (position & mask_);
        const std::uint64_t padding = record > tail ? tail : 0;
        const std::uint64_t end = position + padding + record;
        if (end - released > capacity) {
            return std::nullopt;
        }
        if (!write_.compare_exchange_weak(position, end, std::memory_order_relaxed)) {
            continue;
        }
        if (padding != 0) {
            if (tail >= HEADER_BYTES) {
                writeHeader(position, static_cast<std::uint32_t>(tail - HEADER_BYTES),
                            KIND_PADDING);
            }
            commit(position);
            position += padding;
        }
        writeHeader(position, payload_bytes, KIND_RECORD);
        std::byte* payload =
            reinterpret_cast<std::byte*>(words_.get()) + (position & mask_) + HEADER_BYTES;
        return Reservation{payload, position, payload_bytes};
    }
}

void LogRing::publish(const Reservation& reservation) noexcept {
    commit(reservation.position);
}

void LogRing::publishAsPadding(const Reservation& reservation) noexcept {
    writeHeader(reservation.position, reservation.size, KIND_PADDING);
    commit(reservation.position);
}

std::optional<LogRing::Record> LogRing::read(std::uint64_t position) const noexcept {
    for (;;) {
        // Acquire: pairs with commit(), so the header and payload written before it are visible.
        const std::uint64_t word =
            std::atomic_ref<std::uint64_t>(*wordAt(position)).load(std::memory_order_acquire);
        if (word != (position ^ LOG_COMMIT_MIX)) {
            return std::nullopt;
        }
        // A stale payload word at or past the write position is never a commit word (R4). A real
        // record passes: its reservation moved the write position before its commit, which the
        // acquire load above makes visible.
        if (position >= write_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        const std::uint64_t offset = position & mask_;
        const std::uint64_t tail = bytes_ - offset;
        if (tail < HEADER_BYTES) {
            // Only padding is this close to the end.
            position += tail;
            continue;
        }
        const std::byte* header = base() + offset;
        std::uint32_t size = 0;
        std::uint32_t kind = 0;
        std::memcpy(&size, header + 8, sizeof(size));
        std::memcpy(&kind, header + 12, sizeof(kind));
        // A logged value can forge a commit word (R4), so the header is checked before it is
        // trusted: a record within the limit, or padding within the limit or up to the ring's end,
        // never crossing the end. Anything else counts as not published.
        const std::uint64_t total = alignUp(std::uint64_t{HEADER_BYTES} + size);
        const bool within_limit = total <= record_limit_;
        const bool valid_record = kind == KIND_RECORD && within_limit;
        const bool valid_padding = kind == KIND_PADDING && (within_limit || total == tail);
        if (total > tail || !(valid_record || valid_padding)) {
            return std::nullopt;
        }
        const std::uint64_t next = position + total;
        if (kind == KIND_PADDING) {
            position = next;
            continue;
        }
        return Record{header + HEADER_BYTES, size, position, next};
    }
}

void LogRing::release(std::uint64_t position) noexcept {
    release_.store(position, std::memory_order_release);
}

std::uint64_t LogRing::releasePosition() const noexcept {
    return release_.load(std::memory_order_acquire);
}

std::uint64_t LogRing::writePosition() const noexcept {
    // Coherence suffices for a flush target: a reservation that happens before the load is seen.
    return write_.load(std::memory_order_relaxed);
}

const std::byte* LogRing::base() const noexcept {
    return reinterpret_cast<const std::byte*>(words_.get());
}

std::uint64_t* LogRing::wordAt(std::uint64_t position) const noexcept {
    return words_.get() + ((position & mask_) / sizeof(std::uint64_t));
}

void LogRing::writeHeader(std::uint64_t position, std::uint32_t size, std::uint32_t kind) noexcept {
    std::byte* header = reinterpret_cast<std::byte*>(wordAt(position));
    std::memcpy(header + 8, &size, sizeof(size));
    std::memcpy(header + 12, &kind, sizeof(kind));
}

void LogRing::commit(std::uint64_t position) noexcept {
    // Release: the header and payload written before it are visible to a reader that accepts it.
    std::atomic_ref<std::uint64_t>(*wordAt(position))
        .store(position ^ LOG_COMMIT_MIX, std::memory_order_release);
}

}  // namespace dvv::detail
