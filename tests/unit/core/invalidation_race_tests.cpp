#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "dbwaller/core/sharded_engine.hpp"
#include "../concurrency/test_support.hpp"

#include <atomic>

using dbwaller::core::PutOptions;
using dbwaller::core::ShardedEngine;
using namespace lifecycle_test;

namespace {
ShardedEngine::Config config() {
    ShardedEngine::Config cfg;
    cfg.compute_threads = 2;
    cfg.compute_timeout_ms = 0;
    cfg.sweep_interval_ms = 60'000;
    return cfg;
}

auto result(std::string value) {
    PutOptions opt;
    opt.ttl_ms = 60'000;
    opt.tags = {"post-tag"}; // Dependency becomes known only after fetching.
    return std::pair{std::move(value), opt};
}
} // namespace

// These cases intentionally use the pre-ticket public API as well, so they can
// be compiled and run against the original implementation without source hooks.
TEST_CASE("invalidation fences a cold miss and admits a new flight", "[concurrency][invalidation]") {
    const bool by_tag = GENERATE(false, true);
    const bool pooled = GENERATE(false, true);
    INFO("by_tag=" << by_tag << " pooled=" << pooled);
    auto cfg = config();
    cfg.enable_compute_pool = pooled;
    Gate old_gate;
    ShardedEngine engine(cfg);
    std::atomic<int> backend_version{1};
    auto fetch = [&] { return result("version-" + std::to_string(backend_version.load())); };
    auto old = std::async(std::launch::async, [&] {
        return engine.get_or_compute("post", [&] {
            auto captured = fetch();
            old_gate.block();
            return captured;
        });
    });
    ReleaseOnExit release_old{old_gate.release};
    REQUIRE(old_gate.entered.wait());
    backend_version = 2;
    if (by_tag) engine.invalidate_tag("post-tag");
    else engine.invalidate_key("post");
    auto current = std::async(std::launch::async, [&] { return engine.get_or_compute("post", fetch); });
    ReleaseOnExit release_before_current_join{old_gate.release};
    REQUIRE(eventually([&] {
        const auto stats = engine.stats_snapshot();
        return stats.inflight_leaders == 2 || stats.inflight_waits != 0;
    }));
    REQUIRE(engine.stats_snapshot().inflight_leaders == 2);
    REQUIRE(ready(current));
    CHECK(current.get() == "version-2");
    old_gate.release.set();
    REQUIRE(ready(old));
    CHECK_THROWS_AS(old.get(), std::runtime_error);
    CHECK(engine.get("post") == "version-2");
    CHECK(engine.stats_snapshot().puts == 1);
}

TEST_CASE("invalidated refresh cannot replace a newly published value", "[concurrency][invalidation]") {
    const bool by_tag = GENERATE(false, true);
    Gate old_gate;
    ShardedEngine engine(config());
    ReleaseOnExit release_old{old_gate.release};
    auto opt = result("").second;
    opt.ttl_ms = 0;
    opt.swr_ms = 60'000;
    engine.put("post", "stale", opt);
    engine.trigger_refresh("post", [&] { old_gate.block(); return result("version-1"); });
    REQUIRE(old_gate.entered.wait());
    if (by_tag) engine.invalidate_tag("post-tag");
    else engine.invalidate_key("post");
    engine.put("post", "version-2", result("").second);
    const auto puts = engine.stats_snapshot().puts;
    old_gate.release.set();
    engine.shutdown();
    CHECK(engine.get("post") == "version-2");
    CHECK(engine.stats_snapshot().puts == puts);
}

