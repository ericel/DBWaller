#include "dbwaller/core/sharded_engine.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace dbwaller::core {

static inline uint64_t steady_now_ms() {
    using Clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()
    );
}

uint64_t ShardedEngine::now_ms() { return steady_now_ms(); }

size_t ShardedEngine::shard_index(const std::string& key) const {
    return std::hash<std::string>{}(key) % shards_.size();
}

ShardedEngine::ShardedEngine(Config cfg)
    : cfg_(cfg),
      shards_(cfg_.num_shards ? cfg_.num_shards : 1) {

    if (cfg_.num_shards == 0) cfg_.num_shards = 1;

    // Compute pool setup (miss-path governance)
    if (cfg_.enable_compute_pool) {
        size_t threads = cfg_.compute_threads;
        if (threads == 0) threads = std::max<size_t>(1, std::thread::hardware_concurrency());
        compute_pool_ = std::make_unique<dbwaller::concurrency::ThreadPool>(threads, cfg_.compute_max_queue);
    }

    // Sweeper thread
    sweeper_ = std::jthread([this](std::stop_token st) { sweeper_loop(st); });
}

// Update Destructor to be safe
ShardedEngine::~ShardedEngine() {
    shutdown(); 
}

// -------------------------
// Core cache operations
// -------------------------

