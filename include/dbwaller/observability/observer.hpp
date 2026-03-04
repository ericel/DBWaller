#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "dbwaller/observability/types.hpp"

namespace dbwaller::core {
class ShardedEngine; // forward-declare
}

namespace dbwaller::observability {

// Lightweight facade that can be handed to implementers / controllers.
class Observer {
public:
    explicit Observer(const dbwaller::core::ShardedEngine* engine) : engine_(engine) {}

    // High-level stats: bytes, keys, counters, per-shard usage
    StatsView stats() const;

    // List keys (optionally prefix-filtered) with minimal per-key info
    ListKeysResponse list_keys(const ListKeysRequest& req) const;

    // Inspect one entry (value redacted by default)
    std::optional<EntryInfo> get_entry(const GetEntryRequest& req) const;

private:
    const dbwaller::core::ShardedEngine* engine_ = nullptr;

    // helpers
    static uint64_t fnv1a64(std::string_view s);
    static std::string escape_for_state(std::string_view s); // not JSON, just safe-ish
};

} // namespace dbwaller::observability
