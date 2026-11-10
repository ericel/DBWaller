#include <catch2/catch_test_macros.hpp>

#include "dbwaller/core/sharded_engine.hpp"
#include "../concurrency/test_support.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

using dbwaller::core::PutOptions;
using dbwaller::core::ShardedEngine;
using namespace lifecycle_test;

namespace {
ShardedEngine::Config config() {
    ShardedEngine::Config cfg;
    cfg.compute_threads = 1;
    cfg.compute_max_queue = 1;
    cfg.compute_timeout_ms = 0;
    cfg.sweep_interval_ms = 10;
    return cfg;
}

auto value(std::string bytes) {
    PutOptions opt;
    opt.ttl_ms = 60'000;
    opt.tags = {"computed"};
    return std::pair{std::move(bytes), opt};
}

// Observe admission through public refresh counters, not private locks/test hooks.
bool wait_for_closed_admission(ShardedEngine& engine) {
    unsigned sequence = 0;
    return eventually([&] {
        const auto before = engine.stats_snapshot().refresh_scheduled;
        engine.trigger_refresh("probe-" + std::to_string(sequence++), [] { return value("probe"); });
        return engine.stats_snapshot().refresh_scheduled == before;
    });
}
} // namespace

TEST_CASE("saturated compute queue preserves large inline callbacks", "[concurrency][engine][saturation]") {
    for (const bool refresh : {false, true}) {
        INFO("refresh=" << refresh);
        Gate gate;
        ShardedEngine engine(config());
        ReleaseOnExit release{gate.release};
        engine.trigger_refresh("active", [&] { gate.block(); return value("active"); });
        REQUIRE(gate.entered.wait());
        engine.trigger_refresh("queued", [] { return value("queued"); });
        std::array<char, 4096> payload;
        payload.fill('x');
        std::atomic<int> calls{0};
        auto callback = [payload, &calls] {
            ++calls;
            return value(std::string(payload.data(), payload.size()));
        };
        if (refresh) engine.trigger_refresh("fallback", callback);
        else CHECK(engine.get_or_compute("fallback", callback) == std::string(4096, 'x'));
        CHECK(calls == 1);
        CHECK(engine.get("fallback") == std::string(4096, 'x'));
        CHECK(engine.stats_snapshot().compute_queue_rejects == 1);
        gate.release.set();
        engine.shutdown();
        CHECK(engine.get("queued") == "queued");
        engine.invalidate_tag("computed");
        CHECK_FALSE(engine.get("fallback").has_value());
    }
}

TEST_CASE("fail-fast saturation skips callbacks and keeps stale values", "[concurrency][engine][saturation]") {
    Gate gate;
    auto cfg = config();
    cfg.backpressure = ShardedEngine::BackpressureMode::FailFast;
    ShardedEngine engine(cfg);
    ReleaseOnExit release{gate.release};
    engine.trigger_refresh("active", [&] { gate.block(); return value("active"); });
    REQUIRE(gate.entered.wait());
    engine.trigger_refresh("queued", [] { return value("queued"); });
    PutOptions stale;
    stale.ttl_ms = 0;
    stale.swr_ms = 60'000;
    engine.put("stale", "old", stale);
    int calls = 0;
    CHECK(engine.get_or_compute("miss", [&] { ++calls; return value("unexpected"); }).empty());
    engine.trigger_refresh("stale", [&] { ++calls; return value("unexpected"); });
    CHECK(calls == 0);
    CHECK(engine.get_swr("stale").value == "old");
    CHECK(engine.stats_snapshot().compute_queue_rejects == 2);
    CHECK(engine.stats_snapshot().refresh_failures == 1);
    gate.release.set();
    engine.shutdown();
}

TEST_CASE("engine rejects new compute after shutdown in every mode", "[concurrency][engine][lifecycle]") {
    for (const bool pooled : {false, true}) {
        for (const auto pressure : {ShardedEngine::BackpressureMode::RunInline, ShardedEngine::BackpressureMode::FailFast}) {
            auto cfg = config();
            cfg.enable_compute_pool = pooled;
            cfg.backpressure = pressure;
            ShardedEngine engine(cfg);
            engine.put("cached", "retained", value("").second);
            engine.shutdown();
            int calls = 0;
            CHECK(engine.get_or_compute("new", [&] { ++calls; return value("unexpected"); }).empty());
            engine.trigger_refresh("new", [&] { ++calls; return value("unexpected"); });
            CHECK(calls == 0);
            CHECK_FALSE(engine.get("new").has_value());
            CHECK(engine.get("cached") == "retained");
            engine.shutdown();
        }
    }
}

