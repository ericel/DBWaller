#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include "dbwaller/daemon/cache_service.hpp"
#include "../concurrency/test_support.hpp"

#include <atomic>
#include <barrier>
#include <fstream>
#include <limits>
#include <map>

using namespace dbwaller::daemon;
using namespace lifecycle_test;
using Engine = dbwaller::core::ShardedEngine;
using Kind = Engine::GetKind;

namespace {
Engine::Config config() {
    Engine::Config cfg;
    cfg.enable_compute_pool = false;
    cfg.sweep_interval_ms = 60'000;
    return cfg;
}
struct ManualClock {
    std::atomic<int64_t> ms{0};
    CacheService::Now reader() {
        return [this] { return CacheService::Clock::time_point(std::chrono::milliseconds(ms.load())); };
    }
};
const AccessContext owner{"alice", "posts"};
PublishOptions options() { return {60'000, 0, {"post-tag"}}; }

template<class F>
void expect_error(ErrorCode code, F&& call) {
    try {
        call();
        FAIL("expected a service error");
    } catch (const ServiceError& e) {
        CHECK(e.code() == code);
    }
}

const char* outcome(Publication value) {
    switch (value) {
        case Publication::Stored: return "stored";
        case Publication::Invalidated: return "invalidated";
        case Publication::Expired: return "expired";
        case Publication::Consumed: return "consumed";
        case Publication::UnknownTicket: return "unknown_ticket";
    }
    return "invalid";
}
} // namespace

TEST_CASE("service executes shared v1 conformance scenarios", "[service][protocol]") {
    std::ifstream file(DBWALLER_PROTOCOL_FIXTURES);
    REQUIRE(file.good());
    const auto fixtures = nlohmann::json::parse(file);
    REQUIRE(fixtures.at("protocol") == "dbwaller.v1");
    for (const auto& scenario : fixtures.at("scenarios")) {
        INFO(scenario.at("name"));
        ManualClock clock;
        CacheService::Limits limits;
        limits.ticket_lifetime_ms = fixtures.at("ticket_lifetime_ms");
        auto service = std::make_unique<CacheService>(config(), limits, clock.reader());
        std::map<std::string, LoadGrant> grants;
        for (const auto& step : scenario.at("steps")) {
            INFO(step.dump());
            const auto op = step.at("op").get<std::string>();
            if (op == "begin") {
                grants.emplace(step.at("as"), service->begin_load(owner, step.at("key")));
            } else if (op == "publish") {
                auto opt = options();
                opt.tags = step.value("tags", std::vector<std::string>{});
                CHECK(outcome(service->publish(owner, grants.at(step.at("ticket")).ticket,
                                              step.at("value"), opt)) == step.at("expect"));
            } else if (op == "lookup") {
                auto read = service->lookup(owner, step.at("key"));
                if (step.at("kind") == "miss") {
                    CHECK(read.snapshot.result.kind == Kind::Miss);
                    CHECK_FALSE(read.snapshot.result.value);
                } else {
                    CHECK(read.snapshot.result.kind == Kind::Fresh);
                    CHECK(read.snapshot.result.value == step.at("value").get<std::string>());
                }
            } else if (op == "invalidate_key") {
                service->invalidate_key(owner, step.at("key"));
            } else if (op == "invalidate_tag") {
                service->invalidate_tag(owner, step.at("tag"));
            } else if (op == "advance") {
                clock.ms += step.at("ms").get<int64_t>();
            } else if (op == "restart") {
                service = std::make_unique<CacheService>(config(), limits, clock.reader());
            } else {
                FAIL("unknown fixture operation");
            }
        }
    }
}

TEST_CASE("service fences blocked old loads while new loads publish", "[service][concurrency]") {
    const bool by_tag = GENERATE(false, true);
    const bool present = GENERATE(false, true);
    const bool refresh = GENERATE(false, true);
    ManualClock clock;
    CacheService service(config(), {}, clock.reader());
    if (present) {
        const auto initial = service.begin_load(owner, "post");
        CHECK(service.publish(owner, initial.ticket, "initial", options()) == Publication::Stored);
    }
    Gate gate;
    auto old = std::async(std::launch::async, [&] {
        auto load = refresh ? *service.lookup(owner, "post", true).load : service.begin_load(owner, "post");
        const std::string fetched = "v1";
        gate.block();
        return service.publish(owner, load.ticket, fetched, options());
    });
    ReleaseOnExit release{gate.release};
    REQUIRE(gate.entered.wait());
    if (by_tag) service.invalidate_tag(owner, "post-tag");
    else service.invalidate_key(owner, "post");
    const auto current = service.begin_load(owner, "post");
    CHECK(service.publish(owner, current.ticket, "v2", options()) == Publication::Stored);
    gate.release.set();
    REQUIRE(ready(old));
    CHECK(old.get() == Publication::Invalidated);
    CHECK(service.lookup(owner, "post").snapshot.result.value == "v2");
    CHECK(service.stats().engine.puts == (present ? 2 : 1));
}

TEST_CASE("service concurrent publication consumes a ticket exactly once", "[service][concurrency]") {
    ManualClock clock;
    CacheService service(config(), {}, clock.reader());
    const auto grant = service.begin_load(owner, "post");
    std::barrier start(2);
    auto first = std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        return service.publish(owner, grant.ticket, "first", options());
    });
    start.arrive_and_wait();
    const auto second = service.publish(owner, grant.ticket, "second", options());
    REQUIRE(ready(first));
    const auto result = first.get();
    CHECK(((result == Publication::Stored && second == Publication::Consumed) ||
           (result == Publication::Consumed && second == Publication::Stored)));
    CHECK(service.stats().engine.puts == 1);
    CHECK(service.lookup(owner, "post").snapshot.result.value ==
          (result == Publication::Stored ? "first" : "second"));
    service.invalidate_key(owner, "post");
    CHECK(service.publish(owner, grant.ticket, "replay", options()) == Publication::Consumed);
    CHECK(service.lookup(owner, "post").snapshot.result.kind == Kind::Miss);
}

