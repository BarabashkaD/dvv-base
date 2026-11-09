#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

#include "log_ring.h"

namespace {

using dvv::detail::LogRing;

constexpr std::size_t SMALL_RING = 4096;
// The ring's own header in front of every payload: the commit word, the size and the kind.
constexpr std::uint32_t RING_HEADER = 16;

std::byte patternByte(std::uint64_t seed, std::size_t index) {
    return static_cast<std::byte>((seed * 131U + index * 7U + 1U) & 0xFFU);
}

void fill(const LogRing::Reservation& reservation, std::uint64_t seed) {
    for (std::size_t i = 0; i < reservation.size; ++i) {
        reservation.payload[i] = patternByte(seed, i);
    }
}

bool holds(const LogRing::Record& record, std::uint64_t seed, std::uint32_t size) {
    if (record.size != size) {
        return false;
    }
    for (std::size_t i = 0; i < size; ++i) {
        if (record.payload[i] != patternByte(seed, i)) {
            return false;
        }
    }
    return true;
}

std::uint64_t wordAt(const LogRing& ring, std::uint64_t position) {
    std::uint64_t word = 0;
    std::memcpy(&word, ring.base() + (position % ring.bytes()), sizeof(word));
    return word;
}

// reserve() and read() that must succeed (value_or keeps clang-tidy's optional check satisfied;
// a failed REQUIRE ends the test before it).
LogRing::Reservation reserveOrFail(LogRing& ring, std::uint32_t size, bool urgent) {
    const std::optional<LogRing::Reservation> reservation = ring.reserve(size, urgent);
    REQUIRE(reservation.has_value());
    return reservation.value_or(LogRing::Reservation{});
}

LogRing::Record readOrFail(const LogRing& ring, std::uint64_t position) {
    const std::optional<LogRing::Record> record = ring.read(position);
    REQUIRE(record.has_value());
    return record.value_or(LogRing::Record{});
}

}  // namespace

TEST_CASE("ring: records come back in reservation order with their bytes") {
    LogRing ring(SMALL_RING);
    const std::array<std::uint32_t, 4> sizes{1, 8, 13, 100};
    std::vector<LogRing::Reservation> reservations;
    reservations.reserve(sizes.size());
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const LogRing::Reservation reservation = reserveOrFail(ring, sizes[i], i % 2 == 0);
        CHECK(reservation.size == sizes[i]);
        CHECK(reservation.position % 8 == 0);
        fill(reservation, i);
        reservations.push_back(reservation);
    }
    for (const LogRing::Reservation& reservation : reservations) {
        ring.publish(reservation);
    }

    std::uint64_t position = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const LogRing::Record record = readOrFail(ring, position);
        CHECK(record.position == reservations[i].position);
        CHECK(record.next > record.position);
        CHECK(record.next % 8 == 0);
        CHECK(holds(record, i, sizes[i]));
        position = record.next;
    }
    CHECK_FALSE(ring.read(position).has_value());
    CHECK(ring.releasePosition() == 0);
    ring.release(position);
    CHECK(ring.releasePosition() == position);
}

TEST_CASE("ring: an unpublished record stops the reader, then appears once published") {
    LogRing ring(SMALL_RING);
    const LogRing::Reservation first = reserveOrFail(ring, 32, false);
    const LogRing::Reservation second = reserveOrFail(ring, 48, false);
    fill(first, 1);
    fill(second, 2);

    ring.publish(second);
    CHECK_FALSE(ring.read(first.position).has_value());

    ring.publish(first);
    const LogRing::Record one = readOrFail(ring, first.position);
    CHECK(holds(one, 1, 32));
    const LogRing::Record two = readOrFail(ring, one.next);
    CHECK(two.position == second.position);
    CHECK(holds(two, 2, 48));
    CHECK_FALSE(ring.read(two.next).has_value());

    // A held reservation published as padding is skipped by the reader.
    const LogRing::Reservation held = reserveOrFail(ring, 40, false);
    const LogRing::Reservation after = reserveOrFail(ring, 24, false);
    fill(after, 3);
    ring.publish(after);
    CHECK_FALSE(ring.read(two.next).has_value());
    ring.publishAsPadding(held);
    const LogRing::Record three = readOrFail(ring, two.next);
    CHECK(three.position == after.position);
    CHECK(holds(three, 3, 24));
}

