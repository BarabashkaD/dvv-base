#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#include "dvv/base/executors.h"
#include "dvv/base/task.h"

namespace {

bool isAlive(dvv::ObjectId id) {
    const auto objects = dvv::LifetimeRegistry::instance().liveObjects();
    return std::ranges::any_of(objects,
                               [id](const dvv::LiveObjectInfo& info) { return info.id == id; });
}

// A pool may be destroyed on one of its own (detached) workers, so after the last handle was
// dropped the destruction can still be in flight. Waits until the registry no longer lists id.
void waitUntilDestroyed(dvv::ObjectId id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (isAlive(id) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_FALSE(isAlive(id));
}

// Polls with a deadline so that a stuck executor fails the test instead of hanging it.
void waitForCount(const std::atomic<int>& count, int expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (count.load() < expected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(count.load() == expected);
}

// Test-only executor: forwards to a real executor, or throws std::bad_alloc while failing.
class SwitchableExecutor final : public dvv::IExecutor {
public:
    explicit SwitchableExecutor(std::shared_ptr<dvv::IExecutor> real) : real_(std::move(real)) {}

    void setFailing(bool failing) { failing_.store(failing); }

    // Runs once, right before the next failing post throws (set it while no post is running).
    void beforeNextThrow(std::move_only_function<void()> hook) { before_throw_ = std::move(hook); }

    void post(std::move_only_function<void()> work) override {
        if (failing_.load()) {
            if (auto hook = std::exchange(before_throw_, {})) {
                hook();
            }
            throw std::bad_alloc();
        }
        real_->post(std::move(work));
    }

private:
    std::shared_ptr<dvv::IExecutor> real_;
    std::atomic<bool> failing_{false};
    std::move_only_function<void()> before_throw_;
};

// Work item whose destructor posts one counting item to an executor; a moved-from one posts
// nothing. Calling it counts 100, which a test expecting it never to run can detect.
class PostOnDestroy {
public:
    PostOnDestroy(dvv::IExecutor& executor, std::atomic<int>& ran) noexcept
        : executor_(&executor), ran_(&ran) {}
    PostOnDestroy(PostOnDestroy&& other) noexcept
        : executor_(std::exchange(other.executor_, nullptr)), ran_(other.ran_) {}
    PostOnDestroy& operator=(PostOnDestroy&&) = delete;
    PostOnDestroy(const PostOnDestroy&) = delete;
    PostOnDestroy& operator=(const PostOnDestroy&) = delete;
    ~PostOnDestroy() {
        if (executor_ != nullptr) {
            executor_->post([ran = ran_] { ran->fetch_add(1); });
        }
    }

    void operator()() const { ran_->fetch_add(100); }

private:
    dvv::IExecutor* executor_;
    std::atomic<int>* ran_;
};

// Hops onto executor; counts a resumption and records whether the hop threw.
dvv::Task<void> hopOnto(dvv::IExecutor& executor, std::atomic<int>& resumed, bool& caught) {
    try {
        co_await dvv::resumeOn(executor);
        resumed.fetch_add(1);
    } catch (const std::bad_alloc&) {
        caught = true;
    }
}

}  // namespace

TEST_CASE("thread pool runs every posted item") {
    auto pool = dvv::ExecutorFactory::makeThreadPool(4).value();
    CHECK(pool->threadCount() == 4);
    std::atomic<int> count{0};
    std::latch done(100);
    for (int i = 0; i < 100; ++i) {
        pool->post([&] {
            count.fetch_add(1);
            done.count_down();
        });
    }
    done.wait();
    CHECK(count.load() == 100);
}

TEST_CASE("thread pool with zero threads uses the hardware concurrency") {
    auto pool = dvv::ExecutorFactory::makeThreadPool(0).value();
    CHECK(pool->threadCount() >= 1);
}

TEST_CASE("destroying a thread pool finishes the queued work") {
    std::atomic<int> count{0};
    std::promise<void> gate;
    std::shared_future<void> open = gate.get_future().share();
    // Declared before the pool so it outlives it: the pool is destroyed while its single worker is
    // still gated, and the releaser opens the gate while the destructor waits for the queue to
    // drain.
    std::jthread releaser;
    {
        auto pool = dvv::ExecutorFactory::makeThreadPool(1).value();
        pool->post([&count, open] {
            open.wait();
            count.fetch_add(1);
        });
        for (int i = 1; i < 50; ++i) {
            pool->post([&count] { count.fetch_add(1); });
        }
        releaser = std::jthread([&gate] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            gate.set_value();
        });
    }
    CHECK(count.load() == 50);
}

TEST_CASE("a thread pool may be destroyed by its own worker") {
    struct Sync {
        std::promise<void> released;
        std::latch posted{1};
    };
    auto sync = std::make_shared<Sync>();
    auto future = sync->released.get_future();
    auto pool = dvv::ExecutorFactory::makeThreadPool(2).value();
    const dvv::ObjectId id = pool->objectId();
    pool->post([owner = pool, sync]() mutable {
        sync->posted.wait();  // run only after the test dropped its own reference
        owner.reset();        // last reference: the pool is destroyed on this worker thread
        sync->released.set_value();
    });
    pool.reset();
    sync->posted.count_down();
    future.wait();
    CHECK_FALSE(isAlive(id));
}

TEST_CASE("serial executor runs items in order, one at a time") {
    dvv::ObjectId pool_id = 0;
    dvv::ObjectId serial_id = 0;
    {
        auto pool = dvv::ExecutorFactory::makeThreadPool(4).value();
        pool_id = pool->objectId();
        auto serial = dvv::ExecutorFactory::makeSerialExecutor(pool).value();
        serial_id = serial->objectId();
        CHECK(serial->ownerId() == pool->objectId());
        std::vector<int> order;  // unsynchronised on purpose: the executor serialises access
        std::atomic<int> in_flight{0};
        std::atomic<int> overlaps{0};
        std::latch done(1000);
        for (int i = 0; i < 1000; ++i) {
            serial->post([&, i] {
                if (in_flight.fetch_add(1) != 0) {
                    overlaps.fetch_add(1);
                }
                // Keep the item busy so that a non-serial executor would overlap here.
                for (int spin = 0; spin < 20; ++spin) {
                    std::this_thread::yield();
                }
                order.push_back(i);
                in_flight.fetch_sub(1);
                done.count_down();
            });
        }
        done.wait();
        CHECK(overlaps.load() == 0);
        REQUIRE(order.size() == 1000);
        CHECK(std::ranges::is_sorted(order));
    }
    waitUntilDestroyed(serial_id);
    waitUntilDestroyed(pool_id);
}

TEST_CASE("a work item posted from inside a serial item runs after the current one") {
    dvv::ObjectId pool_id = 0;
    dvv::ObjectId serial_id = 0;
    {
        auto pool = dvv::ExecutorFactory::makeThreadPool(4).value();
        pool_id = pool->objectId();
        auto serial = dvv::ExecutorFactory::makeSerialExecutor(pool).value();
        serial_id = serial->objectId();
        std::vector<int> order;  // unsynchronised on purpose
        std::latch done(2);
        serial->post([&, serial_ptr = serial.get()] {
            order.push_back(1);
            serial_ptr->post([&] {
                order.push_back(3);
                done.count_down();
            });
            order.push_back(2);  // the nested item must not run inline
            done.count_down();
        });
        done.wait();
        CHECK(order == std::vector<int>{1, 2, 3});
    }
    waitUntilDestroyed(serial_id);
    waitUntilDestroyed(pool_id);
}

TEST_CASE("a serial executor may target another serial executor") {
    dvv::ObjectId pool_id = 0;
    dvv::ObjectId inner_id = 0;
    dvv::ObjectId outer_id = 0;
    {
        auto pool = dvv::ExecutorFactory::makeThreadPool(2).value();
        pool_id = pool->objectId();
        auto inner = dvv::ExecutorFactory::makeSerialExecutor(pool).value();
        auto outer = dvv::ExecutorFactory::makeSerialExecutor(inner).value();
        inner_id = inner->objectId();
        outer_id = outer->objectId();
        CHECK(outer->ownerId() == inner->objectId());
        std::vector<int> order;  // unsynchronised on purpose
        std::latch done(100);
        for (int i = 0; i < 100; ++i) {
            outer->post([&, i] {
                order.push_back(i);
                done.count_down();
            });
        }
        done.wait();
        REQUIRE(order.size() == 100);
        CHECK(std::ranges::is_sorted(order));
    }
    waitUntilDestroyed(outer_id);
    waitUntilDestroyed(inner_id);
    waitUntilDestroyed(pool_id);
}

TEST_CASE("a serial executor recovers after scheduling on its target failed") {
    dvv::ObjectId pool_id = 0;
    dvv::ObjectId serial_id = 0;
    {
        auto pool = dvv::ExecutorFactory::makeThreadPool(2).value();
        pool_id = pool->objectId();
        auto target = std::make_shared<SwitchableExecutor>(pool);
        auto serial = dvv::ExecutorFactory::makeSerialExecutor(target).value();
        serial_id = serial->objectId();
        CHECK(serial->ownerId() == dvv::NO_OWNER);  // the double is not a managed type

        std::vector<int> order;  // unsynchronised on purpose
        std::atomic<int> ran{0};
        target->setFailing(true);
        // The post throws, so its item was not queued and must never run.
        CHECK_THROWS_AS(serial->post([&] {
            order.push_back(1);
            ran.fetch_add(1);
        }),
                        std::bad_alloc);
        target->setFailing(false);
        serial->post([&] {
            order.push_back(2);
            ran.fetch_add(1);
        });
        // The flag is not stuck: later posts keep scheduling drains.
        serial->post([&] {
            order.push_back(3);
            ran.fetch_add(1);
        });
        waitForCount(ran, 2);
        // A last item flushes the queue, so a wrongly kept item would have run by now.
        serial->post([&] { ran.fetch_add(1); });
        waitForCount(ran, 3);
        CHECK(order == std::vector<int>{2, 3});
    }
    waitUntilDestroyed(serial_id);
    waitUntilDestroyed(pool_id);
}

TEST_CASE("a failed serial post keeps the items posted concurrently, in order") {
    dvv::ObjectId pool_id = 0;
    dvv::ObjectId serial_id = 0;
    {
        auto pool = dvv::ExecutorFactory::makeThreadPool(2).value();
        pool_id = pool->objectId();
        auto target = std::make_shared<SwitchableExecutor>(pool);
        auto serial = dvv::ExecutorFactory::makeSerialExecutor(target).value();
        serial_id = serial->objectId();

        std::vector<int> order;  // unsynchronised on purpose
        std::atomic<int> ran{0};
        auto item = [&](int value) {
            return [&, value] {
                order.push_back(value);
                ran.fetch_add(1);
            };
        };
        target->setFailing(true);
        // While item 1's post is scheduling the drain, item 2 is posted "concurrently": it is
        // queued behind item 1 and leaves the scheduling to that post. Item 1's post then fails.
        target->beforeNextThrow([&] { serial->post(item(2)); });
        CHECK_THROWS_AS(serial->post(item(1)), std::bad_alloc);
        // Item 2 is still queued, so item 3 lands behind it; this post fails too and must remove
        // its own item, not the one in front of it.
        CHECK_THROWS_AS(serial->post(item(3)), std::bad_alloc);
        target->setFailing(false);
        serial->post(item(4));  // schedules the drain for the waiting item 2 as well
        waitForCount(ran, 2);
        serial->post([&] { ran.fetch_add(1); });  // flush
        waitForCount(ran, 3);
        CHECK(order == std::vector<int>{2, 4});
    }
    waitUntilDestroyed(serial_id);
    waitUntilDestroyed(pool_id);
}

TEST_CASE("a failed serial post destroys its item outside the executor lock") {
    dvv::ObjectId pool_id = 0;
    dvv::ObjectId serial_id = 0;
    {
        auto pool = dvv::ExecutorFactory::makeThreadPool(2).value();
        pool_id = pool->objectId();
        auto target = std::make_shared<SwitchableExecutor>(pool);
        auto serial = dvv::ExecutorFactory::makeSerialExecutor(target).value();
        serial_id = serial->objectId();

        std::atomic<int> ran{0};
        target->setFailing(true);
        // Only this scheduling attempt fails, so the post from the removed item's destructor
        // succeeds. Destroying the item under the executor lock would self-deadlock that post.
        target->beforeNextThrow([&target] { target->setFailing(false); });
        CHECK_THROWS_AS(serial->post(PostOnDestroy(*serial, ran)), std::bad_alloc);
        waitForCount(ran, 1);                     // the destructor's item ran, the removed one not
        serial->post([&] { ran.fetch_add(1); });  // flush
        waitForCount(ran, 2);
    }
    waitUntilDestroyed(serial_id);
    waitUntilDestroyed(pool_id);
}

TEST_CASE("resumeOn on an executor whose post throws resumes nothing later") {
    dvv::ObjectId pool_id = 0;
    dvv::ObjectId serial_id = 0;
    {
        auto pool = dvv::ExecutorFactory::makeThreadPool(2).value();
        pool_id = pool->objectId();
        auto target = std::make_shared<SwitchableExecutor>(pool);
        auto serial = dvv::ExecutorFactory::makeSerialExecutor(target).value();
        serial_id = serial->objectId();

        std::atomic<int> resumed{0};
        bool caught = false;
        target->setFailing(true);
        // The exception is rethrown inside the coroutine, which then finishes and is destroyed.
        dvv::syncWait(hopOnto(*serial, resumed, caught));
        CHECK(caught);
        CHECK(resumed.load() == 0);

        // Nothing may resume the destroyed coroutine afterwards: flush the executor.
        target->setFailing(false);
        std::atomic<int> flushed{0};
        serial->post([&] { flushed.fetch_add(1); });
        waitForCount(flushed, 1);
        CHECK(resumed.load() == 0);
    }
    waitUntilDestroyed(serial_id);
    waitUntilDestroyed(pool_id);
}

TEST_CASE("serial executor needs a target") {
    const auto result = dvv::ExecutorFactory::makeSerialExecutor(nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == dvv::Errc::Internal);
}
