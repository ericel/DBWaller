#pragma once

#include "dbwaller/adapters/adapter.hpp"
#include "dbwaller/policy/cache_key.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dbwaller::policy {

/**
 * CachePolicy:
 * - Determines vary-by dimensions
 * - Determines TTL bounds
 * - Can deny caching (e.g., private data)
 *
 * Start simple; later we add:
 * - allow/deny rules based on claims
 * - field masks
 * - per-namespace TTL budgets
 * - negative cache policy
 */
class CachePolicy {
public:
    struct Decision {
        std::string cache_key;
        uint64_t ttl_ms = 1000;
        std::vector<std::string> tags;
        bool cacheable = true;
    };

    // Build a safe key for "object fetch"
    Decision make_object_key(
        std::string_view ns,
        std::string_view id,
        std::string_view op,
        const dbwaller::adapters::RequestContext& ctx
    ) const {
        Decision d;

        // Baseline: tenant + viewer must be included for authenticated data.
        // You can relax later for explicitly public objects.
        dbwaller::policy::CacheKeyBuilder b;
        d.cache_key = b.version("v1")
                        .ns(ns)
                        .op(op)
                        .id(id)
                        .vary_tenant(ctx)
                        .vary_viewer(ctx)
                        .vary_locale(ctx)
                        .build();

        // Default TTL clamp
        d.ttl_ms = clamp_ttl_ms(1000);

        // Default tag scheme
        d.tags = {std::string(ns) + ":" + std::string(id)};

        d.cacheable = true;
        return d;
    }

    // Clamp TTL to safe bounds (avoid ultra-long accidental caching)
    uint64_t clamp_ttl_ms(uint64_t ttl_ms) const {
        if (ttl_ms < min_ttl_ms_) return min_ttl_ms_;
        if (ttl_ms > max_ttl_ms_) return max_ttl_ms_;
        return ttl_ms;
    }

    void set_ttl_bounds(uint64_t min_ms, uint64_t max_ms) {
        min_ttl_ms_ = min_ms;
        max_ttl_ms_ = max_ms;
    }

private:
    uint64_t min_ttl_ms_ = 50;
    uint64_t max_ttl_ms_ = 30'000; // 30s default safety cap for early builds
};

} // namespace dbwaller::policy
