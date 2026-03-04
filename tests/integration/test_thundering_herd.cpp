#include <iostream>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cassert>

#include "dbwaller/adapters/adapter.hpp" // Base class
#include "dbwaller/core/gateway.hpp"
#include "dbwaller/core/sharded_engine.hpp"
#include "dbwaller/policy/presets.hpp"

// 1. Define a "Slow" Adapter to force overlap
class SlowAdapter : public dbwaller::adapters::Adapter {
public:
    std::atomic<uint64_t> fetch_count_{0};

    std::optional<dbwaller::adapters::FetchResult> fetch_one(
        const std::string& key,
        const dbwaller::adapters::RequestContext&
    ) override {
        // Increment counter
        fetch_count_.fetch_add(1);
        
        std::cout << "   [SlowAdapter] Starting heavy fetch for " << key << "...\n";
        
        // ARTIFICIAL DELAY: 500ms
        // This ensures all other threads arrive while this is still running.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        std::cout << "   [SlowAdapter] Fetch done.\n";

        dbwaller::adapters::FetchResult r;
        r.value = "VIRAL_CONTENT_PAYLOAD";
        r.ttl_ms = 5000;
        return r;
    }
};

int main() {
    using namespace dbwaller::core;
    using namespace dbwaller::policy;

    // 2. Setup Engine
    ShardedEngine::Config cfg;
    cfg.num_shards = 4;
    cfg.compute_threads = 4; // Background workers
    ShardedEngine engine(cfg);
    Gateway waller(engine);
    auto adapter = std::make_shared<SlowAdapter>();

    // 3. Setup Policy
    PolicyRuleSet rules;
    Rule defaultRule;
    defaultRule.scope = CacheScope::Public;
    defaultRule.ttl_ms = 5000;
    rules.set_default(defaultRule);

    dbwaller::adapters::RequestContext ctx;

    // 4. THE THUNDERING HERD
    int num_threads = 50;
    std::vector<std::thread> threads;
    std::atomic<bool> start_flag{false};
    std::atomic<int> success_count{0};

    std::cout << "--- TEST: THUNDERING HERD (50 concurrent requests) ---\n";
    std::cout << "Spawning " << num_threads << " threads to hit key 'viral:post'...\n";

    for(int i=0; i<num_threads; ++i) {
        threads.emplace_back([&, i]() {
            // A. Spin-wait until main signals start (synchronize launch)
            while(!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            // B. Request the SAME key
            auto res = waller.get_or_fetch_object_ruled(rules, "viral", "post", "get", ctx, adapter);
            
            // C. Verify result
            if(res && *res == "VIRAL_CONTENT_PAYLOAD") {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // 5. Unleash the herd
    std::cout << "GO!\n";
    start_flag.store(true, std::memory_order_release);

    // 6. Wait for all
    for(auto& t : threads) t.join();

    // 7. Analysis
    uint64_t actual_fetches = adapter->fetch_count_.load();
    auto stats = engine.stats_snapshot();

    std::cout << "\n--- RESULTS ---\n";
    std::cout << "Total Requests: " << num_threads << "\n";
    std::cout << "Successful Responses: " << success_count.load() << "\n";
    std::cout << "Adapter Fetches: " << actual_fetches << " (Target: 1)\n";
    std::cout << "Engine Stats: \n";
    std::cout << "  - Inflight Leaders (Fetchers): " << stats.inflight_leaders << "\n";
    std::cout << "  - Inflight Waits (Piggybackers): " << stats.inflight_waits << "\n";

    // 8. Verdict
    bool pass = true;
    if (actual_fetches != 1) {
        std::cout << "[FAIL] Adapter was hit " << actual_fetches << " times! (Expected 1)\n";
        pass = false;
    } else {
        std::cout << "[PASS] Perfect Coalescing. Only 1 fetch hit the DB.\n";
    }

    if (success_count.load() != num_threads) {
        std::cout << "[FAIL] Some threads got errors/null.\n";
        pass = false;
    }

    return pass ? 0 : 1;
}