TEST_CASE("service isolates binary namespaces keys tags and ticket owners", "[service][isolation]") {
    ManualClock clock;
    CacheService service(config(), {}, clock.reader());
    const AccessContext one{"alice", "a"};
    const AccessContext two{"alice", "ab"};
    const AccessContext other_principal{"bob", "a"};
    const std::string one_key("bc\0key", 6), two_key("c\0key", 5), bytes("\0\xff\0", 3);
    const auto first = service.begin_load(one, one_key);
    expect_error(ErrorCode::PermissionDenied, [&] { service.publish(two, first.ticket, "wrong", options()); });
    expect_error(ErrorCode::PermissionDenied, [&] { service.publish(other_principal, first.ticket, "wrong", options()); });
    auto opt = options();
    opt.tags = {std::string("t\0g", 3)};
    CHECK(service.publish(one, first.ticket, bytes, opt) == Publication::Stored);
    const auto second = service.begin_load(two, two_key);
    CHECK(service.publish(two, second.ticket, "other", opt) == Publication::Stored);
    CHECK(service.lookup(one, one_key).snapshot.result.value == bytes);
    CHECK(service.lookup(two, two_key).snapshot.result.value == "other");
    // Authorized readers in the SAME namespace share entries, but not tickets.
    CHECK(service.lookup(other_principal, one_key).snapshot.result.value == bytes);
    service.invalidate_tag(one, opt.tags.front());
    CHECK(service.lookup(one, one_key).snapshot.result.kind == Kind::Miss);
    CHECK(service.lookup(two, two_key).snapshot.result.value == "other");
    service.invalidate_key(one, two_key);
    CHECK(service.lookup(two, two_key).snapshot.result.value == "other");
    const auto unrelated = service.begin_load(two, two_key);
    service.invalidate_tag(one, "absent");
    CHECK(service.publish(two, unrelated.ticket, "new", opt) == Publication::Invalidated);
    // Global fencing crosses namespaces; removal of stored data does not.
}

