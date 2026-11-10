#include <catch2/catch_test_macros.hpp>

#include "dbwaller/core/gateway.hpp"
#include "../concurrency/test_support.hpp"

#include <memory>

using namespace lifecycle_test;
using namespace dbwaller;

namespace {
struct Observed {
    std::string key;
    adapters::RequestContext context;
};

class RecordingAdapter final : public adapters::Adapter {
public:
    std::promise<Observed>& observed;
    explicit RecordingAdapter(std::promise<Observed>& output) : observed(output) {}
    std::optional<adapters::FetchResult> fetch_one(const std::string& key, const adapters::RequestContext& ctx) override {
        observed.set_value({key, ctx});
        return adapters::FetchResult{"backend value", 60'000, {}};
    }
};

// Keep the request frame separate so ASan can detect a callback referencing it
// after it returns. No engine instrumentation or private test hooks are needed.
[[gnu::noinline]] std::string timed_out_request(core::ShardedEngine& engine,
    std::promise<Observed>& observed, std::weak_ptr<RecordingAdapter>& weak) {
    core::Gateway gateway(engine);
    policy::PolicyRuleSet rules;
    policy::Rule rule;
    rule.ttl_ms = 30'000;
    rule.extra_tags = {"policy-tag"};
    rules.set_default(rule);
    std::string ns(256, 'n'), id(256, 'i'), op = "fetch";
    adapters::RequestContext ctx{"tenant", "viewer", "locale", std::string(128, 'c')};
    const auto key = rules.decide(ns, id, op, ctx).cache_key;
    std::vector<std::string> tags{"caller-tag"};
    auto adapter = std::make_shared<RecordingAdapter>(observed);
    weak = adapter;
    core::OpMeta meta;
    CHECK_FALSE(gateway.get_or_fetch_object_ruled(rules, ns, id, op, ctx, adapter, &meta, tags).has_value());
    CHECK(meta.result == core::OpResult::Miss);
    ns.assign("changed"); id.assign("changed"); ctx.tenant.assign("changed"); tags.clear();
    return key;
}
} // namespace

TEST_CASE("gateway miss owns request data and adapter after waiter timeout", "[concurrency][gateway][lifetime]") {
    Gate gate;
    std::promise<Observed> observed;
    auto report = observed.get_future();
    std::weak_ptr<RecordingAdapter> weak;
    core::ShardedEngine::Config cfg;
    cfg.compute_threads = 1;
    cfg.compute_max_queue = 2;
    cfg.compute_timeout_ms = 1;
    cfg.sweep_interval_ms = 10;
    core::ShardedEngine engine(cfg);
    ReleaseOnExit release{gate.release};
    engine.trigger_refresh("occupied", [&] { gate.block(); return std::pair{std::string("busy"), core::PutOptions{}}; });
    REQUIRE(gate.entered.wait());
    const auto key = timed_out_request(engine, observed, weak);
    CHECK(engine.stats_snapshot().compute_timeouts == 1);
    CHECK_FALSE(weak.expired());
    gate.release.set();
    REQUIRE(ready(report));
    const auto captured = report.get();
    CHECK(captured.key == std::string(256, 'n') + ":" + std::string(256, 'i'));
    CHECK(captured.context.tenant == "tenant");
    CHECK(captured.context.viewer_id == "viewer");
    CHECK(captured.context.locale == "locale");
    CHECK(captured.context.claims_fingerprint == std::string(128, 'c'));
    engine.shutdown();
    CHECK(engine.get(key) == "backend value");
    engine.invalidate_tag("caller-tag");
    CHECK_FALSE(engine.get(key).has_value());
    CHECK(weak.expired());
}
