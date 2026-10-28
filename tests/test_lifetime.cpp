#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "dvv/base/leak_check.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/testing/test_authority.h"

namespace {

class Widget final : public dvv::Managed<Widget> {
public:
    Widget(dvv::CreationKey<dvv::test::TestAuthority> key, int value) noexcept
        : Managed(std::move(key)), value_(value) {}
    [[nodiscard]] int value() const noexcept { return value_; }

private:
    int value_;
};

class NotFinal : public dvv::Managed<NotFinal> {
public:
    explicit NotFinal(dvv::CreationKey<dvv::test::TestAuthority> key) noexcept
        : Managed(std::move(key)) {}
};

bool isLive(dvv::ObjectId id) {
    const auto objects = dvv::LifetimeRegistry::instance().liveObjects();
    return std::ranges::any_of(objects,
                               [id](const dvv::LiveObjectInfo& info) { return info.id == id; });
}

std::int64_t liveOfType(std::string_view name) {
    for (const auto& stats : dvv::LifetimeRegistry::instance().typeStats()) {
        if (stats.name.find(name) != std::string_view::npos) {
            return stats.live;
        }
    }
    return 0;
}

}  // namespace

static_assert(dvv::ManagedType<Widget>);
static_assert(!dvv::ManagedType<NotFinal>);
static_assert(!std::is_copy_constructible_v<Widget>);
static_assert(!std::is_move_constructible_v<Widget>);

TEST_CASE("managed objects are registered while alive") {
    auto& registry = dvv::LifetimeRegistry::instance();
    const std::size_t before = registry.liveCount();
    auto widget = dvv::test::TestAuthority::makeUnique<Widget>(7);
    CHECK(widget->value() == 7);
    CHECK(widget->objectId() != dvv::NO_OWNER);
    CHECK(widget->typeName().find("Widget") != std::string_view::npos);
    CHECK(registry.liveCount() == before + 1);
    CHECK(isLive(widget->objectId()));
    CHECK(liveOfType("Widget") == 1);

    const dvv::ObjectId id = widget->objectId();
    widget.reset();
    CHECK(registry.liveCount() == before);
    CHECK_FALSE(isLive(id));
    CHECK(liveOfType("Widget") == 0);
}

TEST_CASE("object ids are unique") {
    auto first = dvv::test::TestAuthority::makeShared<Widget>(1);
    auto second = dvv::test::TestAuthority::makeShared<Widget>(2);
    CHECK(first->objectId() != second->objectId());
}

TEST_CASE("detailed mode records the creation location") {
    auto& registry = dvv::LifetimeRegistry::instance();
    const bool previous = registry.detailed();
    registry.setDetailed(false);
    auto plain = dvv::test::TestAuthority::makeUnique<Widget>(1);
    registry.setDetailed(true);
    auto detailed = dvv::test::TestAuthority::makeUnique<Widget>(2);
    int found = 0;
    for (const auto& info : registry.liveObjects()) {
        if (info.id == plain->objectId()) {
            ++found;
            CHECK_FALSE(info.createdAt.has_value());
        }
        if (info.id == detailed->objectId()) {
            ++found;
            CHECK(info.createdAt.has_value());
            CHECK(info.createdTime.has_value());
        }
    }
    registry.setDetailed(previous);
    REQUIRE(found == 2);
}

TEST_CASE("a moved-from lifetime token cannot create another object") {
    auto first = dvv::LifetimeRegistry::instance().issue<Widget>();
    REQUIRE(first.type() != nullptr);
    const dvv::ObjectId id = first.id();
    dvv::LifetimeToken second(std::move(first));
    CHECK(first.type() == nullptr);  // NOLINT(bugprone-use-after-move)
    CHECK(second.type() != nullptr);
    CHECK(second.id() == id);
}

TEST_CASE("concurrent creation and destruction keeps the count consistent") {
    auto& registry = dvv::LifetimeRegistry::instance();
    const std::size_t before = registry.liveCount();
    std::vector<std::jthread> threads;
    threads.reserve(8);
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([] {
            std::vector<std::unique_ptr<Widget>> widgets;
            widgets.reserve(500);
            for (int i = 0; i < 500; ++i) {
                widgets.push_back(dvv::test::TestAuthority::makeUnique<Widget>(i));
            }
        });
    }
    threads.clear();
    CHECK(registry.liveCount() == before);
}

TEST_CASE("leak check reports live objects and applies the policy") {
    CHECK(dvv::finishWithLeakCheck(0, dvv::LeakPolicy::Fail) == 0);
    auto widget = dvv::test::TestAuthority::makeUnique<Widget>(3);
    CHECK(dvv::finishWithLeakCheck(0, dvv::LeakPolicy::Fail) == 1);
    CHECK(dvv::finishWithLeakCheck(5, dvv::LeakPolicy::Fail) == 5);
    CHECK(dvv::finishWithLeakCheck(0, dvv::LeakPolicy::Warn) == 0);
    const auto report = dvv::formatLeakReport(dvv::LifetimeRegistry::instance().liveObjects());
    CHECK(report.find("Widget") != std::string::npos);
    CHECK(report.find("#" + std::to_string(widget->objectId())) != std::string::npos);
}
