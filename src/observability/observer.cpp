#include "dbwaller/observability/observer.hpp"

// We need internal access to shards_ / cfg_ / atomics_
// Add `friend class dbwaller::observability::Observer;` inside ShardedEngine.
#include "dbwaller/core/sharded_engine.hpp"

#include <algorithm>
#include <chrono>
#include <shared_mutex>

namespace dbwaller::observability {

uint64_t Observer::fnv1a64(std::string_view s) {
    // Non-cryptographic stable hash for debugging (DO NOT use for security).
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::string Observer::escape_for_state(std::string_view s) {
    // Minimal sanitizer for state text (not JSON escaping).
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= 32 && c != '\n' && c != '\r' && c != '\t') out.push_back(c);
        else out.push_back(' ');
    }
    return out;
}

StatsView Observer::stats() const {
    StatsView v{};
    if (!engine_) return v;

    v.now_ms = dbwaller::core::ShardedEngine::now_ms();
    v.max_bytes_total = engine_->cfg_.max_bytes_total;
    v.num_shards = engine_->shards_.size();

    v.bytes_per_shard.resize(v.num_shards, 0);
    v.keys_per_shard.resize(v.num_shards, 0);

    // Aggregate shard sizes under shared locks
    for (size_t i = 0; i < engine_->shards_.size(); ++i) {
        const auto& shard = engine_->shards_[i];
        std::shared_lock lk(shard.mu);
        v.bytes_per_shard[i] = shard.bytes;
        v.keys_per_shard[i] = static_cast<uint64_t>(shard.entries.size());
        v.total_bytes += shard.bytes;
        v.total_keys += static_cast<uint64_t>(shard.entries.size());
    }

    // Atomics (direct)
    v.hits = engine_->hits_.load(std::memory_order_relaxed);
    v.stale_hits = engine_->stale_hits_.load(std::memory_order_relaxed);
    v.misses = engine_->misses_.load(std::memory_order_relaxed);
    v.puts = engine_->puts_.load(std::memory_order_relaxed);
    v.evictions = engine_->evictions_.load(std::memory_order_relaxed);
    v.expirations = engine_->expirations_.load(std::memory_order_relaxed);

    v.inflight_leaders = engine_->inflight_leaders_.load(std::memory_order_relaxed);
    v.inflight_waits = engine_->inflight_waits_.load(std::memory_order_relaxed);

    v.compute_queue_rejects = engine_->compute_queue_rejects_.load(std::memory_order_relaxed);
    v.compute_timeouts = engine_->compute_timeouts_.load(std::memory_order_relaxed);

    v.refresh_scheduled = engine_->refresh_scheduled_.load(std::memory_order_relaxed);
    v.refresh_skipped_inflight = engine_->refresh_skipped_inflight_.load(std::memory_order_relaxed);
    v.refresh_failures = engine_->refresh_failures_.load(std::memory_order_relaxed);

    return v;
}

ListKeysResponse Observer::list_keys(const ListKeysRequest& req) const {
    ListKeysResponse resp{};
    if (!engine_) return resp;

    const uint64_t now = dbwaller::core::ShardedEngine::now_ms();

    // Collect candidate keys (and minimal metadata)
    std::vector<KeyInfo> all;
    all.reserve(512);

    for (const auto& shard : engine_->shards_) {
        std::shared_lock lk(shard.mu);

        for (const auto& kv : shard.entries) {
            const std::string& key = kv.first;
            const auto& e = kv.second;

            if (!req.prefix.empty() && key.rfind(req.prefix, 0) != 0) continue; // prefix match
            if (!req.cursor_after_key.empty() && key <= req.cursor_after_key) continue;

            KeyInfo ki{};
            ki.key = key;
            ki.size_bytes = e.size_bytes;

            // Derive state
            const bool fresh = now < e.fresh_until_ms;
            const bool stale_swr = (!fresh) && (now < e.swr_until_ms);
            const bool stale_if_error = (!fresh) && (!stale_swr) && (now < e.stale_until_ms) && e.serve_stale_on_error;

            ki.is_fresh = fresh;
            ki.is_stale_swr = stale_swr;
            ki.is_stale_if_error = stale_if_error;

            // TTL remaining relative to stale_until_ms (0 if expired)
            if (now < e.stale_until_ms) ki.ttl_ms_remaining = e.stale_until_ms - now;

            all.push_back(std::move(ki));
        }
    }

    if (req.stable_sort) {
        std::sort(all.begin(), all.end(), [](const KeyInfo& a, const KeyInfo& b) {
            return a.key < b.key;
        });
    }

    const size_t limit = (req.limit == 0) ? 200 : req.limit;

    if (all.size() > limit) {
        resp.truncated = true;
        all.resize(limit);
    }

    if (!all.empty()) {
        resp.next_cursor_after_key = all.back().key;
    }

    resp.keys = std::move(all);
    return resp;
}

std::optional<EntryInfo> Observer::get_entry(const GetEntryRequest& req) const {
    if (!engine_) return std::nullopt;

    const uint64_t now = dbwaller::core::ShardedEngine::now_ms();
    const size_t shard_idx = engine_->shard_index(req.key);
    auto& shard = engine_->shards_[shard_idx];

    std::shared_lock lk(shard.mu);
    auto it = shard.entries.find(req.key);
    if (it == shard.entries.end()) return std::nullopt;

    const auto& e = it->second;

    EntryInfo out{};
    out.key = req.key;
    out.size_bytes = e.size_bytes;

    out.fresh_until_ms = e.fresh_until_ms;
    out.swr_until_ms = e.swr_until_ms;
    out.stale_until_ms = e.stale_until_ms;

    out.serve_stale_on_error = e.serve_stale_on_error;

    out.refresh_attempted = e.refresh_attempted;
    out.last_refresh_ok = e.last_refresh_ok;
    out.last_refresh_ms = e.last_refresh_ms;

    out.now_ms = now;
    out.ttl_ms_remaining = (now < e.stale_until_ms) ? (e.stale_until_ms - now) : 0;

    const bool fresh = now < e.fresh_until_ms;
    const bool stale_swr = (!fresh) && (now < e.swr_until_ms);
    const bool stale_if_error = (!fresh) && (!stale_swr) && (now < e.stale_until_ms) && e.serve_stale_on_error;

    if (fresh) out.state = "fresh";
    else if (stale_swr) out.state = "stale_swr";
    else if (stale_if_error) out.state = "stale_if_error";
    else out.state = "expired";

    if (req.include_tags) {
        out.tags = e.tags;
    }

    // Value policy
    switch (req.value_mode) {
        case ValueMode::Redacted:
            // nothing
            break;
        case ValueMode::SizeOnly:
            // nothing (size already present)
            break;
        case ValueMode::Hash64:
            out.value_hash64 = fnv1a64(e.encrypted_value);
            break;
        case ValueMode::Preview: {
            const size_t n = (req.preview_bytes == 0) ? 64 : req.preview_bytes;
            out.value = e.encrypted_value.substr(0, std::min(n, e.encrypted_value.size()));
            break;
        }
        case ValueMode::Raw:
            out.value = e.encrypted_value;
            break;
    }

    return out;
}

} // namespace dbwaller::observability
