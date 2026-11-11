#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "dbwaller/concurrency/thread_pool.hpp"
#include "dbwaller/observability/observer.hpp"

namespace dbwaller::core {

enum class PublishStatus { Stored, Invalidated };

// Copyable, opaque capability for one key and one engine invalidation generation.
// Capture before fetching; it does not keep the engine itself alive.
class LoadTicket {
public:
    LoadTicket(const LoadTicket&) = default;
    LoadTicket(LoadTicket&&) noexcept = default;
    LoadTicket& operator=(const LoadTicket&) = default;
    LoadTicket& operator=(LoadTicket&&) noexcept = default;

private:
    friend class ShardedEngine;
    struct Generation {};
    LoadTicket(std::string key, std::shared_ptr<const Generation> generation)
        : key_(std::move(key)), generation_(std::move(generation)) {}
    std::string key_;
    std::shared_ptr<const Generation> generation_;
};

class LoadInvalidatedError : public std::runtime_error {
public:
    LoadInvalidatedError() : std::runtime_error("cache load invalidated") {}
};

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

        // Zero preserves the legacy unlimited entry count. A finite limit is
        // apportioned across shards (unused capacity is not borrowed).
        size_t max_entries_total = 0;
    };

    enum class GetKind { Miss, Fresh, Stale };

    struct GetResult {
        GetKind kind = GetKind::Miss;
        std::optional<std::string> value;
    };

    struct Snapshot {
        GetResult result;
        // Relative hints at the time of the read, not a freshness guarantee.
        uint64_t fresh_remaining_ms = 0;
        uint64_t servable_remaining_ms = 0;
    };

    explicit ShardedEngine(Config cfg);
    ~ShardedEngine();

    static uint64_t now_ms();

    // Fresh-only get (legacy)
    std::optional<std::string> get(const std::string& key);

    // SWR-aware get
    GetResult get_swr(const std::string& key);

    Snapshot get_snapshot(const std::string& key);
    // Linearizes the snapshot and ticket against publication and invalidation.
    // A ticket acquired separately AFTER a read cannot protect that earlier read.
    std::pair<Snapshot, LoadTicket> get_snapshot_and_begin_load(const std::string& key);

    // Unconditional deliberate write; does not fence pending loads. For fetched
    // values use begin_load before fetching and put_if_current after fetching.
    void put(const std::string& key, std::string encrypted_value, const PutOptions& opt);

    LoadTicket begin_load(const std::string& key);
    PublishStatus put_if_current(const LoadTicket& ticket, std::string encrypted_value,
                                 const PutOptions& opt);

    // Every invalidation fences all earlier loads, even if nothing is cached.
    void invalidate_key(const std::string& key);
    void invalidate_tag(const std::string& tag);

    // Miss path: single-flight per generation + governance. Throws
    // LoadInvalidatedError if invalidation supersedes the shared computation.
    std::string get_or_compute(
        const std::string& key,
        const std::function<std::pair<std::string, PutOptions>()>& compute_fn
    );

    // Stale path: schedule refresh. RunInline/pool-disabled execution may block.
    // If already inflight in the current generation or shutdown has started,
    // do nothing. Invalidated background results are discarded.
    void trigger_refresh(
        const std::string& key,
        std::function<std::pair<std::string, PutOptions>()> compute_fn
    );

    EngineStats stats_snapshot() const;

    dbwaller::observability::Observer observer() const {
        return dbwaller::observability::Observer(this);
    }


    // Close compute admission and finish accepted work, including inline work.
    // Concurrent/repeated calls wait for completion. Callbacks must return;
    // shutdown from this engine's own compute callback is unsupported.
    // Stop external callers before destroying the engine. Cache reads remain
    // available after explicit shutdown while the engine is still alive.
    void shutdown();


private:
    // Counts API calls that can run inline or wait on a shared computation.
    // Queued callbacks that outlive a caller are covered by the pool's drain.
    class ComputeCall {
    public:
        explicit ComputeCall(ShardedEngine& engine);
        ~ComputeCall();
        ComputeCall(const ComputeCall&) = delete;
        ComputeCall& operator=(const ComputeCall&) = delete;
        explicit operator bool() const { return admitted_; }
    private:
        ShardedEngine& engine_;
        bool admitted_ = false;
    };

    struct EntryVersion {};

    struct CacheEntry {
        std::shared_ptr<const EntryVersion> version = std::make_shared<const EntryVersion>();
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
        explicit Inflight(LoadTicket load) : ticket(std::move(load)) {}
        const LoadTicket ticket;
        std::shared_ptr<const EntryVersion> refresh_entry;
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        bool ok = false;
        bool invalidated = false;
        std::string value;
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

    // Caller holds publication_mu_; acquires only the target shard's lock.
    void put_locked(const std::string& key, std::string encrypted_value, const PutOptions& opt);

    void remove_entry_locked(Shard& s, const std::string& key);
    void expire_and_evict_locked(Shard& s, uint64_t shard_budget_bytes);
    size_t shard_entry_budget(const Shard& s) const;
    void sweeper_loop(std::stop_token st);

    // Worker helpers
    void run_compute_and_publish(
        size_t shard_idx,
        std::string key,
        std::shared_ptr<Inflight> inflight,
        std::function<std::pair<std::string, PutOptions>()> compute_fn,
        bool is_refresh
    );

    void mark_refresh_failure(size_t shard_idx, const std::string& key,
                              const std::shared_ptr<Inflight>& inflight);

    void finish_compute(size_t shard_idx, const std::string& key,
                        const std::shared_ptr<Inflight>& inflight,
                        std::string value, bool ok);

    friend class dbwaller::observability::Observer;

private:
    Config cfg_;
    std::vector<Shard> shards_;

    // Lock order: publication -> shard/inflight map -> individual flight.
    // Never held across callbacks, scheduling, or waits. Shared generation
    // identities cannot wrap or be reused while an old ticket still exists.
    std::mutex publication_mu_;
    std::shared_ptr<const LoadTicket::Generation> generation_ =
        std::make_shared<const LoadTicket::Generation>();

    std::mutex shutdown_mu_;
    std::mutex lifecycle_mu_;
    std::condition_variable lifecycle_cv_;
    bool closing_ = false;
    size_t active_calls_ = 0;
    std::mutex sweeper_mu_;
    std::condition_variable_any sweeper_cv_;

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