TEST_CASE("ring: stale bytes from earlier laps are never read as a record") {
    // 16 KiB: the smallest ring whose record limit (bytes / 16) admits the 1000-byte payload.
    LogRing ring(4 * SMALL_RING);
    const std::array<std::uint32_t, 4> sizes{24, 200, 72, 1000};
    std::uint64_t read_position = 0;
    std::uint64_t index = 0;
    int padding_skipped = 0;
    int stale_commit_words = 0;
    int stale_payloads = 0;
    while (read_position < 4 * ring.bytes()) {
        const std::uint32_t size = sizes[index % sizes.size()];
        const LogRing::Reservation reservation = reserveOrFail(ring, size, false);
        if (reservation.position >= ring.bytes()) {
            // The previous lap's commit word for this slot, still in place, is not this record's.
            const std::uint64_t old = wordAt(ring, reservation.position);
            if (old == ((reservation.position - ring.bytes()) ^ dvv::detail::LOG_COMMIT_MIX)) {
                ++stale_commit_words;
            }
            if (reservation.payload[0] != std::byte{0}) {
                ++stale_payloads;
            }
        }
        CHECK_FALSE(ring.read(reservation.position).has_value());
        CHECK_FALSE(ring.read(read_position).has_value());
        fill(reservation, index);
        CHECK_FALSE(ring.read(reservation.position).has_value());
        ring.publish(reservation);

        const LogRing::Record record = readOrFail(ring, read_position);
        if (record.position != read_position) {
            ++padding_skipped;
        }
        CHECK(record.position == reservation.position);
        CHECK(holds(record, index, size));
        read_position = record.next;
        ring.release(read_position);
        ++index;
    }
    CHECK(padding_skipped > 0);
    CHECK(stale_commit_words > 0);
    CHECK(stale_payloads > 0);
}

TEST_CASE("ring: a logged value that forges a commit word is not trusted") {
    // R4: a payload word can equal a later position's commit word. Its header is then checked
    // before read() trusts it.
    constexpr std::uint32_t FULL_PAYLOAD = (SMALL_RING / 16) - RING_HEADER;
    LogRing ring(SMALL_RING);
    std::vector<LogRing::Reservation> reservations;
    reservations.reserve(16);
    for (int i = 0; i < 16; ++i) {
        reservations.push_back(reserveOrFail(ring, FULL_PAYLOAD, true));
    }
    for (const LogRing::Reservation& reservation : reservations) {
        std::memset(reservation.payload, 0, reservation.size);
        ring.publish(reservation);
    }
    // Writes a forged header into a published record's payload at ring slot `position % bytes`.
    const auto plant = [&ring, &reservations](std::uint64_t position, std::uint32_t size,
                                              std::uint32_t kind) {
        const std::uint64_t slot = position % ring.bytes();
        const LogRing::Reservation& owner = reservations[slot / (SMALL_RING / 16)];
        std::byte* header = owner.payload + (slot - (owner.position + RING_HEADER));
        const std::uint64_t word = position ^ dvv::detail::LOG_COMMIT_MIX;
        std::memcpy(header, &word, sizeof(word));
        std::memcpy(header + 8, &size, sizeof(size));
        std::memcpy(header + 12, &kind, sizeof(kind));
    };
    constexpr std::uint32_t RECORD = 0;
    constexpr std::uint32_t PADDING = 1;

    // Below the write position: a valid forged header is read, so the forged word does reach the
    // header check.
    constexpr std::uint64_t POSITION = 32;
    plant(POSITION, 8, RECORD);
    const std::optional<LogRing::Record> control = ring.read(POSITION);
    REQUIRE(control.has_value());
    CHECK(control.value_or(LogRing::Record{}).size == 8);

    plant(POSITION, 8, 7);  // unknown kind
    CHECK_FALSE(ring.read(POSITION).has_value());
    plant(POSITION, 0xFFFFFFF0U, RECORD);  // far over the ring
    CHECK_FALSE(ring.read(POSITION).has_value());
    plant(POSITION, FULL_PAYLOAD + 1, RECORD);  // just over the record limit
    CHECK_FALSE(ring.read(POSITION).has_value());
    plant(POSITION, 0xFFFFFFF0U, PADDING);  // padding neither within the limit nor to the end
    CHECK_FALSE(ring.read(POSITION).has_value());

    // At or past the write position (the reader caught up, the slot holds last lap's bytes): even
    // a valid forged header is not read.
    REQUIRE(ring.writePosition() == SMALL_RING);
    plant(SMALL_RING + POSITION, 8, RECORD);
    CHECK_FALSE(ring.read(SMALL_RING + POSITION).has_value());

    // Close to the end: a record or padding that would cross the ring's end.
    constexpr std::uint64_t NEAR_END = SMALL_RING - 32;
    plant(NEAR_END, 100, RECORD);
    CHECK_FALSE(ring.read(NEAR_END).has_value());
    plant(NEAR_END, 100, PADDING);
    CHECK_FALSE(ring.read(NEAR_END).has_value());
}

