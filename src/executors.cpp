#include "dvv/base/executors.h"

#include <algorithm>
#include <condition_variable>
#include <format>
#include <new>
#include <stop_token>
#include <system_error>

namespace dvv {

struct ThreadPool::State {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<std::move_only_function<void()>> queue;
};

void ThreadPool::runWorker(State& state, const std::stop_token& stop) {
    for (;;) {
        std::move_only_function<void()> work;
        {
            std::unique_lock lock(state.mutex);
            state.wake.wait(lock, stop, [&state] { return !state.queue.empty(); });
            if (state.queue.empty()) {
                return;  // stop requested and the queue is drained
            }
            work = std::move(state.queue.front());
            state.queue.pop_front();
        }
        work();
    }
}

ThreadPool::ThreadPool(CreationKey<ExecutorFactory> key, std::size_t thread_count)
    : Managed(std::move(key)), state_(std::make_shared<State>()) {
    threads_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        threads_.emplace_back(
            [state = state_](const std::stop_token& stop) { runWorker(*state, stop); });
    }
}

ThreadPool::~ThreadPool() {
    for (auto& thread : threads_) {
        thread.request_stop();
    }
    for (auto& thread : threads_) {
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();  // the last owner was released on a worker; it keeps the state alive
        } else if (thread.joinable()) {
            thread.join();
        }
    }
}

void ThreadPool::post(std::move_only_function<void()> work) {
    {
        std::lock_guard lock(state_->mutex);
        state_->queue.push_back(std::move(work));
    }
    state_->wake.notify_one();
}

SerialExecutor::SerialExecutor(CreationKey<ExecutorFactory> key,
                               std::shared_ptr<IExecutor> target) noexcept
    : Managed(std::move(key)), target_(std::move(target)) {}

void SerialExecutor::post(std::move_only_function<void()> work) {
    bool schedule = false;
    std::size_t index = 0;
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(work));
        index = queue_.size() - 1;
        schedule = !scheduled_;
        scheduled_ = true;
    }
    if (schedule) {
        try {
            target_->post([self = shared_from_this()] { self->drain(); });
        } catch (...) {
            // Scheduling the drain on the target failed (in practice an allocation failure). Keep
            // the contract of IExecutor::post: remove our item, so it never runs. The index is
            // still valid: no drain runs while this call holds scheduled_ (none was running when
            // we set it, and we failed to schedule one), so nothing pops from the front; items
            // posted concurrently meanwhile were only appended behind ours. They stay queued, in
            // order, and wait for the next successful post to schedule a drain. Clearing the flag
            // lets that post do so instead of wedging for good. Erasing does not throw (moving a
            // move_only_function is noexcept); taking the lock could throw std::system_error
            // (practically unreachable). The removed item is destroyed only after the lock is
            // released, because its captures' destructors may post to this executor again.
            std::move_only_function<void()> removed;
            {
                std::lock_guard lock(mutex_);
                const auto slot = queue_.begin() + static_cast<std::ptrdiff_t>(index);
                removed = std::move(*slot);
                queue_.erase(slot);
                scheduled_ = false;
            }
            throw;
        }
    }
}

void SerialExecutor::drain() {
    for (;;) {
        std::move_only_function<void()> work;
        {
            std::lock_guard lock(mutex_);
            if (queue_.empty()) {
                scheduled_ = false;
                return;
            }
            work = std::move(queue_.front());
            queue_.pop_front();
        }
        work();
    }
}

Result<std::shared_ptr<ThreadPool>> ExecutorFactory::makeThreadPool(std::size_t thread_count,
                                                                    std::source_location where) {
    if (thread_count == 0) {
        thread_count = std::max(1U, std::thread::hardware_concurrency());
    }
    try {
        return std::make_shared<ThreadPool>(
            CreationKey<ExecutorFactory>(
                LifetimeRegistry::instance().issue<ThreadPool>(NO_OWNER, where)),
            thread_count);
    } catch (const std::bad_alloc&) {
        return makeError(Errc::OutOfMemory, "cannot allocate the thread pool", where);
    } catch (const std::system_error& error) {
        return makeError(
            Errc::Internal,
            std::format("cannot start {} worker threads: {}", thread_count, error.what()), where);
    }
}

Result<std::shared_ptr<SerialExecutor>> ExecutorFactory::makeSerialExecutor(
    std::shared_ptr<IExecutor> target, std::source_location where) {
    if (!target) {
        return makeError(Errc::Internal, "serial executor needs a target executor", where);
    }
    const auto* parent = dynamic_cast<const ManagedObject*>(target.get());
    const ObjectId owner = parent != nullptr ? parent->objectId() : NO_OWNER;
    try {
        return std::make_shared<SerialExecutor>(
            CreationKey<ExecutorFactory>(
                LifetimeRegistry::instance().issue<SerialExecutor>(owner, where)),
            std::move(target));
    } catch (const std::bad_alloc&) {
        return makeError(Errc::OutOfMemory, "cannot allocate the serial executor", where);
    }
}

}  // namespace dvv
