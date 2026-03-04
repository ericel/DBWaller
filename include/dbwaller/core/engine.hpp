#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dbwaller::core {

struct PutOptions {
    uint64_t ttl_ms = 5000;                 // Entry TTL in milliseconds
    std::vector<std::string> tags;          // Invalidation tags
};

struct EngineStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t puts = 0;
    uint64_t evictions = 0;
    uint64_t expirations = 0;
    uint64_t inflight_waits = 0;
    uint64_t inflight_leaders = 0;

    // NEW: governance stats
    uint64_t compute_queue_rejects = 0;   // queue full events
    uint64_t compute_timeouts = 0;        // waiter timed out
};

class Engine {
public:
    virtual ~Engine() = default;

    // Returns encrypted value bytes on hit.
    virtual std::optional<std::string> get(const std::string& key) = 0;

    // Stores encrypted value bytes.
    virtual void put(const std::string& key, std::string encrypted_value, const PutOptions& opt) = 0;

    // Invalidate by key or tag.
    virtual void invalidate_key(const std::string& key) = 0;
    virtual void invalidate_tag(const std::string& tag) = 0;

    // Single-flight get-or-compute:
    // compute_fn returns (encrypted_value, PutOptions).
    virtual std::string get_or_compute(
        const std::string& key,
        const std::function<std::pair<std::string, PutOptions>()>& compute_fn) = 0;

    // Snapshot stats (thread-safe).
    virtual EngineStats stats_snapshot() const = 0;
};

} // namespace dbwaller::core
