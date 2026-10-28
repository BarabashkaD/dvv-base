#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <source_location>
#include <stop_token>
#include <thread>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/lifetime.h"

namespace dvv {

class ExecutorFactory;

// Fixed-size pool of worker threads. The destructor finishes all queued work, then joins.
//
// The last owner may be released on one of the pool's own workers (for example by a work item that
// holds the only remaining reference). That worker cannot join itself, so it is detached: the
// destructor then returns while that worker is still finishing the queued work, and the shared
// queue state stays alive until it is done. Consequently a work item must never wait for the
// thread that destroys the pool.
class ThreadPool final : public IExecutor, public Managed<ThreadPool> {
public:
    ThreadPool(CreationKey<ExecutorFactory> key, std::size_t thread_count);
    ~ThreadPool() override;

    void post(std::move_only_function<void()> work) override;
    [[nodiscard]] std::size_t threadCount() const noexcept { return threads_.size(); }

private:
    struct State;
    static void runWorker(State& state, const std::stop_token& stop);

    std::shared_ptr<State> state_;  // shared with the workers so a worker may destroy the pool
    std::vector<std::jthread> threads_;
};

// Runs posted work one item at a time, in order, on a target executor (an actor's mailbox).
//
// If post() throws (in practice an allocation failure, while queuing the item or while scheduling
// the drain on the target), nothing of that call is queued and its item will never run, as
// IExecutor::post requires. Items posted concurrently while a failing post was scheduling stay
// queued, in order, and wait until the next successful post schedules a drain. Recovery takes the
// internal lock, which could itself throw std::system_error (practically unreachable); then the
// failed item stays queued and the executor stays wedged: it never runs any item again, so the
// failed item still never runs.
class SerialExecutor final : public IExecutor,
                             public Managed<SerialExecutor>,
                             public std::enable_shared_from_this<SerialExecutor> {
public:
    SerialExecutor(CreationKey<ExecutorFactory> key, std::shared_ptr<IExecutor> target) noexcept;

    void post(std::move_only_function<void()> work) override;

private:
    void drain();

    std::shared_ptr<IExecutor> target_;  // strong reference up to the parent executor
    std::mutex mutex_;
    std::deque<std::move_only_function<void()>> queue_;
    bool scheduled_ = false;
};

// Creation authority for executors.
class ExecutorFactory {
public:
    // thread_count 0 means one thread per hardware thread.
    [[nodiscard]] static Result<std::shared_ptr<ThreadPool>> makeThreadPool(
        std::size_t thread_count = 0, std::source_location where = std::source_location::current());

    [[nodiscard]] static Result<std::shared_ptr<SerialExecutor>> makeSerialExecutor(
        std::shared_ptr<IExecutor> target,
        std::source_location where = std::source_location::current());
};

}  // namespace dvv
