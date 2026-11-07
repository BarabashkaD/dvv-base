#include <doctest/doctest.h>

#include <memory>
#include <type_traits>
#include <utility>

#include "dvv/base/move_only_function.h"

namespace {

// Counts destructions of the objects it was moved from and into, so a test can check that a
// stored callable is destroyed exactly once.
class DestroyCounter {
public:
    explicit DestroyCounter(int* destroyed) : destroyed_(destroyed) {}
    DestroyCounter(DestroyCounter&& other) noexcept
        : destroyed_(std::exchange(other.destroyed_, nullptr)) {}
    DestroyCounter& operator=(DestroyCounter&&) = delete;
    ~DestroyCounter() {
        if (destroyed_ != nullptr) {
            ++*destroyed_;
        }
    }
    void operator()() const {}

private:
    int* destroyed_;
};

}  // namespace

TEST_CASE_TEMPLATE("MoveOnlyFunction", F, dvv::detail::MoveOnlyFunction<int()>,
                   dvv::MoveOnlyFunction<int()>) {
    SUBCASE("holds a lambda that owns a unique_ptr") {
        auto owned = std::make_unique<int>(42);
        F function([p = std::move(owned)] { return *p; });
        REQUIRE(static_cast<bool>(function));
        CHECK(function() == 42);
    }
    SUBCASE("is not copyable") {
        CHECK_FALSE(std::is_copy_constructible_v<F>);
        CHECK_FALSE(std::is_copy_assignable_v<F>);
        CHECK(std::is_nothrow_move_constructible_v<F>);
    }
    SUBCASE("move construction leaves the source empty") {
        F source([] { return 7; });
        F target(std::move(source));
        CHECK_FALSE(static_cast<bool>(source));  // NOLINT(bugprone-use-after-move)
        REQUIRE(static_cast<bool>(target));
        CHECK(target() == 7);
    }
    SUBCASE("move assignment leaves the source empty") {
        F source([] { return 8; });
        F target([] { return 1; });
        target = std::move(source);
        CHECK_FALSE(static_cast<bool>(source));  // NOLINT(bugprone-use-after-move)
        REQUIRE(static_cast<bool>(target));
        CHECK(target() == 8);
    }
    SUBCASE("default constructed is empty") {
        F function;
        CHECK_FALSE(static_cast<bool>(function));
        CHECK(function == nullptr);
    }
    SUBCASE("assigning a callable") {
        F function;
        function = [] { return 1; };
        REQUIRE(static_cast<bool>(function));
        CHECK(function() == 1);
    }
    SUBCASE("assigning a callable that owns a unique_ptr") {
        F function([] { return 0; });
        function = [p = std::make_unique<int>(9)] { return *p; };
        CHECK(function() == 9);
    }
    SUBCASE("assigning nullptr empties") {
        F function([] { return 3; });
        function = nullptr;
        CHECK_FALSE(static_cast<bool>(function));
    }
}

TEST_CASE_TEMPLATE("MoveOnlyFunction destroys the target once", F,
                   dvv::detail::MoveOnlyFunction<void()>, dvv::MoveOnlyFunction<void()>) {
    int destroyed = 0;
    SUBCASE("on scope exit, after moves") {
        {
            F first{DestroyCounter(&destroyed)};
            F second(std::move(first));
            F third;
            third = std::move(second);
            CHECK(destroyed == 0);
        }
        CHECK(destroyed == 1);
    }
    SUBCASE("on nullptr assignment") {
        F function{DestroyCounter(&destroyed)};
        function = nullptr;
        CHECK(destroyed == 1);
    }
    SUBCASE("on move assignment over a full target") {
        int other_destroyed = 0;
        F target{DestroyCounter(&other_destroyed)};
        F source{DestroyCounter(&destroyed)};
        target = std::move(source);
        CHECK(other_destroyed == 1);
        CHECK(destroyed == 0);
        target = nullptr;
        CHECK(destroyed == 1);
    }
}

TEST_CASE_TEMPLATE("MoveOnlyFunction forwards move-only arguments", F,
                   dvv::detail::MoveOnlyFunction<int(std::unique_ptr<int>)>,
                   dvv::MoveOnlyFunction<int(std::unique_ptr<int>)>) {
    F function([](std::unique_ptr<int> p) { return *p; });
    CHECK(function(std::make_unique<int>(5)) == 5);
}

TEST_CASE_TEMPLATE("MoveOnlyFunction passes references through", F,
                   dvv::detail::MoveOnlyFunction<void(int&)>, dvv::MoveOnlyFunction<void(int&)>) {
    F function([](int& value) { ++value; });
    int value = 1;
    function(value);
    CHECK(value == 2);
}

TEST_CASE_TEMPLATE("MoveOnlyFunction supports noexcept signatures", F,
                   dvv::detail::MoveOnlyFunction<int(const int&) noexcept>,
                   dvv::MoveOnlyFunction<int(const int&) noexcept>) {
    F function([](const int& value) noexcept { return value * 2; });
    static_assert(noexcept(function(0)));
    CHECK(function(4) == 8);
    CHECK(function != nullptr);
}

TEST_CASE("dvv::detail::MoveOnlyFunction rejects a throwing callable for a noexcept signature") {
    using Function = dvv::detail::MoveOnlyFunction<void() noexcept>;
    struct Throwing {
        void operator()() {}
    };
    struct NoThrow {
        void operator()() noexcept {}
    };
    static_assert(!std::is_constructible_v<Function, Throwing>);
    static_assert(std::is_constructible_v<Function, NoThrow>);
}

TEST_CASE(
    "dvv::detail::MoveOnlyFunction rejects a target that is not callable with the signature") {
    using Function = dvv::detail::MoveOnlyFunction<int(int)>;
    static_assert(!std::is_constructible_v<Function, int>);
    static_assert(std::is_constructible_v<Function, int (*)(int)>);
}
