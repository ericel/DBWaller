#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "dbwaller/adapters/request_context.hpp"
#include "dbwaller/core/gateway.hpp"
#include "dbwaller/core/sharded_engine.hpp"
#include "dbwaller/policy/config_loader.hpp"
#include "dbwaller/policy/dump.hpp"
#include "dbwaller/policy/presets.hpp"

#include "support/fake_adapter.hpp"

int main() {
    using namespace dbwaller::core;

    // -----------------------------
    // Engine configuration
    // -----------------------------
    ShardedEngine::Config cfg;
    cfg.num_shards = 16;
    cfg.max_bytes_total = 64ull * 1024ull * 1024ull; // 64MB
    cfg.sweep_interval_ms = 200;

    // Miss-path governance
    cfg.enable_compute_pool = true;
    cfg.compute_threads = 4;
    cfg.compute_max_queue = 32;
    cfg.compute_timeout_ms = 1500;
    cfg.backpressure = ShardedEngine::BackpressureMode::RunInline;

    ShardedEngine engine(cfg);
    Gateway waller(engine);

    // -----------------------------
    // Fake authoritative adapter
    // -----------------------------
    auto adapter = std::make_shared<FakeAdapter>();
    adapter->set_value("post:1", "post_bytes_1");
    adapter->set_value("timeline:home", "timeline_v1_bytes");
    adapter->set_value("adminview:dashboard", "dashboard_sensitive_bytes");
    adapter->set_value("session:token", "session_secret");

    // -----------------------------
    // Request contexts
    // -----------------------------
    dbwaller::adapters::RequestContext ctxUser;
    ctxUser.tenant = "dev";
    ctxUser.viewer_id = "42";
    ctxUser.locale = "en";

    dbwaller::adapters::RequestContext ctxAdmin = ctxUser;

    // Claims fingerprints via helper API (normalized + hashed + truncated)
    dbwaller::adapters::set_claims_fingerprint(ctxUser, {"user"}, {"read"});
    dbwaller::adapters::set_claims_fingerprint(ctxAdmin, {"admin"}, {"read", "write"});

    // -----------------------------
    // Policy config: JSON (optional) + env overrides (env wins)
    // -----------------------------
    dbwaller::policy::NamespacePolicyConfig fallback;
    fallback.default_rule.scope = dbwaller::policy::CacheScope::PerViewer;
    fallback.default_rule.ttl_ms = 1500;
    fallback.default_rule.min_ttl_ms = 50;
    fallback.default_rule.max_ttl_ms = 30'000;
    fallback.default_rule.vary_by_claims = false;
    fallback.default_rule.require_claims_fingerprint = false;

    auto pcfg = dbwaller::policy::load_policy_from_env_or_fallback(std::move(fallback));

    dbwaller::policy::dump_config(std::cout, pcfg);
    dbwaller::policy::dump_effective_rules(std::cout, pcfg);
    std::cout << "----\n";

    auto rules = dbwaller::policy::build_ruleset_from_config(pcfg);

    // -----------------------------
    // Normal calls
    // -----------------------------
    auto pUser = waller.get_or_fetch_object_ruled(rules, "post", "1", "get", ctxUser, adapter);
    std::cout << "user post:1 => " << (pUser ? *pUser : "<null>") << "\n";

    auto pAdmin = waller.get_or_fetch_object_ruled(rules, "post", "1", "get", ctxAdmin, adapter);
    std::cout << "admin post:1 => " << (pAdmin ? *pAdmin : "<null>") << "\n";

    auto tUser = waller.get_or_fetch_object_ruled(rules, "timeline", "home", "get", ctxUser, adapter);
    std::cout << "user timeline:home => " << (tUser ? *tUser : "<null>") << "\n";

    auto dUser = waller.get_or_fetch_object_ruled(rules, "adminview", "dashboard", "get", ctxUser, adapter);
    std::cout << "user adminview:dashboard => " << (dUser ? *dUser : "<null>") << "\n";

    auto dAdmin = waller.get_or_fetch_object_ruled(rules, "adminview", "dashboard", "get", ctxAdmin, adapter);
    std::cout << "admin adminview:dashboard => " << (dAdmin ? *dAdmin : "<null>") << "\n";

    auto sUser = waller.get_or_fetch_object_ruled(rules, "session", "token", "get", ctxUser, adapter);
    std::cout << "user session:token => " << (sUser ? *sUser : "<null>") << "\n";

    // -----------------------------
    // SWR demo (timeline)
    // -----------------------------
    std::cout << "---- SWR demo (timeline) ----\n";

    // Fetch #1: should be fresh v1 (already cached above, but we show flow anyway)
    auto t1 = waller.get_or_fetch_object_ruled(rules, "timeline", "home", "get", ctxUser, adapter);
    std::cout << "timeline fetch #1 (expect v1 fresh) => " << (t1 ? *t1 : "<null>") << "\n";

    // Wait long enough for TTL(200ms) to expire, but still within SWR window
    std::this_thread::sleep_for(std::chrono::milliseconds(260));

    // Update authoritative value to v2 BEFORE triggering stale fetch
    adapter->set_value("timeline:home", "timeline_v2_bytes");

    // Fetch #2: should serve stale v1, and schedule refresh (background)
    auto t2 = waller.get_or_fetch_object_ruled(rules, "timeline", "home", "get", ctxUser, adapter);
    std::cout << "timeline fetch #2 (expect v1 stale served; refresh scheduled) => " << (t2 ? *t2 : "<null>") << "\n";

    // Give refresh time to run and publish v2
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    // Fetch #3: should now be fresh v2 (if refresh completed)
    auto t3 = waller.get_or_fetch_object_ruled(rules, "timeline", "home", "get", ctxUser, adapter);
    std::cout << "timeline fetch #3 (expect v2 fresh after refresh) => " << (t3 ? *t3 : "<null>") << "\n";

    // -----------------------------
    // Stats
    // -----------------------------
    auto st = engine.stats_snapshot();
    std::cout << "stats: hits=" << st.hits
              << " stale_hits=" << st.stale_hits
              << " misses=" << st.misses
              << " puts=" << st.puts
              << " evictions=" << st.evictions
              << " expirations=" << st.expirations
              << " inflight_leaders=" << st.inflight_leaders
              << " inflight_waits=" << st.inflight_waits
              << " compute_queue_rejects=" << st.compute_queue_rejects
              << " compute_timeouts=" << st.compute_timeouts
              << "\n";

    return 0;
}
