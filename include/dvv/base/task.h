#pragma once

// Coroutine task layer (spec §4): Task<T>, syncWait, resumeOn and co_try. Capped by design: no
// generators, no custom allocators. Tasks are lazy, move-only handles; awaiting one starts it and
// resumes the awaiting coroutine by symmetric transfer when it finishes.

#include <condition_variable>
#include <coroutine>
#include <exception>
#include <expected>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "dvv/base/error.h"
#include "dvv/base/executor.h"

namespace dvv {

template <class T = void>
class Task;

namespace detail {

template <class T>
inline constexpr bool IS_RESULT = false;
template <class U>
inline constexpr bool IS_RESULT<std::expected<U, Error>> = true;

class PromiseBase {
public:
    struct FinalAwaiter {
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> handle) const noexcept {
            return handle.promise().continuationOrNoop();
        }
        void await_resume() const noexcept {}
    };

    [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
    [[nodiscard]] FinalAwaiter final_suspend() const noexcept { return {}; }

    void setContinuation(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
    }
    [[nodiscard]] std::coroutine_handle<> continuationOrNoop() const noexcept {
        if (continuation_) {
            return continuation_;
        }
        return std::noop_coroutine();
    }

private:
    std::coroutine_handle<> continuation_;
};

template <class T>
class Promise : public PromiseBase {
public:
    Task<T> get_return_object() noexcept;

    template <class U = T>
        requires std::convertible_to<U&&, T>
    void return_value(U&& value) {
        result_.template emplace<1>(std::forward<U>(value));
    }

    // NOLINTNEXTLINE(bugprone-exception-escape): nothrow emplace; flagged for any variant emplace
    void unhandled_exception() noexcept { result_.template emplace<2>(std::current_exception()); }

    // Used by co_try: completes the task with an error.
    // NOLINTNEXTLINE(bugprone-exception-escape): nothrow emplace (Error moves without throwing)
    void failWith(Error error) noexcept
        requires IS_RESULT<T>
    {
        result_.template emplace<1>(std::unexpected(std::move(error)));
    }

    T takeResult() {
        if (result_.index() == 2) {
            std::rethrow_exception(std::get<2>(result_));
        }
        return std::move(std::get<1>(result_));
    }

private:
    std::variant<std::monostate, T, std::exception_ptr> result_;
};

template <>
class Promise<void> : public PromiseBase {
public:
    Task<void> get_return_object() noexcept;
    void return_void() const noexcept {}
    void unhandled_exception() noexcept { exception_ = std::current_exception(); }
    void takeResult() const {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

private:
    std::exception_ptr exception_;
};

}  // namespace detail

template <class T>
class [[nodiscard]] Task {
public:
    using promise_type = detail::Promise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(Handle handle) noexcept : handle_(handle) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { reset(); }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(handle_); }

    auto operator co_await() && noexcept { return Awaiter{handle_}; }

private:
    struct Awaiter {
        Handle handle;
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) const noexcept {
            handle.promise().setContinuation(awaiting);
            return handle;
        }
        T await_resume() const { return handle.promise().takeResult(); }
    };

    void reset() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }

    Handle handle_;
};

namespace detail {

template <class T>
Task<T> Promise<T>::get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<Promise<T>>::from_promise(*this)};
}

inline Task<void> Promise<void>::get_return_object() noexcept {
    return Task<void>{std::coroutine_handle<Promise<void>>::from_promise(*this)};
}

struct SyncSignal {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;

    void notify() {
        std::lock_guard lock(mutex);
        done = true;
        condition.notify_all();  // under the lock: the waiter may destroy us right after
    }
    void wait() {
        std::unique_lock lock(mutex);
        condition.wait(lock, [this] { return done; });
    }
};

class SyncDriver {
public:
    struct promise_type {
        SyncSignal* signal = nullptr;

        SyncDriver get_return_object() noexcept {
            return SyncDriver{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
        [[nodiscard]] auto final_suspend() const noexcept {
            struct Awaiter {
                [[nodiscard]] bool await_ready() const noexcept { return false; }
                void await_suspend(std::coroutine_handle<promise_type> handle) const noexcept {
                    handle.promise().signal->notify();
                }
                void await_resume() const noexcept {}
            };
            return Awaiter{};
        }
        void return_void() const noexcept {}
        void unhandled_exception() const noexcept { std::terminate(); }
    };

    explicit SyncDriver(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}
    SyncDriver(SyncDriver&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    SyncDriver& operator=(SyncDriver&&) = delete;
    SyncDriver(const SyncDriver&) = delete;
    SyncDriver& operator=(const SyncDriver&) = delete;
    ~SyncDriver() {
        if (handle_) {
            handle_.destroy();
        }
    }

    void start(SyncSignal& signal) {
        handle_.promise().signal = &signal;
        handle_.resume();
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

template <class T>
struct SyncResult {
    std::optional<T> value;
    std::exception_ptr error;
};

template <>
struct SyncResult<void> {
    std::exception_ptr error;
};

template <class T>
SyncDriver driveSync(Task<T> task, SyncResult<T>& result) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(task);
        } else {
            result.value.emplace(co_await std::move(task));
        }
    } catch (...) {
        result.error = std::current_exception();
    }
}

template <class T>
class TryAwaiter {
public:
    explicit TryAwaiter(Result<T> result) noexcept : result_(std::move(result)) {}

    [[nodiscard]] bool await_ready() const noexcept { return result_.has_value(); }

    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> handle) noexcept {
        handle.promise().failWith(std::move(result_).error());
        return handle.promise().continuationOrNoop();
    }

    T await_resume() {
        if constexpr (!std::is_void_v<T>) {
            return std::move(*result_);
        }
    }

private:
    Result<T> result_;
};

template <class T>
[[nodiscard]] TryAwaiter<T> tryAwait(Result<T> result) noexcept {
    return TryAwaiter<T>(std::move(result));
}

}  // namespace detail

// Blocks the calling thread until the task finishes; rethrows its exception. For main() and tests.
// Throws std::invalid_argument for an empty task (default-constructed or moved from).
template <class T>
T syncWait(Task<T> task) {
    if (!task.valid()) {
        throw std::invalid_argument("syncWait: empty task");
    }
    detail::SyncResult<T> result;
    detail::SyncSignal signal;
    auto driver = detail::driveSync(std::move(task), result);
    driver.start(signal);
    signal.wait();
    if (result.error) {
        std::rethrow_exception(result.error);
    }
    if constexpr (!std::is_void_v<T>) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): driveSync sets the value or the error
        return std::move(*result.value);
    }
}

// co_await resumeOn(executor): continues the coroutine on the executor (never inline).
[[nodiscard]] inline auto resumeOn(IExecutor& executor) noexcept {
    struct Awaiter {
        IExecutor* executor;
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) const {
            executor->post([handle] { handle.resume(); });
        }
        void await_resume() const noexcept {}
    };
    return Awaiter{&executor};
}

}  // namespace dvv

// Inside a Task<Result<U>>: unwraps a Result<T> or completes the task with its error (spec §11).
#define co_try(...) (co_await ::dvv::detail::tryAwait(__VA_ARGS__))