TEST_CASE("old refresh completion preserves the new flight and its waiters", "[concurrency][invalidation]") {
    const bool old_fails = GENERATE(false, true);
    Gate old_gate, new_gate;
    ShardedEngine engine(config());
    ReleaseOnExit release_old{old_gate.release};
    ReleaseOnExit release_new{new_gate.release};
    engine.trigger_refresh("post", [&] {
        old_gate.block();
        if (old_fails) throw std::runtime_error("old upstream failure");
        return result("version-1");
    });
    REQUIRE(old_gate.entered.wait());
    std::atomic<int> unexpected_calls{0};
    auto unexpected = [&] { ++unexpected_calls; return result("unexpected"); };
    auto old_waiter = std::async(std::launch::async, [&] { return engine.get_or_compute("post", unexpected); });
    ReleaseOnExit release_before_old_waiter{old_gate.release};
    REQUIRE(eventually([&] { return engine.stats_snapshot().inflight_waits == 1; }));
    engine.invalidate_tag("post-tag"); // Still no entry or tag index.
    engine.trigger_refresh("post", [&] { new_gate.block(); return result("version-2"); });
    REQUIRE(engine.stats_snapshot().refresh_scheduled == 2);
    REQUIRE(new_gate.entered.wait());
    auto new_waiter = std::async(std::launch::async, [&] { return engine.get_or_compute("post", unexpected); });
    ReleaseOnExit release_before_new_waiter{new_gate.release};
    REQUIRE(eventually([&] { return engine.stats_snapshot().inflight_waits == 2; }));
    old_gate.release.set();
    REQUIRE(ready(old_waiter));
    CHECK_THROWS_AS(old_waiter.get(), std::runtime_error);

    // Old finish_compute has now run. A later caller must still join the new
    // refresh, rather than becoming a leader because the old flight erased it.
    auto late_waiter = std::async(std::launch::async, [&] { return engine.get_or_compute("post", unexpected); });
    ReleaseOnExit release_before_late_waiter{new_gate.release};
    REQUIRE(eventually([&] {
        return engine.stats_snapshot().inflight_waits == 3 || unexpected_calls != 0;
    }));
    CHECK(unexpected_calls == 0);
    new_gate.release.set();
    REQUIRE(ready(new_waiter));
    REQUIRE(ready(late_waiter));
    CHECK(new_waiter.get() == "version-2");
    CHECK(late_waiter.get() == "version-2");
    CHECK(engine.get("post") == "version-2");
}

TEST_CASE("old refresh failure cannot mark a replacement entry failed", "[concurrency][invalidation]") {
    const bool invalidate = GENERATE(false, true);
    Gate old_gate;
    ShardedEngine engine(config());
    ReleaseOnExit release_old{old_gate.release};
    auto opt = result("").second;
    opt.ttl_ms = 0;
    opt.stale_ttl_ms = 60'000;
    opt.serve_stale_on_error = true;
    engine.put("post", "old", opt);
    engine.trigger_refresh("post", [&]() -> std::pair<std::string, PutOptions> {
        old_gate.block();
        throw std::runtime_error("old upstream failure");
    });
    REQUIRE(old_gate.entered.wait());
    if (invalidate) engine.invalidate_tag("post-tag");
    engine.put("post", "replacement", opt);
    old_gate.release.set();
    engine.shutdown();
    const auto entry = engine.observer().get_entry({.key = "post"});
    REQUIRE(entry.has_value());
    CHECK(entry->last_refresh_ok);
    CHECK(engine.get_swr("post").kind == ShardedEngine::GetKind::Miss);
    CHECK(engine.stats_snapshot().refresh_failures == 0);
}

TEST_CASE("queued loads capture invalidation generation before callback execution", "[concurrency][invalidation]") {
    const bool refresh = GENERATE(false, true);
    auto cfg = config();
    cfg.compute_threads = 1;
    Gate occupied;
    ShardedEngine engine(cfg);
    ReleaseOnExit release{occupied.release};
    engine.trigger_refresh("occupied", [&] { occupied.block(); return result("occupied"); });
    REQUIRE(occupied.entered.wait());
    std::atomic<int> calls{0};
    auto callback = [&] { ++calls; return result("version-1"); };
    std::future<std::string> waiting;
    ReleaseOnExit release_before_waiter{occupied.release};
    if (refresh) engine.trigger_refresh("post", callback);
    else {
        waiting = std::async(std::launch::async, [&] { return engine.get_or_compute("post", callback); });
        REQUIRE(eventually([&] { return engine.stats_snapshot().inflight_leaders == 1; }));
    }
    CHECK(calls == 0);
    engine.invalidate_key("post");
    engine.put("post", "version-2", result("").second);
    occupied.release.set();
    engine.shutdown();
    if (!refresh) {
        REQUIRE(ready(waiting));
        CHECK_THROWS_AS(waiting.get(), std::runtime_error);
    }
    CHECK(engine.get("post") == "version-2");
}
