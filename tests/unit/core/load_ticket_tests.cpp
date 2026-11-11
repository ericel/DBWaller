#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "dbwaller/core/sharded_engine.hpp"
#include "../concurrency/test_support.hpp"

#include <atomic>
#include <barrier>
#include <cstddef>

using namespace dbwaller::core;
using namespace lifecycle_test;

namespace {
ShardedEngine::Config config() {
    ShardedEngine::Config cfg;
    cfg.enable_compute_pool = false;
    cfg.sweep_interval_ms = 60'000;
    return cfg;
}
PutOptions options() {
    PutOptions opt;
    opt.ttl_ms = 60'000;
    opt.tags = {"post-tag"};
    return opt;
}
} // namespace

TEST_CASE("external load tickets fence old fetches across key and tag invalidation", "[concurrency][invalidation][ticket]") {
    const bool by_tag = GENERATE(false, true);
    const bool initially_present = GENERATE(false, true);
    Gate old_gate;
    ShardedEngine engine(config());
    auto opt = options();
    if (initially_present) engine.put("post", "initial", opt);
    std::atomic<int> backend_version{1};
    auto old = std::async(std::launch::async, [&] {
        auto ticket = engine.begin_load("post");
        const auto bytes = "version-" + std::to_string(backend_version.load());
        old_gate.block();
        return engine.put_if_current(ticket, bytes, opt);
    });
    ReleaseOnExit release{old_gate.release};
    REQUIRE(old_gate.entered.wait());
    backend_version = 2;
    if (by_tag) engine.invalidate_tag("post-tag");
    else engine.invalidate_key("post");
    CHECK_FALSE(engine.get("post").has_value());
    const auto current = engine.begin_load("post");
    CHECK(engine.put_if_current(current, "version-" + std::to_string(backend_version.load()), opt) == PublishStatus::Stored);
    const auto puts = engine.stats_snapshot().puts;
    old_gate.release.set();
    REQUIRE(ready(old));
    CHECK(old.get() == PublishStatus::Invalidated);
    CHECK(engine.stats_snapshot().puts == puts);
    CHECK(engine.get("post") == "version-2");
    engine.invalidate_tag("post-tag");
    CHECK_FALSE(engine.get("post").has_value()); // New publication indexed tags.
}

TEST_CASE("tickets own their key and cannot cross engine lifetimes", "[concurrency][invalidation][ticket]") {
    ShardedEngine engine(config());
    std::string key = "post";
    auto ticket = engine.begin_load(key);
    key = "changed";
    auto copy = ticket;
    auto moved = std::move(ticket);
    CHECK(engine.put_if_current(ticket, "moved-from", options()) == PublishStatus::Invalidated);
    CHECK(engine.put_if_current(copy, "copy", options()) == PublishStatus::Stored);
    CHECK(engine.put_if_current(moved, "moved", options()) == PublishStatus::Stored);
    CHECK(engine.get("post") == "moved");
    CHECK_FALSE(engine.get(key).has_value());
    ShardedEngine other(config());
    CHECK(other.put_if_current(copy, "wrong engine", options()) == PublishStatus::Invalidated);

    // Reusing the exact engine address must not reactivate a retained ticket.
    alignas(ShardedEngine) std::byte storage[sizeof(ShardedEngine)];
    auto* first = std::construct_at(reinterpret_cast<ShardedEngine*>(storage), config());
    auto retained = first->begin_load("post");
    std::destroy_at(first);
    auto* replacement = std::construct_at(reinterpret_cast<ShardedEngine*>(storage), config());
    const auto status = replacement->put_if_current(retained, "old lifetime", options());
    std::destroy_at(replacement);
    CHECK(status == PublishStatus::Invalidated);
}

TEST_CASE("global invalidation fences unrelated loads without restricting deliberate puts", "[concurrency][invalidation][ticket]") {
    ShardedEngine engine(config());
    const auto old = engine.begin_load("post");
    for (int i = 0; i < 100; ++i) engine.invalidate_tag("absent-tag");
    CHECK(engine.put_if_current(old, "old", options()) == PublishStatus::Invalidated);
    engine.put("post", "deliberate", options());
    CHECK(engine.get("post") == "deliberate");
    const auto current = engine.begin_load("post");
    engine.put("post", "another deliberate write", options());
    // Unconditional put is explicitly not an invalidation fence.
    CHECK(engine.put_if_current(current, "current load", options()) == PublishStatus::Stored);
    CHECK(engine.get("post") == "current load");
}

