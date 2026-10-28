#include <doctest/doctest.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "dvv/base/executors.h"
#include "dvv/base/when_all.h"

namespace {

dvv::Task<int> value(int v) {
    co_return v;
}

dvv::Task<std::string> text(std::string t) {
    co_return t;
}

dvv::Task<int> squareOn(dvv::IExecutor& executor, int v) {
    co_await dvv::resumeOn(executor);
    const int square = v * v;
    co_return square;
}

dvv::Task<int> failing() {
    throw std::runtime_error("child failed");
    co_return 0;
}

dvv::Task<std::tuple<int, std::string>> pair() {
    co_return co_await dvv::whenAll(value(1), text("two"));
}

dvv::Task<std::vector<int>> squares(dvv::IExecutor& executor, int count) {
    std::vector<dvv::Task<int>> tasks;
    tasks.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        tasks.push_back(squareOn(executor, i));
    }
    co_return co_await dvv::whenAll(std::move(tasks));
}

dvv::Task<std::tuple<int, int>> withFailure() {
    co_return co_await dvv::whenAll(value(1), failing());
}

}  // namespace

TEST_CASE("whenAll returns results in argument order") {
    const auto [number, word] = dvv::syncWait(pair());
    CHECK(number == 1);
    CHECK(word == "two");
}

TEST_CASE("whenAll over a vector runs tasks on a pool") {
    auto pool = dvv::ExecutorFactory::makeThreadPool(4).value();
    const auto results = dvv::syncWait(squares(*pool, 50));
    REQUIRE(results.size() == 50);
    for (int i = 0; i < 50; ++i) {
        CHECK(results[static_cast<std::size_t>(i)] == i * i);
    }
}

TEST_CASE("whenAll over an empty vector completes immediately") {
    auto pool = dvv::ExecutorFactory::makeThreadPool(1).value();
    CHECK(dvv::syncWait(squares(*pool, 0)).empty());
}

TEST_CASE("whenAll rethrows a child's exception") {
    CHECK_THROWS_AS(dvv::syncWait(withFailure()), std::runtime_error);
}

// The returned task is never awaited: whenAll must throw before it exists.
TEST_CASE("whenAll rejects an empty task at the call") {
    CHECK_THROWS_AS(static_cast<void>(dvv::whenAll(value(1), dvv::Task<int>{})),
                    std::invalid_argument);

    std::vector<dvv::Task<int>> tasks;
    tasks.push_back(value(1));
    tasks.emplace_back();
    CHECK_THROWS_AS(static_cast<void>(dvv::whenAll(std::move(tasks))), std::invalid_argument);
}
