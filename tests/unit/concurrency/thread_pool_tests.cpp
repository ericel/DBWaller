#include <catch2/catch_test_macros.hpp>

#include "dbwaller/concurrency/thread_pool.hpp"
#include "test_support.hpp"

#include <atomic>
#include <memory>
#include <vector>

using dbwaller::concurrency::ThreadPool;
using namespace lifecycle_test;

TEST_CASE("pool rejects every submission after shutdown", "[concurrency][pool]") {
    ThreadPool pool(1);
    pool.shutdown();
    CHECK(pool.size() == 0);
    std::atomic<int> calls{0};
    CHECK_FALSE(pool.try_submit([&](std::stop_token) { ++calls; }));
    CHECK_FALSE(pool.try_submit_future([&] { ++calls; return 42; }).has_value());
    CHECK_THROWS_AS(pool.submit_or_throw([&](std::stop_token) { ++calls; }), std::runtime_error);
    CHECK(calls == 0);
    CHECK_NOTHROW(pool.shutdown());
}

TEST_CASE("pool drains queued futures before shutdown returns", "[concurrency][pool]") {
    Gate gate;
    Signal stopping;
    ThreadPool pool(1, 1);
    ReleaseOnExit release{gate.release};
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> weak = owner;
    REQUIRE(pool.try_submit([&](std::stop_token token) {
        std::stop_callback stopped(token, [&] { stopping.set(); });
        gate.block();
    }));
    REQUIRE(gate.entered.wait());
    auto queued = pool.try_submit_future([owner] { return *owner; });
    owner.reset();
    REQUIRE(queued.has_value());
    CHECK_FALSE(pool.try_submit_future([] { return -1; }).has_value());

    auto closing = std::async(std::launch::async, [&] { pool.shutdown(); });
    ReleaseOnExit release_before_join{gate.release};
    REQUIRE(stopping.wait());
    CHECK(closing.wait_for(0ms) == std::future_status::timeout);
    CHECK_FALSE(pool.try_submit_future([] { return -2; }).has_value());
    gate.release.set();
    REQUIRE(ready(closing));
    closing.get();
    // The pool remains alive: destruction must not be what settles this future.
    REQUIRE(queued->wait_for(0ms) == std::future_status::ready);
    CHECK(queued->get() == 42);
    CHECK(weak.expired());
}

TEST_CASE("pool shutdown is concurrent and repeatable", "[concurrency][pool]") {
    Gate gate;
    Signal stopping;
    ThreadPool pool(1, 4);
    ReleaseOnExit release{gate.release};
    REQUIRE(pool.try_submit([&](std::stop_token token) {
        std::stop_callback stopped(token, [&] { stopping.set(); });
        gate.block();
    }));
    REQUIRE(gate.entered.wait());
    auto queued = pool.try_submit_future([] { return 17; });
    REQUIRE(queued.has_value());
    std::vector<std::future<void>> closers;
    for (int i = 0; i < 4; ++i) {
        closers.push_back(std::async(std::launch::async, [&] { pool.shutdown(); }));
    }
    ReleaseOnExit release_before_join{gate.release};
    REQUIRE(stopping.wait());
    // Exercise size concurrently with join/cleanup as well as submission rejection.
    CHECK(pool.size() <= 1);
    CHECK_FALSE(pool.try_submit([](std::stop_token) {}));
    gate.release.set();
    for (auto& closer : closers) { REQUIRE(ready(closer)); closer.get(); }
    REQUIRE(queued->wait_for(0ms) == std::future_status::ready);
    CHECK(queued->get() == 17);
    CHECK(pool.size() == 0);
    pool.shutdown();
}

TEST_CASE("pool destruction drains accepted work", "[concurrency][pool]") {
    std::future<int> result;
    {
        ThreadPool pool(1);
        auto accepted = pool.try_submit_future([] { return 9; });
        REQUIRE(accepted.has_value());
        result = std::move(*accepted);
    }
    REQUIRE(result.wait_for(0ms) == std::future_status::ready);
    CHECK(result.get() == 9);
}

TEST_CASE("pool survives throwing tasks and propagates future exceptions", "[concurrency][pool]") {
    ThreadPool pool(1);
    pool.submit_or_throw([](std::stop_token) { throw std::runtime_error("fire and forget"); });
    auto bad = pool.try_submit_future([]() -> int { throw std::runtime_error("future failure"); });
    auto good = pool.try_submit_future([] { return 3; });
    REQUIRE(bad.has_value());
    REQUIRE(good.has_value());
    pool.shutdown();
    CHECK_THROWS_AS(bad->get(), std::runtime_error);
    CHECK(good->get() == 3);
}