TEST_CASE("invalidated computations report a distinct error in both execution modes", "[concurrency][invalidation][ticket]") {
    const bool pooled = GENERATE(false, true);
    auto cfg = config();
    cfg.enable_compute_pool = pooled;
    ShardedEngine engine(cfg);
    CHECK_THROWS_AS(engine.get_or_compute("post", [&] {
        engine.invalidate_key("post"); // No locks may span a backend callback.
        return std::pair{std::string("invalidated"), options()};
    }), LoadInvalidatedError);
    CHECK_FALSE(engine.get("post").has_value());
    CHECK(engine.get_or_compute("post", [] { return std::pair{std::string("new"), options()}; }) == "new");
}

TEST_CASE("publication and invalidation serialize their complete cache effects", "[concurrency][invalidation][ticket]") {
    const bool by_tag = GENERATE(false, true);
    ShardedEngine engine(config());
    for (int i = 0; i < 32; ++i) {
        const auto ticket = engine.begin_load("post");
        std::barrier start(2);
        auto publisher = std::async(std::launch::async, [&] {
            start.arrive_and_wait();
            return engine.put_if_current(ticket, "value", options());
        });
        start.arrive_and_wait();
        if (by_tag) engine.invalidate_tag("post-tag");
        else engine.invalidate_key("post");
        REQUIRE(ready(publisher));
        const auto status = publisher.get();
        CHECK((status == PublishStatus::Stored || status == PublishStatus::Invalidated));
        CHECK_FALSE(engine.get("post").has_value()); // Valid for either ordering.
    }
}

TEST_CASE("snapshot tickets cannot certify a read made before invalidation", "[concurrency][invalidation][ticket]") {
    ShardedEngine engine(config());
    for (int i = 0; i < 32; ++i) {
        engine.put("post", "old", options());
        std::barrier start(2);
        auto reader = std::async(std::launch::async, [&] {
            start.arrive_and_wait();
            return engine.get_snapshot_and_begin_load("post");
        });
        start.arrive_and_wait();
        engine.invalidate_key("post");
        engine.put("post", "new", options());
        REQUIRE(ready(reader));
        auto [snapshot, ticket] = reader.get();
        const auto result = engine.put_if_current(ticket, "derived", options());
        if (snapshot.result.value == "old") CHECK(result == PublishStatus::Invalidated);
        else CHECK(result == PublishStatus::Stored); // Read occurred after the invalidation.
    }
    // Also exercise the observed ordering deterministically, without a race.
    engine.put("post", "before", options());
    auto [snapshot, ticket] = engine.get_snapshot_and_begin_load("post");
    REQUIRE(snapshot.result.value == "before");
    engine.invalidate_tag("post-tag");
    CHECK(engine.put_if_current(ticket, "derived from before", options()) == PublishStatus::Invalidated);
}

TEST_CASE("finite entry budgets bound empty values across shards", "[concurrency][capacity]") {
    const size_t shards = GENERATE(1u, 4u, 16u);
    auto cfg = config();
    cfg.num_shards = shards;
    cfg.max_entries_total = 7;
    ShardedEngine bounded(cfg);
    for (int i = 0; i < 100; ++i) bounded.put("key-" + std::to_string(i), "", options());
    size_t retained = 0;
    for (int i = 0; i < 100; ++i) retained += bounded.get("key-" + std::to_string(i)).has_value();
    CHECK(retained <= 7);
    CHECK(bounded.stats_snapshot().evictions >= 93);
    cfg.max_entries_total = 0;
    ShardedEngine unlimited(cfg);
    for (int i = 0; i < 100; ++i) unlimited.put("key-" + std::to_string(i), "", options());
    for (int i = 0; i < 100; ++i) CHECK(unlimited.get("key-" + std::to_string(i)).has_value());
}

TEST_CASE("finite entry mode enforces the byte budget on publication", "[concurrency][capacity]") {
    auto cfg = config();
    cfg.num_shards = 1;
    cfg.max_entries_total = 1000;
    cfg.max_bytes_total = 8;
    ShardedEngine engine(cfg);
    for (int i = 0; i < 10; ++i) engine.put("key-" + std::to_string(i), "1234", options());
    size_t retained_bytes = 0;
    for (int i = 0; i < 10; ++i) {
        if (auto value = engine.get("key-" + std::to_string(i))) retained_bytes += value->size();
    }
    CHECK(retained_bytes <= 8);
    // A successful publication is subject to eviction even when that entry alone
    // exceeds the budget. It must not leave the shard over budget until a sweep.
    const auto ticket = engine.begin_load("oversized");
    REQUIRE(engine.put_if_current(ticket, std::string(9, 'x'), options()) == PublishStatus::Stored);
    CHECK_FALSE(engine.get("oversized").has_value());
}