TEST_CASE("service expiry uses monotonic time and rejects the exact deadline", "[service][expiry]") {
    const int64_t elapsed = GENERATE(99, 100, 101);
    ManualClock clock;
    CacheService::Limits limits;
    limits.ticket_lifetime_ms = 100;
    CacheService service(config(), limits, clock.reader());
    const auto load = service.begin_load(owner, "post");
    CHECK(load.valid_for_ms == 100);
    clock.ms = elapsed;
    CHECK(service.publish(owner, load.ticket, "value", options()) ==
          (elapsed < 100 ? Publication::Stored : Publication::Expired));
    CHECK(service.stats().engine.puts == (elapsed < 100 ? 1 : 0));
    const auto fresh = service.begin_load(owner, "post");
    CHECK(service.publish(owner, fresh.ticket, "new fetch", options()) == Publication::Stored);
}

TEST_CASE("service bounds ticket records and key bytes with incremental expiry", "[service][capacity]") {
    ManualClock clock;
    CacheService::Limits limits;
    limits.max_ticket_records = 2;
    limits.reap_per_call = 1;
    limits.ticket_lifetime_ms = 100;
    CacheService service(config(), limits, clock.reader());
    const auto one = service.begin_load(owner, "one");
    service.begin_load(owner, "two");
    expect_error(ErrorCode::ResourceExhausted, [&] { service.begin_load(owner, "three"); });
    expect_error(ErrorCode::ResourceExhausted, [&] { service.lookup(owner, "three", true); });
    CHECK(service.lookup(owner, "three").snapshot.result.kind == Kind::Miss);
    CHECK(service.publish(owner, one.ticket, "value", options()) == Publication::Stored);
    CHECK(service.stats().ticket_records == 2); // Single-use receipt occupies a bounded slot.
    CHECK(service.stats().ticket_key_bytes == 8 + owner.cache_namespace.size() + 3);
    expect_error(ErrorCode::ResourceExhausted, [&] { service.begin_load(owner, "three"); });
    clock.ms = 100;
    CHECK(service.stats().ticket_records == 1); // Reaps at most one per call.
    CHECK(service.stats().ticket_records == 0);
    CHECK(service.stats().ticket_key_bytes == 0);
    CHECK(service.publish(owner, one.ticket, "replay", options()) == Publication::UnknownTicket);
    const auto next = service.begin_load(owner, "next");
    CHECK(next.ticket != one.ticket);

    limits.max_ticket_records = 100;
    limits.max_ticket_key_bytes = 8 + owner.cache_namespace.size() + 4;
    CacheService bytes(config(), limits, clock.reader());
    const auto held = bytes.begin_load(owner, "four");
    CHECK(bytes.stats().ticket_key_bytes == limits.max_ticket_key_bytes);
    expect_error(ErrorCode::ResourceExhausted, [&] { bytes.begin_load(owner, "x"); });
    CHECK(bytes.publish(owner, held.ticket, "v", options()) == Publication::Stored);
    CHECK(bytes.stats().ticket_key_bytes == 0);
    CHECK_FALSE(bytes.begin_load(owner, "new").ticket.empty());
}

TEST_CASE("service rejects invalid inputs before consuming a valid ticket", "[service][limits]") {
    ManualClock clock;
    CacheService::Limits limits;
    limits.max_value_bytes = 4;
    limits.max_key_bytes = 4;
    limits.max_tag_bytes = 4;
    limits.max_tags = 1;
    CacheService service(config(), limits, clock.reader());
    expect_error(ErrorCode::InvalidArgument, [&] { service.begin_load(owner, ""); });
    expect_error(ErrorCode::InvalidArgument, [&] { service.lookup(owner, "long-key"); });
    expect_error(ErrorCode::InvalidArgument, [&] { service.invalidate_tag(owner, "long-tag"); });
    expect_error(ErrorCode::PermissionDenied, [&] { service.begin_load({"", "ns"}, "post"); });
    const auto grant = service.begin_load(owner, "post");
    PublishOptions opt{1000, 0, {"tag"}};
    expect_error(ErrorCode::InvalidArgument, [&] { service.publish(owner, grant.ticket, "12345", opt); });
    auto bad = opt;
    bad.ttl_ms = std::numeric_limits<uint64_t>::max();
    expect_error(ErrorCode::InvalidArgument, [&] { service.publish(owner, grant.ticket, "v", bad); });
    bad = opt;
    bad.swr_ms = limits.max_cache_lifetime_ms;
    expect_error(ErrorCode::InvalidArgument, [&] { service.publish(owner, grant.ticket, "v", bad); });
    bad = opt;
    bad.tags = {"a", "b"};
    expect_error(ErrorCode::InvalidArgument, [&] { service.publish(owner, grant.ticket, "v", bad); });
    bad.tags = {""};
    expect_error(ErrorCode::InvalidArgument, [&] { service.publish(owner, grant.ticket, "v", bad); });
    expect_error(ErrorCode::InvalidArgument, [&] { service.publish(owner, "not-a-ticket", "v", opt); });
    CHECK(service.stats().engine.puts == 0);
    CHECK(service.publish(owner, grant.ticket, "1234", opt) == Publication::Stored);
    limits.ticket_lifetime_ms = 0;
    CHECK_THROWS_AS(CacheService(config(), limits, clock.reader()), ServiceError);
}

