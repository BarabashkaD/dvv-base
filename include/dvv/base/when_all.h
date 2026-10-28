#pragma once

// whenAll: starts several tasks and resumes when all have finished. The result order matches the
// argument order. If a task throws, the first exception (in argument order) is rethrown. An empty
// task (default-constructed or moved from) makes whenAll throw std::invalid_argument at the call.

#include <algorithm>
#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "dvv/base/task.h"

namespace dvv {

namespace detail {

struct WhenAllCounter {
    std::atomic<std::size_t> remaining;
    std::coroutine_handle<> parent;
};

class WhenAllChild {
public:
    struct promise_type {
        WhenAllCounter* counter = nullptr;

        WhenAllChild get_return_object() noexcept {
            return WhenAllChild{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
        [[nodiscard]] auto final_suspend() const noexcept {
            struct Awaiter {
                [[nodiscard]] bool await_ready() const noexcept { return false; }
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> handle) const noexcept {
                    WhenAllCounter* counter = handle.promise().counter;
                    if (counter->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        return counter->parent;
                    }
                    return std::noop_coroutine();
                }
                void await_resume() const noexcept {}
            };
            return Awaiter{};
        }
        void return_void() const noexcept {}
        void unhandled_exception() const noexcept { std::terminate(); }
    };

    explicit WhenAllChild(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}
    WhenAllChild(WhenAllChild&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    WhenAllChild& operator=(WhenAllChild&&) = delete;
    WhenAllChild(const WhenAllChild&) = delete;
    WhenAllChild& operator=(const WhenAllChild&) = delete;
    ~WhenAllChild() {
        if (handle_) {
            handle_.destroy();
        }
    }

    void start(WhenAllCounter& counter) {
        handle_.promise().counter = &counter;
        handle_.resume();
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

template <class T>
WhenAllChild runWhenAllChild(Task<T> task, std::optional<T>& slot, std::exception_ptr& error) {
    try {
        slot.emplace(co_await std::move(task));
    } catch (...) {
        error = std::current_exception();
    }
}

// Starts every child; the extra count held here keeps the parent suspended until all are started.
class WhenAllStart {
public:
    WhenAllStart(std::span<WhenAllChild> children, WhenAllCounter& counter) noexcept
        : children_(children), counter_(counter) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) noexcept {
        counter_.parent = parent;
        for (WhenAllChild& child : children_) {
            child.start(counter_);
        }
        if (counter_.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            return parent;
        }
        return std::noop_coroutine();
    }
    void await_resume() const noexcept {}

private:
    std::span<WhenAllChild> children_;
    WhenAllCounter& counter_;
};

template <class... T>
Task<std::tuple<T...>> whenAllTuple(Task<T>... tasks) {
    std::tuple<std::optional<T>...> slots;
    std::array<std::exception_ptr, sizeof...(T)> errors{};
    detail::WhenAllCounter counter{sizeof...(T) + 1, {}};
    auto children = [&]<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<detail::WhenAllChild, sizeof...(T)>{
            detail::runWhenAllChild(std::move(tasks), std::get<I>(slots), errors[I])...};
    }(std::index_sequence_for<T...>{});
    co_await detail::WhenAllStart(children, counter);
    for (const std::exception_ptr& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    co_return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return std::tuple<T...>(std::move(*std::get<I>(slots))...);
    }(std::index_sequence_for<T...>{});
}

template <class T>
Task<std::vector<T>> whenAllVector(std::vector<Task<T>> tasks) {
    std::vector<std::optional<T>> slots(tasks.size());
    std::vector<std::exception_ptr> errors(tasks.size());
    detail::WhenAllCounter counter{tasks.size() + 1, {}};
    std::vector<detail::WhenAllChild> children;
    children.reserve(tasks.size());
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        children.push_back(detail::runWhenAllChild(std::move(tasks[i]), slots[i], errors[i]));
    }
    co_await detail::WhenAllStart(children, counter);
    for (const std::exception_ptr& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    std::vector<T> values;
    values.reserve(slots.size());
    for (std::optional<T>& slot : slots) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): no error, so every child set its slot
        values.push_back(std::move(*slot));
    }
    co_return values;
}

}  // namespace detail

// The tasks are checked here, at the call, not when the returned task is awaited.
template <class... T>
Task<std::tuple<T...>> whenAll(Task<T>... tasks) {
    static_assert(sizeof...(T) > 0, "whenAll needs at least one task");
    static_assert((!std::is_void_v<T> && ...), "whenAll needs tasks with a value");
    if ((!tasks.valid() || ...)) {
        throw std::invalid_argument("whenAll: empty task");
    }
    return detail::whenAllTuple(std::move(tasks)...);
}

template <class T>
Task<std::vector<T>> whenAll(std::vector<Task<T>> tasks) {
    static_assert(!std::is_void_v<T>, "whenAll needs tasks with a value");
    if (std::ranges::any_of(tasks, [](const Task<T>& task) { return !task.valid(); })) {
        throw std::invalid_argument("whenAll: empty task");
    }
    return detail::whenAllVector(std::move(tasks));
}

}  // namespace dvv