TEST_CASE("pool-disabled compute executes inline with either backpressure policy", "[concurrency][engine][lifecycle]") {
    for (const auto pressure : {ShardedEngine::BackpressureMode::RunInline, ShardedEngine::BackpressureMode::FailFast}) {
        auto cfg = config();
        cfg.enable_compute_pool = false;
        cfg.backpressure = pressure;
        ShardedEngine engine(cfg);
        int calls = 0;
        CHECK(engine.get_or_compute("miss", [&] { ++calls; return value("inline"); }) == "inline");
        engine.trigger_refresh("refresh", [&] { ++calls; return value("refreshed"); });
        CHECK(calls == 2);
        CHECK(engine.get("refresh") == "refreshed");
        CHECK(engine.stats_snapshot().compute_queue_rejects == 0);
    }
}

TEST_CASE("engine shutdown waits for pool-disabled callbacks", "[concurrency][engine][lifecycle]") {
    for (const bool refresh : {false, true}) {
        Gate gate;
        auto cfg = config();
        cfg.enable_compute_pool = false;
        ShardedEngine engine(cfg);
        auto work = std::async(std::launch::async, [&] {
            auto callback = [&] { gate.block(); return value("finished"); };
            if (refresh) { engine.trigger_refresh("key", callback); return std::string("finished"); }
            return engine.get_or_compute("key", callback);
        });
        ReleaseOnExit release{gate.release};
        REQUIRE(gate.entered.wait());
        auto closing = std::async(std::launch::async, [&] { engine.shutdown(); });
        ReleaseOnExit release_before_join{gate.release};
        CHECK(wait_for_closed_admission(engine));
        CHECK(closing.wait_for(0ms) == std::future_status::timeout);
        gate.release.set();
        REQUIRE(ready(work));
        REQUIRE(ready(closing));
        CHECK(work.get() == "finished");
        closing.get();
        CHECK(engine.get("key") == "finished");
    }
}

TEST_CASE("engine shutdown waits for saturated inline work", "[concurrency][engine][lifecycle]") {
    Gate pool_gate, inline_gate;
    ShardedEngine engine(config());
    ReleaseOnExit release_pool{pool_gate.release};
    engine.trigger_refresh("active", [&] { pool_gate.block(); return value("active"); });
    REQUIRE(pool_gate.entered.wait());
    engine.trigger_refresh("queued", [] { return value("queued"); });
    auto work = std::async(std::launch::async, [&] {
        return engine.get_or_compute("inline", [&] { inline_gate.block(); return value("inline"); });
    });
    ReleaseOnExit release_inline{inline_gate.release};
    REQUIRE(inline_gate.entered.wait());
    auto closing = std::async(std::launch::async, [&] { engine.shutdown(); });
    ReleaseOnExit release_before_join{inline_gate.release};
    ReleaseOnExit release_pool_before_join{pool_gate.release};
    CHECK(wait_for_closed_admission(engine));
    pool_gate.release.set();
    CHECK(closing.wait_for(0ms) == std::future_status::timeout);
    inline_gate.release.set();
    REQUIRE(ready(work));
    REQUIRE(ready(closing));
    CHECK(work.get() == "inline");
    closing.get();
    CHECK(engine.get("queued") == "queued");
}

TEST_CASE("shared waiters and queued work settle during concurrent engine shutdown", "[concurrency][engine][lifecycle]") {
    for (const bool failure : {false, true}) {
        Gate gate;
        ShardedEngine engine(config());
        auto leader = std::async(std::launch::async, [&] {
            return engine.get_or_compute("shared", [&] {
                gate.block();
                if (failure) throw std::runtime_error("backend unavailable");
                return value("shared result");
            });
        });
        ReleaseOnExit release{gate.release};
        REQUIRE(gate.entered.wait());
        std::atomic<int> extra_calls{0};
        auto follower = std::async(std::launch::async, [&] {
            return engine.get_or_compute("shared", [&] { ++extra_calls; return value("wrong"); });
        });
        ReleaseOnExit release_before_follower{gate.release};
        REQUIRE(eventually([&] { return engine.stats_snapshot().inflight_waits == 1; }));
        engine.trigger_refresh("queued", [] { return value("queued"); });
        std::vector<std::future<void>> closers;
        for (int i = 0; i < 3; ++i) closers.push_back(std::async(std::launch::async, [&] { engine.shutdown(); }));
        ReleaseOnExit release_before_closers{gate.release};
        CHECK(wait_for_closed_admission(engine));
        for (auto& closer : closers) CHECK(closer.wait_for(0ms) == std::future_status::timeout);
        gate.release.set();
        REQUIRE(ready(leader));
        REQUIRE(ready(follower));
        CHECK(leader.get() == (failure ? "" : "shared result"));
        CHECK(follower.get() == (failure ? "" : "shared result"));
        CHECK(extra_calls == 0);
        for (auto& closer : closers) { REQUIRE(ready(closer)); closer.get(); }
        CHECK(engine.get("queued") == "queued");
        engine.shutdown();
    }
}

