#pragma once

#include "dbwaller/adapters/adapter.hpp"
#include "dbwaller/policy/cache_key.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dbwaller::policy {

enum class CacheScope {
    Public,
    PerViewer,
    PerTenant,
    NoStore
};

struct Rule {
    CacheScope scope = CacheScope::PerViewer;

    uint64_t ttl_ms = 1000;
    uint64_t min_ttl_ms = 50;
    uint64_t max_ttl_ms = 30'000;

    // SWR controls
    uint64_t swr_ms = 0;
    uint64_t stale_ttl_ms = 0;
    bool serve_stale_on_error = false;

    bool vary_by_claims = false;
    bool require_claims_fingerprint = false;

    std::vector<std::string> extra_tags;
};

struct Decision {
    std::string cache_key;
    uint64_t ttl_ms = 1000;

    // SWR controls
    uint64_t swr_ms = 0;
    uint64_t stale_ttl_ms = 0;
    bool serve_stale_on_error = false;

    std::vector<std::string> tags;
    bool cacheable = true;
    CacheScope scope = CacheScope::PerViewer;
};

class PolicyRuleSet {
public:
    void set_default(Rule r) { default_ = std::move(r); }

    void set_rule(std::string ns, Rule r) {
        rules_[std::move(ns)] = std::move(r);
    }

    Decision decide(
        std::string_view ns,
        std::string_view id,
        std::string_view op,
        const dbwaller::adapters::RequestContext& ctx,
        bool /*vary_by_claims param ignored here; rule owns it*/ = false
    ) const {
        Rule r = default_;
        if (auto it = rules_.find(std::string(ns)); it != rules_.end()) {
            r = it->second;
        }

        Decision d;
        d.scope = r.scope;
        d.cacheable = (r.scope != CacheScope::NoStore);

        // Build canonical key
        CacheKeyBuilder b;
        b.version("v1").ns(ns).op(op).id(id);

        if (!ctx.tenant.empty()) b.dim("tenant", ctx.tenant);

        switch (r.scope) {
            case CacheScope::Public:
                break;
            case CacheScope::PerViewer:
                b.dim("viewer", ctx.viewer_id.empty() ? "anon" : ctx.viewer_id);
                break;
            case CacheScope::PerTenant:
                break;
            case CacheScope::NoStore:
                b.dim("viewer", ctx.viewer_id.empty() ? "anon" : ctx.viewer_id);
                break;
        }

        if (!ctx.locale.empty()) b.dim("loc", ctx.locale);

        // Claims vary-by
        if (r.vary_by_claims) {
            if (r.require_claims_fingerprint && ctx.claims_fingerprint.empty()) {
                // Fail-closed: treat as NoStore
                d.cacheable = false;
            } else if (!ctx.claims_fingerprint.empty()) {
                b.dim("claims", ctx.claims_fingerprint);
            }
        }

        d.cache_key = b.build();

        auto clamp = [&](uint64_t ttl) -> uint64_t {
            if (ttl < r.min_ttl_ms) return r.min_ttl_ms;
            if (ttl > r.max_ttl_ms) return r.max_ttl_ms;
            return ttl;
        };

        d.ttl_ms = clamp(r.ttl_ms);

        // Carry SWR settings straight through
        d.swr_ms = r.swr_ms;
        d.stale_ttl_ms = r.stale_ttl_ms;
        d.serve_stale_on_error = r.serve_stale_on_error;

        d.tags.push_back(std::string(ns) + ":" + std::string(id));
        for (const auto& t : r.extra_tags) d.tags.push_back(t);

        return d;
    }

private:
    Rule default_{};
    std::unordered_map<std::string, Rule> rules_;
};

} // namespace dbwaller::policy