std::optional<std::string> ShardedEngine::get(const std::string& key) {
    Shard& s = shards_[shard_index(key)];

    {
        std::shared_lock lk(s.mu);
        auto it = s.entries.find(key);
        if (it == s.entries.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        const uint64_t now = now_ms();
        if (it->second.fresh_until_ms > now) {
            hits_.fetch_add(1, std::memory_order_relaxed);
            return it->second.encrypted_value;
        }
    }

    // Lazy cleanup only if fully expired beyond stale windows
    {
        std::unique_lock lk(s.mu);
        auto it = s.entries.find(key);
        if (it != s.entries.end()) {
            const uint64_t now = now_ms();
            if (now >= it->second.stale_until_ms && it->second.stale_until_ms != 0) {
                remove_entry_locked(s, key);
                expirations_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    misses_.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
}

ShardedEngine::GetResult ShardedEngine::get_swr(const std::string& key) {
    Shard& s = shards_[shard_index(key)];
    const uint64_t now = now_ms();

    {
        std::shared_lock lk(s.mu);
        auto it = s.entries.find(key);
        if (it == s.entries.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return {GetKind::Miss, std::nullopt};
        }

        const CacheEntry& e = it->second;

        // Fresh
        if (now < e.fresh_until_ms) {
            hits_.fetch_add(1, std::memory_order_relaxed);
            return {GetKind::Fresh, e.encrypted_value};
        }

        // Stale (SWR window)
        if (e.swr_until_ms > 0 && now < e.swr_until_ms) {
            stale_hits_.fetch_add(1, std::memory_order_relaxed);
            return {GetKind::Stale, e.encrypted_value};
        }

        // Stale-if-error window (only if refresh attempted & failed)
        if (e.stale_until_ms > 0 && now < e.stale_until_ms &&
            e.serve_stale_on_error &&
            e.refresh_attempted &&
            !e.last_refresh_ok) {
            stale_hits_.fetch_add(1, std::memory_order_relaxed);
            return {GetKind::Stale, e.encrypted_value};
        }

        // Past all allowed windows -> treat as miss (cleanup below)
    }

    // Cleanup if we're truly beyond stale_until_ms (or if stale_until_ms==0, beyond fresh_until)
    {
        std::unique_lock lk(s.mu);
        auto it = s.entries.find(key);
        if (it != s.entries.end()) {
            const uint64_t n = now_ms();
            const uint64_t hard_expire = (it->second.stale_until_ms > 0) ? it->second.stale_until_ms : it->second.fresh_until_ms;
            if (hard_expire > 0 && n >= hard_expire) {
                remove_entry_locked(s, key);
                expirations_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    misses_.fetch_add(1, std::memory_order_relaxed);
    return {GetKind::Miss, std::nullopt};
}

void ShardedEngine::put(const std::string& key, std::string encrypted_value, const PutOptions& opt) {
    Shard& s = shards_[shard_index(key)];

    CacheEntry e;
    e.encrypted_value = std::move(encrypted_value);
    e.size_bytes = static_cast<uint32_t>(e.encrypted_value.size());

    const uint64_t now = now_ms();
    e.fresh_until_ms = now + opt.ttl_ms;

    // swr_until_ms includes the SWR window after TTL
    e.swr_until_ms = (opt.swr_ms > 0) ? (e.fresh_until_ms + opt.swr_ms) : 0;

    // stale_until_ms extends beyond swr_until only if stale_ttl_ms > 0
    if (opt.stale_ttl_ms > 0) {
        const uint64_t base = (e.swr_until_ms > 0) ? e.swr_until_ms : e.fresh_until_ms;
        e.stale_until_ms = base + opt.stale_ttl_ms;
    } else {
        // If no stale_ttl, hard expire is fresh_until (or swr_until if you want SWR to count as "still present")
        e.stale_until_ms = (e.swr_until_ms > 0) ? e.swr_until_ms : e.fresh_until_ms;
    }

    e.serve_stale_on_error = opt.serve_stale_on_error;
    e.tags = opt.tags;

    // Reset refresh status on successful write
    e.refresh_attempted = true;
    e.last_refresh_ok = true;
    e.last_refresh_ms = now;

    const uint64_t shard_budget = cfg_.max_bytes_total / shards_.size();

    std::unique_lock lk(s.mu);

    // Replace existing entry if any
    if (s.entries.find(key) != s.entries.end()) {
        remove_entry_locked(s, key);
    }

    s.bytes += e.size_bytes;
    s.entries.emplace(key, std::move(e));

    // Update tag index
    auto it = s.entries.find(key);
    for (const auto& tag : it->second.tags) {
        s.tag_index[tag].insert(key);
    }

    puts_.fetch_add(1, std::memory_order_relaxed);

    // Best-effort eviction
    if (s.bytes > shard_budget) {
        expire_and_evict_locked(s, shard_budget);
    }
}

void ShardedEngine::invalidate_key(const std::string& key) {
    Shard& s = shards_[shard_index(key)];
    std::unique_lock lk(s.mu);
    if (s.entries.find(key) != s.entries.end()) {
        remove_entry_locked(s, key);
        evictions_.fetch_add(1, std::memory_order_relaxed);
    }
}

void ShardedEngine::invalidate_tag(const std::string& tag) {
    for (auto& s : shards_) {
        std::unique_lock lk(s.mu);
        auto it = s.tag_index.find(tag);
        if (it == s.tag_index.end()) continue;

        std::vector<std::string> keys;
        keys.reserve(it->second.size());
        for (const auto& k : it->second) keys.push_back(k);

        for (const auto& k : keys) {
            if (s.entries.find(k) != s.entries.end()) {
                remove_entry_locked(s, k);
                evictions_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        s.tag_index.erase(tag);
    }
}

// -------------------------
// Refresh failure marking (keep stale entry!)
// -------------------------

void ShardedEngine::mark_refresh_failure(size_t shard_idx, const std::string& key) {
    Shard& s = shards_[shard_idx];
    std::unique_lock lk(s.mu);
    auto it = s.entries.find(key);
    if (it == s.entries.end()) return;

    CacheEntry& e = it->second;
    e.refresh_attempted = true;
    e.last_refresh_ok = false;
    e.last_refresh_ms = now_ms();

    refresh_failures_.fetch_add(1, std::memory_order_relaxed);
}

// -------------------------
// Miss-path governance + single-flight
// -------------------------

void ShardedEngine::run_compute_and_publish(
    size_t shard_idx,
    std::string key,
    std::shared_ptr<Inflight> inflight,
    std::function<std::pair<std::string, PutOptions>()> compute_fn,
    bool is_refresh
) {
    std::string value;
    PutOptions opt;

    try {
        auto out = compute_fn();
        value = std::move(out.first);
        opt = std::move(out.second);

        // Store in cache (thread-safe)
        put(key, value, opt);

        // Publish success
        {
            std::lock_guard<std::mutex> lk(inflight->mu);
            inflight->done = true;
            inflight->ok = true;
            inflight->value = value;
        }
        inflight->cv.notify_all();
    } catch (...) {
        // If this is a refresh: DO NOT delete cache entry. Mark refresh failure.
        if (is_refresh) {
            mark_refresh_failure(shard_idx, key);
        }

        {
            std::lock_guard<std::mutex> lk(inflight->mu);
            inflight->done = true;
            inflight->ok = false;
            inflight->error = "compute failed";
        }
        inflight->cv.notify_all();
    }

    // Cleanup inflight map entry
    Shard& s = shards_[shard_idx];
    {
        std::lock_guard<std::mutex> g(s.inflight_mu);
        s.inflight.erase(key);
    }
}

std::string ShardedEngine::get_or_compute(
    const std::string& key,
    const std::function<std::pair<std::string, PutOptions>()>& compute_fn
) {
    // 1) Fast path: cache hit (fresh only)
    if (auto hit = get(key)) {
        return *hit;
    }

    // 2) Single-flight install/join
    const size_t idx = shard_index(key);
    Shard& s = shards_[idx];

    std::shared_ptr<Inflight> inflight;
    bool leader = false;

    {
        std::lock_guard<std::mutex> g(s.inflight_mu);
        auto it = s.inflight.find(key);
        if (it == s.inflight.end()) {
            inflight = std::make_shared<Inflight>();
            s.inflight.emplace(key, inflight);
            leader = true;
            inflight_leaders_.fetch_add(1, std::memory_order_relaxed);
        } else {
            inflight = it->second;
            inflight_waits_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 3) If leader, schedule compute onto pool (or run inline if needed)
    if (leader) {
        auto fn_copy = std::function<std::pair<std::string, PutOptions>()>(compute_fn);
        bool scheduled = false;

        if (compute_pool_) {
            scheduled = compute_pool_->try_submit(
                [this, idx, k = std::string(key), inflight, fn = std::move(fn_copy)](std::stop_token) mutable {
                    run_compute_and_publish(idx, std::move(k), inflight, std::move(fn), /*is_refresh=*/false);
                });

            if (!scheduled) {
                compute_queue_rejects_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (!scheduled) {
            if (cfg_.backpressure == BackpressureMode::FailFast) {
                {
                    std::lock_guard<std::mutex> lk(inflight->mu);
                    inflight->done = true;
                    inflight->ok = false;
                    inflight->error = "compute queue full (fail fast)";
                }
                inflight->cv.notify_all();

                std::lock_guard<std::mutex> g(s.inflight_mu);
                s.inflight.erase(key);

                return "";
            }

            // RunInline fallback
            run_compute_and_publish(idx, std::string(key), inflight, std::move(fn_copy), /*is_refresh=*/false);
        }
    }

    // 4) Wait for result with timeout governance
    std::unique_lock<std::mutex> lk(inflight->mu);

    if (cfg_.compute_timeout_ms == 0) {
        inflight->cv.wait(lk, [&] { return inflight->done; });
    } else {
        const auto timeout = std::chrono::milliseconds(cfg_.compute_timeout_ms);
        if (!inflight->cv.wait_for(lk, timeout, [&] { return inflight->done; })) {
            compute_timeouts_.fetch_add(1, std::memory_order_relaxed);
            return "";
        }
    }

    if (!inflight->ok) return "";
    return inflight->value;
}

void ShardedEngine::trigger_refresh(
    const std::string& key,
    std::function<std::pair<std::string, PutOptions>()> compute_fn
) {
    const size_t idx = shard_index(key);
    Shard& s = shards_[idx];

    std::shared_ptr<Inflight> inflight;
    bool leader = false;

    {
        std::lock_guard<std::mutex> g(s.inflight_mu);
        auto it = s.inflight.find(key);
        if (it == s.inflight.end()) {
            inflight = std::make_shared<Inflight>();
            s.inflight.emplace(key, inflight);
            leader = true;
        } else {
            refresh_skipped_inflight_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // If somehow not leader (shouldn't happen), bail
    if (!leader) return;

    refresh_scheduled_.fetch_add(1, std::memory_order_relaxed);

    bool scheduled = false;
    if (compute_pool_) {
        scheduled = compute_pool_->try_submit(
            [this, idx, k = std::string(key), inflight, fn = std::move(compute_fn)](std::stop_token) mutable {
                run_compute_and_publish(idx, std::move(k), inflight, std::move(fn), /*is_refresh=*/true);
            });

        if (!scheduled) {
            compute_queue_rejects_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (!scheduled) {
        if (cfg_.backpressure == BackpressureMode::FailFast) {
            // Mark refresh failure but keep the stale entry (if any)
            mark_refresh_failure(idx, key);

            // Cleanup inflight immediately
            std::lock_guard<std::mutex> g(s.inflight_mu);
            s.inflight.erase(key);
            return;
        }

        // RunInline fallback
        run_compute_and_publish(idx, std::string(key), inflight, std::move(compute_fn), /*is_refresh=*/true);
    }
}

// -------------------------
// Stats + sweeper
// -------------------------

EngineStats ShardedEngine::stats_snapshot() const {
    EngineStats s;
    s.hits = hits_.load(std::memory_order_relaxed);
    s.stale_hits = stale_hits_.load(std::memory_order_relaxed);
    s.misses = misses_.load(std::memory_order_relaxed);
    s.puts = puts_.load(std::memory_order_relaxed);
    s.evictions = evictions_.load(std::memory_order_relaxed);
    s.expirations = expirations_.load(std::memory_order_relaxed);
    s.inflight_waits = inflight_waits_.load(std::memory_order_relaxed);
    s.inflight_leaders = inflight_leaders_.load(std::memory_order_relaxed);

    s.compute_queue_rejects = compute_queue_rejects_.load(std::memory_order_relaxed);
    s.compute_timeouts = compute_timeouts_.load(std::memory_order_relaxed);

    s.refresh_scheduled = refresh_scheduled_.load(std::memory_order_relaxed);
    s.refresh_skipped_inflight = refresh_skipped_inflight_.load(std::memory_order_relaxed);
    s.refresh_failures = refresh_failures_.load(std::memory_order_relaxed);

    return s;
}

void ShardedEngine::remove_entry_locked(Shard& s, const std::string& key) {
    auto it = s.entries.find(key);
    if (it == s.entries.end()) return;

    for (const auto& tag : it->second.tags) {
        auto tit = s.tag_index.find(tag);
        if (tit != s.tag_index.end()) {
            tit->second.erase(key);
            if (tit->second.empty()) s.tag_index.erase(tit);
        }
    }

    if (s.bytes >= it->second.size_bytes) s.bytes -= it->second.size_bytes;
    else s.bytes = 0;

    s.entries.erase(it);
}

void ShardedEngine::expire_and_evict_locked(Shard& s, uint64_t shard_budget_bytes) {
    const uint64_t now = now_ms();

    // Expire pass (bounded)
    std::vector<std::string> expired;
    expired.reserve(128);

    for (const auto& kv : s.entries) {
        const uint64_t hard_expire = (kv.second.stale_until_ms > 0) ? kv.second.stale_until_ms : kv.second.fresh_until_ms;
        if (hard_expire > 0 && hard_expire <= now) {
            expired.push_back(kv.first);
            if (expired.size() >= 256) break;
        }
    }
    for (const auto& k : expired) {
        remove_entry_locked(s, k);
        expirations_.fetch_add(1, std::memory_order_relaxed);
    }

    if (s.bytes <= shard_budget_bytes) return;

    // Approx eviction (bounded)
    size_t evicted = 0;
    for (auto it = s.entries.begin(); it != s.entries.end() && s.bytes > shard_budget_bytes;) {
        const std::string k = it->first;
        ++it;
        remove_entry_locked(s, k);
        evictions_.fetch_add(1, std::memory_order_relaxed);
        if (++evicted >= 128) break;
    }
}

void ShardedEngine::sweeper_loop(std::stop_token st) {
    const auto sleep_dur = std::chrono::milliseconds(cfg_.sweep_interval_ms);
    while (!st.stop_requested()) {
        std::this_thread::sleep_for(sleep_dur);

        const uint64_t shard_budget = cfg_.max_bytes_total / shards_.size();
        for (auto& s : shards_) {
            std::unique_lock lk(s.mu);
            expire_and_evict_locked(s, shard_budget);
        }
    }
}

void ShardedEngine::shutdown() {
    // 1. Stop the Sweeper thread
    if (sweeper_.joinable()) {
        sweeper_.request_stop();
        sweeper_.join();
    }

    // 2. Stop the Compute Pool (Worker Threads)
    // This assumes your ThreadPool has a stop/join mechanism.
    if (compute_pool_) {
        // You need to implement this in your ThreadPool if it doesn't exist.
        // It should set a 'stop' flag and join all worker threads.
        compute_pool_->shutdown(); 
    }
}



} // namespace dbwaller::core