TEST_CASE("ring: limits") {
    // Records of exactly the record limit (bytes / 16, header included).
    constexpr std::uint32_t FULL_PAYLOAD = (SMALL_RING / 16) - RING_HEADER;

    SUBCASE("Info stops at 3/4; Warn and Error fill the rest") {
        LogRing ring(SMALL_RING);
        for (int i = 0; i < 12; ++i) {
            CHECK(ring.reserve(FULL_PAYLOAD, false).has_value());
        }
        CHECK_FALSE(ring.reserve(FULL_PAYLOAD, false).has_value());
        CHECK_FALSE(ring.reserve(1, false).has_value());
        for (int i = 0; i < 4; ++i) {
            CHECK(ring.reserve(FULL_PAYLOAD, true).has_value());
        }
        CHECK_FALSE(ring.reserve(1, true).has_value());
        CHECK_FALSE(ring.reserve(1, false).has_value());
    }

    SUBCASE("a record over bytes / 16 is refused at any level") {
        LogRing ring(SMALL_RING);
        CHECK_FALSE(ring.reserve(FULL_PAYLOAD + 1, false).has_value());
        CHECK_FALSE(ring.reserve(FULL_PAYLOAD + 1, true).has_value());
        CHECK_FALSE(ring.reserve(0xFFFFFFFFU, true).has_value());
        CHECK(ring.reserve(FULL_PAYLOAD, false).has_value());
        CHECK(ring.reserve(FULL_PAYLOAD, true).has_value());
    }

    SUBCASE("the default ring is LOG_RING_BYTES") {
        const LogRing ring;
        CHECK(ring.bytes() == dvv::detail::LOG_RING_BYTES);
        CHECK(ring.releasePosition() == 0);
    }
}

TEST_CASE("ring: 8 writer threads, one reader, nothing lost or repeated") {
    constexpr std::uint32_t WRITERS = 8;
    constexpr std::uint32_t PER_WRITER = 5000;
    LogRing ring(SMALL_RING);
    std::atomic<std::uint32_t> published{0};
    std::atomic<std::uint32_t> released{0};

    std::vector<std::thread> writers;
    writers.reserve(WRITERS);
    for (std::uint32_t thread = 0; thread < WRITERS; ++thread) {
        writers.emplace_back([&ring, &published, &released, thread] {
            for (std::uint32_t sequence = 0; sequence < PER_WRITER; ++sequence) {
                // 8 to 104 bytes, so some records need padding at the ring's end.
                const std::uint32_t size = 8 + ((sequence % 5) * 24);
                const bool urgent = sequence % 3 == 0;
                for (;;) {
                    const std::uint32_t seen = released.load();
                    const std::optional<LogRing::Reservation> reservation =
                        ring.reserve(size, urgent);
                    if (!reservation) {
                        released.wait(seen);
                        continue;
                    }
                    std::memcpy(reservation->payload, &thread, sizeof(thread));
                    std::memcpy(reservation->payload + 4, &sequence, sizeof(sequence));
                    for (std::uint32_t i = 8; i < size; ++i) {
                        reservation->payload[i] = patternByte(thread + sequence, i);
                    }
                    ring.publish(*reservation);
                    published.fetch_add(1);
                    published.notify_one();
                    break;
                }
            }
        });
    }

    std::array<std::uint32_t, WRITERS> next_sequence{};
    std::uint32_t received = 0;
    int errors = 0;
    std::uint64_t position = 0;
    while (received < WRITERS * PER_WRITER) {
        const std::uint32_t seen = published.load();
        const std::optional<LogRing::Record> record = ring.read(position);
        if (!record) {
            published.wait(seen);
            continue;
        }
        std::uint32_t thread = 0;
        std::uint32_t sequence = 0;
        std::memcpy(&thread, record->payload, sizeof(thread));
        std::memcpy(&sequence, record->payload + 4, sizeof(sequence));
        if (thread >= WRITERS || sequence != next_sequence[thread] ||
            record->size != 8 + ((sequence % 5) * 24)) {
            ++errors;
        } else {
            for (std::uint32_t i = 8; i < record->size; ++i) {
                if (record->payload[i] != patternByte(thread + sequence, i)) {
                    ++errors;
                    break;
                }
            }
            ++next_sequence[thread];
        }
        ++received;
        position = record->next;
        ring.release(position);
        released.fetch_add(1);
        released.notify_all();
    }
    for (std::thread& writer : writers) {
        writer.join();
    }

    CHECK(errors == 0);
    for (std::uint32_t thread = 0; thread < WRITERS; ++thread) {
        CHECK(next_sequence[thread] == PER_WRITER);
    }
    CHECK_FALSE(ring.read(position).has_value());
}
