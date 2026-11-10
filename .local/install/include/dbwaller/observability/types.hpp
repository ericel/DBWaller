#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace dbwaller::observability {

// How much of the cached value we return.
// NOTE: DBWaller stores *encrypted_value*; raw exposure can still be sensitive.
// Keep default = Redacted and protect any HTTP endpoints.
enum class ValueMode {
    Redacted,   // No value returned
    SizeOnly,   // Only size returned
    Hash64,     // Stable 64-bit hash (non-crypto) of value
    Preview,    // First N bytes (still risky)
    Raw         // Full encrypted_value (risky)
};

struct KeyInfo {
    std::string key;
    uint32_t size_bytes = 0;
    uint64_t ttl_ms_remaining = 0;   // 0 if expired or unknown
    bool is_fresh = false;
    bool is_stale_swr = false;
    bool is_stale_if_error = false;
};

struct EntryInfo {
    std::string key;

    // Stored encrypted blob metadata
    uint32_t size_bytes = 0;

    // Freshness windows (same semantics as your CacheEntry)
    uint64_t fresh_until_ms = 0;
    uint64_t swr_until_ms = 0;
    uint64_t stale_until_ms = 0;

    bool serve_stale_on_error = false;

    // Refresh status
    bool refresh_attempted = false;
    bool last_refresh_ok = true;
    uint64_t last_refresh_ms = 0;

    // Derived at read time
    uint64_t now_ms = 0;
    uint64_t ttl_ms_remaining = 0;
    std::string state; // "fresh" | "stale_swr" | "stale_if_error" | "expired"

    // Tags (optional)
    std::vector<std::string> tags;

    // Value (optional)
    std::string value;          // preview/raw depending on request
    uint64_t value_hash64 = 0;  // only when Hash64 requested
};

struct StatsView {
    uint64_t now_ms = 0;

    // Capacity config
    uint64_t max_bytes_total = 0;
    size_t num_shards = 0;

    // Current usage
    uint64_t total_bytes = 0;
    uint64_t total_keys = 0;
    std::vector<uint64_t> bytes_per_shard;
    std::vector<uint64_t> keys_per_shard;

    // Counters (mirrors your atomics)
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

struct ListKeysRequest {
    std::string prefix;
    size_t limit = 200;              // keep small for debug endpoints
    std::string cursor_after_key;    // lexicographic pagination
    bool stable_sort = true;         // if true, sorts keys (more CPU)
};

struct ListKeysResponse {
    std::vector<KeyInfo> keys;
    bool truncated = false;
    std::string next_cursor_after_key; // last key returned
};

struct GetEntryRequest {
    std::string key;
    ValueMode value_mode = ValueMode::Redacted;
    size_t preview_bytes = 64; // used when Preview mode
    bool include_tags = false;
};

} // namespace dbwaller::observability
