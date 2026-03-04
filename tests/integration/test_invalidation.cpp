#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include "dbwaller/adapters/request_context.hpp"
#include "dbwaller/core/gateway.hpp"
#include "dbwaller/core/sharded_engine.hpp"
#include "dbwaller/policy/presets.hpp"
#include "support/fake_adapter.hpp"

int main() {
    using namespace dbwaller::core;
    using namespace dbwaller::policy;

    // 1. Setup Engine
    ShardedEngine::Config cfg;
    cfg.num_shards = 4;
    cfg.max_bytes_total = 10 * 1024 * 1024;
    ShardedEngine engine(cfg);
    Gateway waller(engine);
    auto adapter = std::make_shared<FakeAdapter>();

    // 2. Setup Policy
    PolicyRuleSet rules;
    Rule postRule;
    postRule.scope = CacheScope::Public;
    postRule.ttl_ms = 3600 * 1000; 
    rules.set_rule("post", postRule);

    Rule vipRule;
    vipRule.scope = CacheScope::Public;
    vipRule.ttl_ms = 3600 * 1000; 
    vipRule.extra_tags = {"group:vip_users"}; 
    rules.set_rule("vip", vipRule);

    dbwaller::adapters::RequestContext ctx;

    std::cout << "--- TEST: DIRECT INVALIDATION ---\n";

    // ---------------------------------------------------------
    // SCENARIO A: Single Item Invalidation via Logical Tag
    // ---------------------------------------------------------
    adapter->set_value("post:1", "Content_V1");
    auto res1 = waller.get_or_fetch_object_ruled(rules, "post", "1", "get", ctx, adapter);
    std::cout << "1. First Fetch (Miss)  => " << (res1 ? *res1 : "null") << "\n";

    // Update Backend
    adapter->set_value("post:1", "Content_V2");

    // Verify Cache Hit (Stale V1)
    auto res2 = waller.get_or_fetch_object_ruled(rules, "post", "1", "get", ctx, adapter);
    std::cout << "2. Second Fetch (Hit)  => " << (res2 ? *res2 : "null") << " (Should be V1)\n";

    // THE FIX: Use invalidate_tag instead of invalidate
    // The policy ensures 'post:1' is attached as a tag to the cache entry.
    std::cout << "   [!] Invalidating tag 'post:1'...\n";
    engine.invalidate_tag("post:1"); 

    // Verify Cache Miss (New V2)
    auto res3 = waller.get_or_fetch_object_ruled(rules, "post", "1", "get", ctx, adapter);
    std::cout << "3. Third Fetch (Miss)  => " << (res3 ? *res3 : "null") << " (Should be V2)\n";

    if (res3 && *res3 == "Content_V2") {
        std::cout << "[PASS] Direct invalidation successful.\n";
    } else {
        std::cout << "[FAIL] Still got old data or null.\n";
        return 1;
    }

    // ---------------------------------------------------------
    // SCENARIO B: Group Invalidation via Extra Tag
    // ---------------------------------------------------------
    std::cout << "\n--- TEST: GROUP INVALIDATION ---\n";
    adapter->set_value("vip:alpha", "VIP_Alpha_V1");
    adapter->set_value("vip:beta",  "VIP_Beta_V1");

    waller.get_or_fetch_object_ruled(rules, "vip", "alpha", "get", ctx, adapter);
    waller.get_or_fetch_object_ruled(rules, "vip", "beta",  "get", ctx, adapter);

    adapter->set_value("vip:alpha", "VIP_Alpha_V2");
    adapter->set_value("vip:beta",  "VIP_Beta_V2");

    // THE FIX: Use invalidate_tag
    std::cout << "   [!] Invalidating group tag 'group:vip_users'...\n";
    engine.invalidate_tag("group:vip_users");

    auto vipA = waller.get_or_fetch_object_ruled(rules, "vip", "alpha", "get", ctx, adapter);
    auto vipB = waller.get_or_fetch_object_ruled(rules, "vip", "beta",  "get", ctx, adapter);

    std::cout << "Fetch alpha => " << (vipA ? *vipA : "null") << "\n";
    std::cout << "Fetch beta  => " << (vipB ? *vipB : "null") << "\n";

    if (vipA && *vipA == "VIP_Alpha_V2" && vipB && *vipB == "VIP_Beta_V2") {
        std::cout << "[PASS] Group invalidation successful.\n";
    } else {
        std::cout << "[FAIL] Group invalidation failed.\n";
        return 1;
    }

    return 0;
}
