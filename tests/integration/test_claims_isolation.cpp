#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <cassert>

// Adjust these includes to match your file structure
#include "dbwaller/adapters/request_context.hpp"
#include "dbwaller/core/gateway.hpp"
#include "dbwaller/core/sharded_engine.hpp"
#include "dbwaller/policy/presets.hpp" // or wherever your PolicyRuleSet is defined
#include "support/fake_adapter.hpp"

// 

int main() {
    using namespace dbwaller::core;
    using namespace dbwaller::policy;

    // ---------------------------------------------------------
    // 1. Setup Engine & Adapter
    // ---------------------------------------------------------
    ShardedEngine::Config cfg;
    cfg.num_shards = 4;
    cfg.max_bytes_total = 10 * 1024 * 1024; // 10MB
    ShardedEngine engine(cfg);
    Gateway waller(engine);
    auto adapter = std::make_shared<FakeAdapter>();

    // ---------------------------------------------------------
    // 2. Setup Policy (Directly using your PolicyRuleSet class)
    // ---------------------------------------------------------
    PolicyRuleSet rules;

    // A. Default Rule: Public, short TTL
    Rule defaultRule;
    defaultRule.scope = CacheScope::Public;
    defaultRule.ttl_ms = 5000;
    rules.set_default(defaultRule);

    // B. AdminView Rule: The core of the test
    // We want to prove that even though this is cached (PerViewer),
    // distinct claims create distinct cache keys.
    Rule adminRule;
    adminRule.scope = CacheScope::PerViewer; 
    adminRule.ttl_ms = 60000;        // Long TTL so we hit it if isolation fails
    adminRule.vary_by_claims = true; // <--- CRITICAL FLAG
    adminRule.require_claims_fingerprint = true;
    
    rules.set_rule("adminview", adminRule);

    std::cout << "--- TEST SETUP ---\n";
    std::cout << "Rule for [adminview]: vary_by_claims=true\n\n";

    // ---------------------------------------------------------
    // 3. Setup Contexts (Attacker vs Victim)
    // ---------------------------------------------------------
    
    // ADMIN Context
    dbwaller::adapters::RequestContext ctxAdmin;
    ctxAdmin.viewer_id = "admin_bob";
    // Helper to generate hash (assuming you have this helper from the demo)
    dbwaller::adapters::set_claims_fingerprint(ctxAdmin, {"admin", "finance"}, {"read", "write"});

    // USER Context
    dbwaller::adapters::RequestContext ctxUser;
    ctxUser.viewer_id = "user_alice";
    // Different claims = Different hash
    dbwaller::adapters::set_claims_fingerprint(ctxUser, {"user"}, {"read"});

    // ---------------------------------------------------------
    // STEP 1: Admin hydrates the cache
    // ---------------------------------------------------------
    std::cout << "--- STEP 1: Admin Access ---\n";
    adapter->set_value("adminview:dashboard", "SECRET_ADMIN_DATA");
    
    // Note: ensure Gateway::get_or_fetch_object_ruled accepts your PolicyRuleSet type
    auto resAdmin = waller.get_or_fetch_object_ruled(rules, "adminview", "dashboard", "get", ctxAdmin, adapter);
    std::cout << "Admin got: " << (resAdmin ? *resAdmin : "null") << "\n";
    
    if (!resAdmin || *resAdmin != "SECRET_ADMIN_DATA") {
        std::cerr << "CRITICAL: Admin failed to fetch own data.\n";
        return 1;
    }

    // ---------------------------------------------------------
    // STEP 2: Change the Backend (The Poison Pill)
    // ---------------------------------------------------------
    // We change the source of truth.
    // If the User hits the Admin's cache entry, they will see the OLD secret data.
    // If the User creates a NEW cache key (proper isolation), they will miss and see this NEW data.
    adapter->set_value("adminview:dashboard", "SAFE_USER_DATA");

    // ---------------------------------------------------------
    // STEP 3: User Access (The Attack)
    // ---------------------------------------------------------
    std::cout << "\n--- STEP 3: User Access (Expect Miss -> New Data) ---\n";
    auto resUser = waller.get_or_fetch_object_ruled(rules, "adminview", "dashboard", "get", ctxUser, adapter);
    std::cout << "User got:  " << (resUser ? *resUser : "null") << "\n";

    // ---------------------------------------------------------
    // VERIFICATION
    // ---------------------------------------------------------
    std::cout << "\n--- RESULTS ---\n";
    bool success = true;

    if (!resUser) {
        std::cout << "[FAIL] User got null result.\n";
        success = false;
    } 
    else if (*resUser == "SECRET_ADMIN_DATA") {
        std::cout << "[FAIL] SECURITY LEAK! User received cached admin data.\n";
        std::cout << "       Reason: 'vary_by_claims' did not alter the cache key.\n";
        success = false;
    } 
    else if (*resUser == "SAFE_USER_DATA") {
        std::cout << "[PASS] Isolation confirmed.\n";
        std::cout << "       User claims generated a unique cache key (Cache Miss).\n";
    } 
    else {
        std::cout << "[?] Unexpected data received.\n";
        success = false;
    }

    // Check stats to confirm the "Miss"
    auto st = engine.stats_snapshot();
    // We expect:
    // 1. Admin fetch (Miss -> Put)
    // 2. User fetch (Miss -> Put)
    // Total misses = 2. Hits = 0.
    if (st.misses == 2 && st.hits == 0) {
        std::cout << "[PASS] Stats confirm clean separation (2 misses).\n";
    } else {
        std::cout << "[WARN] Stats look suspicious (expected 2 misses, 0 hits). Got hits=" << st.hits << "\n";
    }

    return success ? 0 : 1;
}