TEST_CASE("miss callers can join a refresh and observe its failure", "[concurrency][engine]") {
    Gate gate;
    ShardedEngine engine(config());
    ReleaseOnExit release{gate.release};
    engine.trigger_refresh("shared", [&]() -> std::pair<std::string, PutOptions> {
        gate.block();
        throw std::runtime_error("refresh failed");
    });
    REQUIRE(gate.entered.wait());
    std::atomic<int> calls{0};
    auto waiter = std::async(std::launch::async, [&] {
        return engine.get_or_compute("shared", [&] { ++calls; return value("unexpected"); });
    });
    ReleaseOnExit release_before_waiter{gate.release};
    REQUIRE(eventually([&] { return engine.stats_snapshot().inflight_waits == 1; }));
    gate.release.set();
    REQUIRE(ready(waiter));
    CHECK(waiter.get().empty());
    CHECK(calls == 0);
    CHECK(engine.get_or_compute("shared", [] { return value("recovered"); }) == "recovered");
}

TEST_CASE("a rejected refresh settles callers already sharing its flight", "[concurrency][engine][saturation]") {
    struct Cleanup {
        std::atomic<bool> armed{false};
        std::atomic<bool> timed_out{false};
        Signal entered, release;
    };
    struct Callback {
        std::shared_ptr<Cleanup> cleanup;
        ~Callback() {
            // Pause destruction of the rejected queue argument. This is a user
            // callable's resource cleanup, not an engine-only test hook.
            if (cleanup->armed.exchange(false)) {
                cleanup->entered.set();
                cleanup->timed_out = !cleanup->release.wait();
            }
        }
        auto operator()() const { return value("unexpected"); }
    };
    Gate gate;
    auto cfg = config();
    cfg.backpressure = ShardedEngine::BackpressureMode::FailFast;
    cfg.compute_timeout_ms = 10'000;
    ShardedEngine engine(cfg);
    ReleaseOnExit release{gate.release};
    engine.trigger_refresh("active", [&] { gate.block(); return value("active"); });
    REQUIRE(gate.entered.wait());
    engine.trigger_refresh("queued", [] { return value("queued"); });

    auto cleanup = std::make_shared<Cleanup>();
    std::function<std::pair<std::string, PutOptions>()> callback = Callback{cleanup};
    cleanup->armed = true;
    auto refresh = std::async(std::launch::async, [&] { engine.trigger_refresh("shared", std::move(callback)); });
    ReleaseOnExit release_cleanup{cleanup->release};
    REQUIRE(cleanup->entered.wait());
    std::atomic<int> calls{0};
    auto waiter = std::async(std::launch::async, [&] {
        return engine.get_or_compute("shared", [&] { ++calls; return value("unexpected"); });
    });
    ReleaseOnExit release_before_waiter{cleanup->release};
    REQUIRE(eventually([&] { return engine.stats_snapshot().inflight_waits == 1; }));
    cleanup->release.set();
    REQUIRE(ready(refresh));
    refresh.get();
    REQUIRE(ready(waiter));
    CHECK(waiter.get().empty());
    CHECK(engine.stats_snapshot().compute_timeouts == 0);
    CHECK(calls == 0);
    CHECK_FALSE(cleanup->timed_out);
    gate.release.set();
    engine.shutdown();
}

TEST_CASE("callback copy failure retires the single-flight entry", "[concurrency][engine]") {
    struct ThrowingCopy {
        std::shared_ptr<bool> armed;
        explicit ThrowingCopy(std::shared_ptr<bool> flag) : armed(std::move(flag)) {}
        ThrowingCopy(const ThrowingCopy& other) : armed(other.armed) {
            if (*armed) throw std::runtime_error("copy failed");
        }
        auto operator()() const { return value("unused"); }
    };
    auto cfg = config();
    cfg.compute_timeout_ms = 1;
    cfg.enable_compute_pool = false;
    ShardedEngine engine(cfg);
    auto armed = std::make_shared<bool>(false);
    std::function<std::pair<std::string, PutOptions>()> callback = ThrowingCopy(armed);
    *armed = true;
    std::string result;
    CHECK_NOTHROW(result = engine.get_or_compute("key", callback));
    CHECK(result.empty());
    *armed = false;
    CHECK(engine.get_or_compute("key", [] { return value("recovered"); }) == "recovered");
}

TEST_CASE("engine shutdown wakes a sleeping sweeper", "[concurrency][engine][lifecycle]") {
    auto cfg = config();
    cfg.sweep_interval_ms = 3'600'000;
    ShardedEngine engine(cfg);
    auto closing = std::async(std::launch::async, [&] { engine.shutdown(); });
    REQUIRE(ready(closing));
    closing.get();
}
