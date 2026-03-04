#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dbwaller/concurrency/thread_pool.hpp"
#include "dbwaller/observability/observer.hpp"

namespace dbwaller::core {

struct PutOptions {
    uint64_t ttl_ms = 1000;

    // Stale-while-revalidate window:
    // after ttl_ms expires, we can serve stale for swr_ms while scheduling refresh
    uint64_t swr_ms = 0;

    // Additional stale window *only if refresh fails* (requires serve_stale_on_error=true)
    uint64_t stale_ttl_ms = 0;

    bool serve_stale_on_error = false;

    std::vector<std::string> tags;
};

struct EngineStats {
    uint64_t hits = 0;
    uint64_t stale_hits = 0;
    uint64_t misses = 0;
    uint64_t puts = 0;
    uint64_t evictions = 0;
    uint64_t expirations = 0;

    uint64_t inflight_leaders = 0;
    uint64_t inflight_waits = 0;

    uint64_t compute_queue_rejects = 0;
    uint64_t compute_timeouts = 0;

    uint64_t refresh_scheduled = 0;
    uint64_t refresh_skipped_inflight = 0;
    uint64_t refresh_failures = 0;
};

class ShardedEngine {
public:
    enum class BackpressureMode { RunInline, FailFast };

    struct Config {
        size_t num_shards = 16;
        uint64_t max_bytes_total = 64ull * 1024ull * 1024ull;
        uint64_t sweep_interval_ms = 250;

        // Miss-path governance
        bool enable_compute_pool = true;
        size_t compute_threads = 4;
        size_t compute_max_queue = 32;
        uint64_t compute_timeout_ms = 1500;
        BackpressureMode backpressure = BackpressureMode::RunInline;
    };

    enum class GetKind { Miss, Fresh, Stale };

    struct GetResult {
        GetKind kind = GetKind::Miss;
        std::optional<std::string> value;
    };

    explicit ShardedEngine(Config cfg);
    ~ShardedEngine();

    static uint64_t now_ms();

    // Fresh-only get (legacy)
    std::optional<std::string> get(const std::string& key);

    // SWR-aware get
    GetResult get_swr(const std::string& key);

    // Put value with TTL/SWR metadata
    void put(const std::string& key, std::string encrypted_value, const PutOptions& opt);

    // Invalidation
    void invalidate_key(const std::string& key);
    void invalidate_tag(const std::string& tag);

    // Miss path: single-flight + governance
    std::string get_or_compute(
        const std::string& key,
        const std::function<std::pair<std::string, PutOptions>()>& compute_fn
    );

    // Stale path: schedule refresh (non-blocking). If already inflight, do nothing.
    void trigger_refresh(
        const std::string& key,
        std::function<std::pair<std::string, PutOptions>()> compute_fn
    );

    EngineStats stats_snapshot() const;

    dbwaller::observability::Observer observer() const {
        return dbwaller::observability::Observer(this);
    }


    // Graceful shutdown: stops background threads.
    // Call this BEFORE destroying any adapters this engine might be using.
    void shutdown();


private:
    struct CacheEntry {
        std::string encrypted_value;
        uint32_t size_bytes = 0;

        // Time windows:
        //   fresh: now < fresh_until_ms
        //   stale (SWR): fresh_until_ms <= now < swr_until_ms
        //   stale-if-error: swr_until_ms <= now < stale_until_ms AND last_refresh_ok == false AND serve_stale_on_error
        uint64_t fresh_until_ms = 0;
        uint64_t swr_until_ms = 0;
        uint64_t stale_until_ms = 0;

        bool serve_stale_on_error = false;

        // Refresh status for stale-if-error decisions
        bool refresh_attempted = false;
        bool last_refresh_ok = true;
        uint64_t last_refresh_ms = 0;

        std::vector<std::string> tags;
    };

    struct Inflight {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        bool ok = false;
        std::string value;
        std::string error;
    };

    struct Shard {
        //std::shared_mutex mu;
        mutable std::shared_mutex mu; // ✅ allow locking in const observer methods
        std::unordered_map<std::string, CacheEntry> entries;
        uint64_t bytes = 0;

        std::unordered_map<std::string, std::unordered_set<std::string>> tag_index;

        // single-flight map
        std::mutex inflight_mu;
        std::unordered_map<std::string, std::shared_ptr<Inflight>> inflight;
    };

    size_t shard_index(const std::string& key) const;

    void remove_entry_locked(Shard& s, const std::string& key);
    void expire_and_evict_locked(Shard& s, uint64_t shard_budget_bytes);
    void sweeper_loop(std::stop_token st);

    // Worker helpers
    void run_compute_and_publish(
        size_t shard_idx,
        std::string key,
        std::shared_ptr<Inflight> inflight,
        std::function<std::pair<std::string, PutOptions>()> compute_fn,
        bool is_refresh
    );

    void mark_refresh_failure(size_t shard_idx, const std::string& key);

    friend class dbwaller::observability::Observer;

private:
    Config cfg_;
    std::vector<Shard> shards_;

    std::unique_ptr<dbwaller::concurrency::ThreadPool> compute_pool_;
    std::jthread sweeper_;

    // Stats
    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> stale_hits_{0};
    std::atomic<uint64_t> misses_{0};
    std::atomic<uint64_t> puts_{0};
    std::atomic<uint64_t> evictions_{0};
    std::atomic<uint64_t> expirations_{0};

    std::atomic<uint64_t> inflight_leaders_{0};
    std::atomic<uint64_t> inflight_waits_{0};

    std::atomic<uint64_t> compute_queue_rejects_{0};
    std::atomic<uint64_t> compute_timeouts_{0};

    std::atomic<uint64_t> refresh_scheduled_{0};
    std::atomic<uint64_t> refresh_skipped_inflight_{0};
    std::atomic<uint64_t> refresh_failures_{0};
};

} // namespace dbwaller::core
