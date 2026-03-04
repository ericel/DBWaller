#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dbwaller/adapters/request_context.hpp"
#include "dbwaller/core/gateway.hpp"
#include "dbwaller/core/sharded_engine.hpp"
#include "dbwaller/policy/rules.hpp"

#include "support/fake_adapter.hpp"

using namespace std::chrono;

static void sleep_ms(uint64_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#define REQUIRE(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] " << msg << "\n"; \
            return 1; \
        } \
    } while (0)

// A tiny barrier so all threads start together.
class StartGate {
public:
    explicit StartGate(int n) : needed_(n) {}

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lk(mu_);
        arrived_++;
        if (arrived_ >= needed_) {
            open_ = true;
            cv_.notify_all();
            return;
        }
        cv_.wait(lk, [&] { return open_; });
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    int needed_ = 0;
    int arrived_ = 0;
    bool open_ = false;
};

static std::optional<std::string> get_until(
    dbwaller::core::Gateway& waller,
    const dbwaller::policy::PolicyRuleSet& rules,
    std::string_view ns,
    std::string_view id,
    std::string_view op,
    const dbwaller::adapters::RequestContext& ctx,
    std::shared_ptr<dbwaller::adapters::Adapter> adapter,
    std::string_view expected,
    milliseconds timeout
) {
    const auto deadline = steady_clock::now() + timeout;
    while (steady_clock::now() < deadline) {
        auto v = waller.get_or_fetch_object_ruled(rules, ns, id, op, ctx, adapter);
        if (v && *v == expected) return v;
        sleep_ms(25);
    }
    return std::nullopt;
}

int main() {
    using namespace dbwaller::core;
    using namespace dbwaller::policy;

    // -----------------------------
    // Engine
    // -----------------------------
    ShardedEngine::Config cfg;
    cfg.num_shards = 4;
    cfg.enable_compute_pool = true;
    cfg.compute_threads = 2;
    cfg.compute_max_queue = 64;
    cfg.compute_timeout_ms = 1500;
    cfg.backpressure = ShardedEngine::BackpressureMode::RunInline;
    cfg.sweep_interval_ms = 50;

    ShardedEngine engine(cfg);
    Gateway waller(engine);

    // -----------------------------
    // Policy
    // -----------------------------
    PolicyRuleSet rules;
    Rule r;
    r.scope = CacheScope::Public;
    r.ttl_ms = 100;
    r.swr_ms = 5000;
    r.stale_ttl_ms = 3000;
    r.serve_stale_on_error = true;
    rules.set_default(r);

    // Context
    dbwaller::adapters::RequestContext ctx;
    ctx.tenant = "bench";
    ctx.viewer_id = "anon";
    ctx.locale = "en";
    dbwaller::adapters::set_claims_fingerprint(ctx, {"user"}, {"read"});

    // Adapter
    auto adapter = std::make_shared<FakeAdapter>();
    adapter->verbose = false;
    adapter->set_value("data:1", "STALE_V1");

    // -----------------------------
    // TEST 1: Refresh coalescing
    // -----------------------------
    std::cout << "--- TEST 1: REFRESH COALESCING ---\n";

    // Prime cache via Gateway (1 authoritative fetch)
    {
        adapter->reset_fetch_count();
        auto v = waller.get_or_fetch_object_ruled(rules, "data", "1", "get", ctx, adapter);
        REQUIRE(v && *v == "STALE_V1", "prime should return STALE_V1");
        REQUIRE(adapter->fetch_count() == 1, "prime should do exactly 1 authoritative fetch");
    }

    // Force stale
    sleep_ms(150);

    // Keep refresh inflight long enough for the whole herd to overlap on it.
    // Otherwise, a fast refresh can complete, clear the inflight guard, and
    // let a delayed caller schedule a second refresh for the same stale window.
    adapter->fetch_delay_ms.store(250, std::memory_order_relaxed);

    // Count only refreshes during herd (stale path should not call adapter directly)
    adapter->reset_fetch_count();

    const int N = 20;
    StartGate gate(N);
    std::atomic<int> stale_served{0};

    std::vector<std::thread> threads;
    threads.reserve(N);

    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&] {
            gate.arrive_and_wait();
            auto res = waller.get_or_fetch_object_ruled(rules, "data", "1", "get", ctx, adapter);
            if (res && *res == "STALE_V1") stale_served.fetch_add(1);
        });
    }

    for (auto& t : threads) t.join();

    // Give refresh time to complete
    sleep_ms(300);

    std::cout << "Stale served: " << stale_served.load() << "/" << N << "\n";
    std::cout << "Adapter fetches during herd: " << adapter->fetch_count() << " (Expected 1)\n";

    REQUIRE(stale_served.load() == N, "all callers should receive stale immediately during SWR");
    REQUIRE(adapter->fetch_count() == 1, "refresh coalescing failed; expected exactly 1 refresh");
    adapter->fetch_delay_ms.store(0, std::memory_order_relaxed);

    std::cout << "[PASS] Refresh coalescing confirmed.\n\n";

    // -----------------------------
    // TEST 2: Serve stale on error + eventual update
    // -----------------------------
    std::cout << "--- TEST 2: SERVE STALE ON ERROR + EVENTUAL UPDATE ---\n";

    // Fresh key for this test (avoid cross-test timing surprises)
    adapter->set_value("data:2", "V1");

    // Prime V1
    {
        adapter->reset_fetch_count();
        auto v = waller.get_or_fetch_object_ruled(rules, "data", "2", "get", ctx, adapter);
        REQUIRE(v && *v == "V1", "prime data:2 should return V1");
        REQUIRE(adapter->fetch_count() == 1, "prime should do 1 fetch");
    }

    // Make stale
    sleep_ms(150);

    // Force the *next refresh* to fail once
    adapter->fail_next_for("data:2");

    // First stale request: should serve V1 (stale) even though refresh fails
    auto s = waller.get_or_fetch_object_ruled(rules, "data", "2", "get", ctx, adapter);
    REQUIRE(s && *s == "V1", "should serve stale V1 even when refresh fails");

    // Now backend recovers and has V2
    adapter->set_value("data:2", "V2");

    // Eventually we should observe V2
    auto v2 = get_until(waller, rules, "data", "2", "get", ctx, adapter, "V2", milliseconds(1500));
    REQUIRE(v2 && *v2 == "V2", "expected eventual V2 after refresh succeeds");

    std::cout << "[PASS] Serve stale on error + eventual refresh update confirmed.\n\n";

    // Stats snapshot
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
