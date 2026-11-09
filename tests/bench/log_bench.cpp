// Cost of a log call in asynchronous mode (not a test; build with --target dvv_base_log_bench in a
// Release configuration). One sink only counts. Every round submits few enough messages to fit the
// ring, and the log thread is drained between rounds outside the timing, so a row measures queued
// calls; the table shows submitted and seen counts, which are equal when nothing was dropped.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "dvv/base/log.h"

namespace {

class CountingSink final : public dvv::ILogSink {
public:
    void write(const dvv::LogRecord& /*record*/, std::string_view /*line*/) override {
        count_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t count() const { return count_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> count_{0};
};

// 16 threads x 1500 calls of about 100 bytes stay well under the 3 MiB that Info may use.
constexpr std::size_t CALLS_PER_THREAD = 1500;
constexpr int ROUNDS = 200;

struct Result {
    double ns_per_call = 0.0;
    std::uint64_t submitted = 0;
};

template <class Body>
Result measure(unsigned threads, Body body) {
    double total_ns = 0.0;
    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<bool> go{false};
        std::vector<std::thread> workers;
        std::vector<double> elapsed(threads, 0.0);
        for (unsigned index = 0; index < threads; ++index) {
            workers.emplace_back([&, index] {
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                const auto begin = std::chrono::steady_clock::now();
                for (std::size_t call = 0; call < CALLS_PER_THREAD; ++call) {
                    body(call);
                }
                const auto end = std::chrono::steady_clock::now();
                elapsed[index] = std::chrono::duration<double, std::nano>(end - begin).count();
            });
        }
        go.store(true, std::memory_order_release);
        for (std::thread& worker : workers) {
            worker.join();
        }
        for (const double value : elapsed) {
            total_ns += value;
        }
        dvv::Logger::instance().flush();  // drain between rounds, not timed
    }
    const std::uint64_t calls = static_cast<std::uint64_t>(threads) * CALLS_PER_THREAD * ROUNDS;
    return {total_ns / static_cast<double>(calls), calls};
}

}  // namespace

int main() {
    dvv::Logger& logger = dvv::Logger::instance();
    const auto sink = std::make_shared<CountingSink>();
    logger.addSink(sink);
    logger.setLevel(dvv::LogLevel::Info);
    logger.start();

    const std::string text(40, 'x');
    std::printf("hardware threads: %u\n", std::thread::hardware_concurrency());
    std::printf("%-8s %10s | %10s %9s %9s | %10s %9s %9s\n", "threads", "disabled", "3 integers",
                "submitted", "seen", "40B string", "submitted", "seen");
    for (const unsigned threads : {1U, 4U, 16U}) {
        const Result disabled = measure(threads, [](std::size_t call) {
            DVV_LOG_DEBUG("value {}", static_cast<std::int64_t>(call));
        });
        std::uint64_t before = sink->count();
        const Result integers = measure(threads, [](std::size_t call) {
            const auto value = static_cast<std::int64_t>(call);
            DVV_LOG_INFO("{} {} {}", value, value + 1, value + 2);
        });
        const std::uint64_t integers_seen = sink->count() - before;
        before = sink->count();
        const Result string =
            measure(threads, [&text](std::size_t /*call*/) { DVV_LOG_INFO("{}", text); });
        const std::uint64_t string_seen = sink->count() - before;
        std::printf("%-8u %7.1f ns | %7.1f ns %9llu %9llu | %7.1f ns %9llu %9llu\n", threads,
                    disabled.ns_per_call, integers.ns_per_call,
                    static_cast<unsigned long long>(integers.submitted),
                    static_cast<unsigned long long>(integers_seen), string.ns_per_call,
                    static_cast<unsigned long long>(string.submitted),
                    static_cast<unsigned long long>(string_seen));
    }
    logger.stop();
    return 0;
}