TEST_CASE("service lookup reports misses fresh empty bytes and stale snapshots", "[service][snapshot]") {
    ManualClock clock;
    CacheService service(config(), {}, clock.reader());
    auto miss = service.lookup(owner, "post", true);
    REQUIRE(miss.load);
    CHECK(miss.snapshot.result.kind == Kind::Miss);
    CHECK(miss.snapshot.fresh_remaining_ms == 0);
    CHECK(miss.snapshot.servable_remaining_ms == 0);
    CHECK(service.publish(owner, miss.load->ticket, "", options()) == Publication::Stored);
    auto fresh = service.lookup(owner, "post", true);
    CHECK(fresh.snapshot.result.kind == Kind::Fresh);
    CHECK(fresh.snapshot.result.value == "");
    CHECK(fresh.snapshot.fresh_remaining_ms > 0);
    CHECK(fresh.snapshot.fresh_remaining_ms <= 60'000);
    CHECK(fresh.snapshot.servable_remaining_ms == fresh.snapshot.fresh_remaining_ms);
    REQUIRE(fresh.load);
    PublishOptions swr{0, 60'000, {}}; // Immediately stale, without sleeping.
    CHECK(service.publish(owner, fresh.load->ticket, "stale", swr) == Publication::Stored);
    auto stale = service.lookup(owner, "post", true);
    CHECK(stale.snapshot.result.kind == Kind::Stale);
    CHECK(stale.snapshot.result.value == "stale");
    CHECK(stale.snapshot.fresh_remaining_ms == 0);
    CHECK(stale.snapshot.servable_remaining_ms > 0);
    REQUIRE(stale.load);
    service.invalidate_key(owner, "post");
    CHECK(service.publish(owner, stale.load->ticket, "derived", options()) == Publication::Invalidated);
}

TEST_CASE("service restart and concurrent shutdown retire admission and tickets", "[service][lifecycle]") {
    ManualClock clock;
    CacheService service(config(), {}, clock.reader());
    const auto grant = service.begin_load(owner, "post");
    const auto id = service.instance_id();
    auto closing = std::async(std::launch::async, [&] { service.shutdown(); });
    service.shutdown();
    REQUIRE(ready(closing));
    closing.get();
    CHECK_FALSE(service.stats().ready);
    CHECK(service.stats().ticket_records == 0);
    CHECK(service.stats().ticket_key_bytes == 0);
    expect_error(ErrorCode::Unavailable, [&] { service.begin_load(owner, "post"); });
    expect_error(ErrorCode::Unavailable, [&] { service.lookup(owner, "post"); });
    expect_error(ErrorCode::Unavailable, [&] { service.publish(owner, grant.ticket, "v", options()); });
    expect_error(ErrorCode::Unavailable, [&] { service.invalidate_key(owner, "post"); });
    expect_error(ErrorCode::Unavailable, [&] { service.invalidate_tag(owner, "post"); });
    CacheService replacement(config(), {}, clock.reader());
    CHECK(replacement.instance_id() != id);
    CHECK(replacement.publish(owner, grant.ticket, "old", options()) == Publication::UnknownTicket);
    CHECK(replacement.lookup(owner, "post").snapshot.result.kind == Kind::Miss);
}
