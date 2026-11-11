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

ShardedEngine::~ShardedEngine() {
    shutdown();
}

ShardedEngine::ComputeCall::ComputeCall(ShardedEngine& engine) : engine_(engine) {
    std::lock_guard lock(engine_.lifecycle_mu_);
    admitted_ = !engine_.closing_;
    if (admitted_) ++engine_.active_calls_;
}

ShardedEngine::ComputeCall::~ComputeCall() {
    if (!admitted_) return;
    std::lock_guard lock(engine_.lifecycle_mu_);
    --engine_.active_calls_;
    engine_.lifecycle_cv_.notify_all();
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
    return get_snapshot(key).result;
}

std::pair<ShardedEngine::Snapshot, LoadTicket>
ShardedEngine::get_snapshot_and_begin_load(const std::string& key) {
    std::lock_guard publication_lock(publication_mu_);
    return {get_snapshot(key), LoadTicket(key, generation_)};
}

ShardedEngine::Snapshot ShardedEngine::get_snapshot(const std::string& key) {
    Shard& s = shards_[shard_index(key)];

    {
        std::shared_lock lk(s.mu);
        const uint64_t now = now_ms();
        auto it = s.entries.find(key);
        if (it == s.entries.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        const CacheEntry& e = it->second;
        uint64_t servable_until = e.swr_until_ms > 0 ? e.swr_until_ms : e.fresh_until_ms;
        if (e.serve_stale_on_error && e.refresh_attempted && !e.last_refresh_ok &&
            e.stale_until_ms > servable_until) {
            servable_until = e.stale_until_ms;
        }

        // Fresh
        if (now < e.fresh_until_ms) {
            hits_.fetch_add(1, std::memory_order_relaxed);
            return {{GetKind::Fresh, e.encrypted_value}, e.fresh_until_ms - now,
                    servable_until - now};
        }

        // Stale (SWR window)
        if (e.swr_until_ms > 0 && now < e.swr_until_ms) {
            stale_hits_.fetch_add(1, std::memory_order_relaxed);
            return {{GetKind::Stale, e.encrypted_value}, 0, servable_until - now};
        }

        // Stale-if-error window (only if refresh attempted & failed)
        if (e.stale_until_ms > 0 && now < e.stale_until_ms &&
            e.serve_stale_on_error &&
            e.refresh_attempted &&
            !e.last_refresh_ok) {
            stale_hits_.fetch_add(1, std::memory_order_relaxed);
            return {{GetKind::Stale, e.encrypted_value}, 0, servable_until - now};
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
    return {};
}

void ShardedEngine::put(const std::string& key, std::string encrypted_value, const PutOptions& opt) {
    std::lock_guard publication_lock(publication_mu_);
    put_locked(key, std::move(encrypted_value), opt);
}

LoadTicket ShardedEngine::begin_load(const std::string& key) {
    std::lock_guard publication_lock(publication_mu_);
    return LoadTicket(key, generation_);
}

PublishStatus ShardedEngine::put_if_current(const LoadTicket& ticket,
    std::string encrypted_value, const PutOptions& opt) {
    std::lock_guard publication_lock(publication_mu_);
    if (ticket.generation_ != generation_) return PublishStatus::Invalidated;
    put_locked(ticket.key_, std::move(encrypted_value), opt);
    return PublishStatus::Stored;
}

void ShardedEngine::put_locked(const std::string& key, std::string encrypted_value, const PutOptions& opt) {
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
    if (cfg_.max_entries_total) {
        // Daemon mode needs strict value and metadata bounds at publication,
        // independently of the sweeper (including zero-byte values).
        const size_t entries = shard_entry_budget(s);
        while (s.entries.size() > entries || s.bytes > shard_budget) {
            remove_entry_locked(s, s.entries.begin()->first);
            evictions_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (s.bytes > shard_budget) {
        expire_and_evict_locked(s, shard_budget);
    }
}

void ShardedEngine::invalidate_key(const std::string& key) {
    std::lock_guard publication_lock(publication_mu_);
    generation_ = std::make_shared<const LoadTicket::Generation>();
    Shard& s = shards_[shard_index(key)];
    std::unique_lock lk(s.mu);
    if (s.entries.find(key) != s.entries.end()) {
        remove_entry_locked(s, key);
        evictions_.fetch_add(1, std::memory_order_relaxed);
    }
}

void ShardedEngine::invalidate_tag(const std::string& tag) {
    std::lock_guard publication_lock(publication_mu_);
    generation_ = std::make_shared<const LoadTicket::Generation>();
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

void ShardedEngine::mark_refresh_failure(size_t shard_idx, const std::string& key,
    const std::shared_ptr<Inflight>& inflight) {
    std::lock_guard publication_lock(publication_mu_);
    if (inflight->ticket.generation_ != generation_) return;
    Shard& s = shards_[shard_idx];
    std::unique_lock lk(s.mu);
    auto it = s.entries.find(key);
    if (it == s.entries.end() || it->second.version != inflight->refresh_entry) return;

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

        const auto status = put_if_current(inflight->ticket, value, opt);

        finish_compute(shard_idx, key, inflight, std::move(value), status == PublishStatus::Stored);
    } catch (...) {
        // If this is a refresh: DO NOT delete cache entry. Mark refresh failure.
        if (is_refresh) {
            mark_refresh_failure(shard_idx, key, inflight);
        }

        finish_compute(shard_idx, key, inflight, {}, false);
    }
}

void ShardedEngine::finish_compute(size_t shard_idx, const std::string& key,
    const std::shared_ptr<Inflight>& inflight, std::string value, bool ok) {
    Shard& s = shards_[shard_idx];
    {
        std::lock_guard publication_lock(publication_mu_);
        std::lock_guard<std::mutex> g(s.inflight_mu);
        {
            std::lock_guard<std::mutex> lock(inflight->mu);
            inflight->invalidated = inflight->ticket.generation_ != generation_;
            inflight->ok = ok && !inflight->invalidated;
            inflight->value = inflight->ok ? std::move(value) : std::string{};
            inflight->done = true;
        }
        // Retire exactly this flight before waking callers that may retry.
        auto it = s.inflight.find(key);
        if (it != s.inflight.end() && it->second == inflight) s.inflight.erase(it);
    }
    inflight->cv.notify_all();
}

std::string ShardedEngine::get_or_compute(
    const std::string& key,
    const std::function<std::pair<std::string, PutOptions>()>& compute_fn
) {
    ComputeCall call(*this);
    if (!call) return {};

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
        std::lock_guard publication_lock(publication_mu_);
        std::lock_guard<std::mutex> g(s.inflight_mu);
        auto it = s.inflight.find(key);
        if (it == s.inflight.end() || it->second->ticket.generation_ != generation_) {
            inflight = std::make_shared<Inflight>(LoadTicket(key, generation_));
            s.inflight.insert_or_assign(key, inflight);
            leader = true;
            inflight_leaders_.fetch_add(1, std::memory_order_relaxed);
        } else {
            inflight = it->second;
            inflight_waits_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 3) If leader, schedule compute onto pool (or run inline if needed)
    if (leader) {
        try {
            dbwaller::concurrency::ThreadPool::Task work =
                [this, idx, k = std::string(key), inflight, fn = compute_fn](std::stop_token) mutable {
                    run_compute_and_publish(idx, std::move(k), inflight, std::move(fn), /*is_refresh=*/false);
                };

            // Keep an owned task until acceptance; a rejected by-value argument
            // otherwise consumes the callable needed for the inline fallback.
            if (!compute_pool_ || !compute_pool_->try_submit(work)) {
                if (compute_pool_) {
                    compute_queue_rejects_.fetch_add(1, std::memory_order_relaxed);
                    if (cfg_.backpressure == BackpressureMode::FailFast) {
                        finish_compute(idx, key, inflight, {}, false);
                    } else {
                        work(std::stop_token{});
                    }
                } else {
                    work(std::stop_token{});
                }
            }
        } catch (...) {
            // Copying/allocating the queued task can fail after installing a
            // flight; retire it and release all callers just like compute errors.
            finish_compute(idx, key, inflight, {}, false);
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

    if (inflight->invalidated) throw LoadInvalidatedError();
    if (!inflight->ok) return "";
    return inflight->value;
}

void ShardedEngine::trigger_refresh(
    const std::string& key,
    std::function<std::pair<std::string, PutOptions>()> compute_fn
) {
    ComputeCall call(*this);
    if (!call) return;

    const size_t idx = shard_index(key);
    Shard& s = shards_[idx];

    std::shared_ptr<Inflight> inflight;
    bool leader = false;

    {
        std::lock_guard publication_lock(publication_mu_);
        std::lock_guard<std::mutex> g(s.inflight_mu);
        auto it = s.inflight.find(key);
        if (it == s.inflight.end() || it->second->ticket.generation_ != generation_) {
            inflight = std::make_shared<Inflight>(LoadTicket(key, generation_));
            {
                std::shared_lock entry_lock(s.mu);
                const auto entry = s.entries.find(key);
                if (entry != s.entries.end()) inflight->refresh_entry = entry->second.version;
            }
            s.inflight.insert_or_assign(key, inflight);
            leader = true;
        } else {
            refresh_skipped_inflight_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // If somehow not leader (shouldn't happen), bail
    if (!leader) return;

    refresh_scheduled_.fetch_add(1, std::memory_order_relaxed);

    try {
        dbwaller::concurrency::ThreadPool::Task work =
            [this, idx, k = std::string(key), inflight, fn = std::move(compute_fn)](std::stop_token) mutable {
                run_compute_and_publish(idx, std::move(k), inflight, std::move(fn), /*is_refresh=*/true);
            };

        if (!compute_pool_ || !compute_pool_->try_submit(work)) {
            if (compute_pool_) {
                compute_queue_rejects_.fetch_add(1, std::memory_order_relaxed);
                if (cfg_.backpressure == BackpressureMode::FailFast) {
                    mark_refresh_failure(idx, key, inflight);
                    finish_compute(idx, key, inflight, {}, false);
                    return;
                }
            }
            work(std::stop_token{});
        }
    } catch (...) {
        mark_refresh_failure(idx, key, inflight);
        finish_compute(idx, key, inflight, {}, false);
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

size_t ShardedEngine::shard_entry_budget(const Shard& s) const {
    const size_t index = static_cast<size_t>(&s - shards_.data());
    return cfg_.max_entries_total / shards_.size() +
        (index < cfg_.max_entries_total % shards_.size() ? 1 : 0);
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
    std::unique_lock sleep_lock(sweeper_mu_);
    while (!st.stop_requested()) {
        sweeper_cv_.wait_for(sleep_lock, st, sleep_dur, [] { return false; });
        if (st.stop_requested()) break;

        const uint64_t shard_budget = cfg_.max_bytes_total / shards_.size();
        for (auto& s : shards_) {
            std::unique_lock lk(s.mu);
            expire_and_evict_locked(s, shard_budget);
        }
    }
}

void ShardedEngine::shutdown() {
    std::lock_guard shutdown_lock(shutdown_mu_);
    {
        std::lock_guard lock(lifecycle_mu_);
        closing_ = true;
    }

    if (sweeper_.joinable()) {
        sweeper_.request_stop();
        sweeper_.join();
    }

    if (compute_pool_) compute_pool_->shutdown();

    // A caller admitted before closure can still be running an inline fallback
    // or waiting for a shared result. Neither path is represented by pool joins.
    std::unique_lock lock(lifecycle_mu_);
    lifecycle_cv_.wait(lock, [&] { return active_calls_ == 0; });
}



} // namespace dbwaller::core
