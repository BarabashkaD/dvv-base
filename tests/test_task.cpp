#include <doctest/doctest.h>

#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include "dvv/base/executors.h"
#include "dvv/base/task.h"

namespace {

dvv::Task<int> answer() {
    co_return 42;
}

dvv::Task<int> addOne(int value) {
    co_return value + 1;
}

dvv::Task<int> chained() {
    const int first = co_await answer();
    co_return co_await addOne(first);
}

dvv::Task<void> setFlag(bool& flag) {
    flag = true;
    co_return;
}

dvv::Task<int> throws() {
    throw std::runtime_error("boom");
    co_return 0;
}

dvv::Task<std::unique_ptr<int>> moveOnly() {
    co_return std::make_unique<int>(5);
}

dvv::Task<int> one() {
    co_return 1;
}

dvv::Task<long> manySequential(int count) {
    long sum = 0;
    for (int i = 0; i < count; ++i) {
        sum += co_await one();
    }
    co_return sum;
}

dvv::Task<std::thread::id> threadAfterResume(dvv::IExecutor& executor) {
    co_await dvv::resumeOn(executor);
    co_return std::this_thread::get_id();
}

dvv::Result<int> parse(std::string_view text) {
    if (text == "21") {
        return 21;
    }
    return dvv::makeError(dvv::Errc::Format, "not 21");
}

dvv::Result<void> check(bool ok) {
    if (ok) {
        return {};
    }
    return dvv::makeError(dvv::Errc::Unsupported, "check failed");
}

struct Guard {
    int& destroyed;
    ~Guard() { ++destroyed; }
};

dvv::Task<dvv::Result<int>> doubledAsync(std::string_view text, bool& reached_end, int& destroyed) {
    Guard guard{destroyed};
    const int value = co_try(parse(text));
    reached_end = true;
    co_return value * 2;
}

dvv::Task<dvv::Result<int>> checkedAsync(bool ok) {
    co_try(check(ok));
    co_return 1;
}

dvv::Task<dvv::Result<int>> outerAsync(std::string_view text) {
    bool reached_end = false;
    int destroyed = 0;
    const int value = co_try(co_await doubledAsync(text, reached_end, destroyed));
    co_return value + 1;
}

}  // namespace

TEST_CASE("syncWait returns the task result") {
    CHECK(dvv::syncWait(answer()) == 42);
    CHECK(dvv::syncWait(chained()) == 43);
}

TEST_CASE("void tasks run") {
    bool flag = false;
    dvv::syncWait(setFlag(flag));
    CHECK(flag);
}

TEST_CASE("tasks are lazy") {
    bool flag = false;
    {
        auto task = setFlag(flag);
        CHECK(task.valid());
    }
    CHECK_FALSE(flag);
}

TEST_CASE("exceptions propagate through syncWait") {
    CHECK_THROWS_AS(dvv::syncWait(throws()), std::runtime_error);
}

TEST_CASE("syncWait rejects an empty task") {
    CHECK_THROWS_AS(dvv::syncWait(dvv::Task<int>{}), std::invalid_argument);
    auto task = answer();
    auto moved = std::move(task);
    // NOLINTNEXTLINE(bugprone-use-after-move): an empty task is the point of this check
    CHECK_THROWS_AS(dvv::syncWait(std::move(task)), std::invalid_argument);
    CHECK(dvv::syncWait(std::move(moved)) == 42);
}

TEST_CASE("move-only results") {
    auto value = dvv::syncWait(moveOnly());
    REQUIRE(value != nullptr);
    CHECK(*value == 5);
}

TEST_CASE("long chains of synchronous awaits do not grow the stack") {
    constexpr int COUNT = 200000;
    CHECK(dvv::syncWait(manySequential(COUNT)) == COUNT);
}

TEST_CASE("resumeOn continues on the executor") {
    auto pool = dvv::ExecutorFactory::makeThreadPool(1).value();
    const auto id = dvv::syncWait(threadAfterResume(*pool));
    CHECK(id != std::this_thread::get_id());
}

TEST_CASE("co_try unwraps a value") {
    bool reached_end = false;
    int destroyed = 0;
    const auto result = dvv::syncWait(doubledAsync("21", reached_end, destroyed));
    CHECK(result.value() == 42);
    CHECK(reached_end);
    CHECK(destroyed == 1);
}

TEST_CASE("co_try completes the task with the error and destroys locals") {
    bool reached_end = false;
    int destroyed = 0;
    const auto result = dvv::syncWait(doubledAsync("x", reached_end, destroyed));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == dvv::Errc::Format);
    CHECK_FALSE(reached_end);
    CHECK(destroyed == 1);
}

TEST_CASE("co_try works with Result<void> and nested tasks") {
    CHECK(dvv::syncWait(checkedAsync(true)).value() == 1);
    CHECK(dvv::syncWait(checkedAsync(false)).error().code == dvv::Errc::Unsupported);
    CHECK(dvv::syncWait(outerAsync("21")).value() == 43);
    CHECK(dvv::syncWait(outerAsync("y")).error().message == "not 21");
}
