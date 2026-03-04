#pragma once

#include "dbwaller/policy/rules.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dbwaller::policy {

/**
 * NamespacePolicyConfig:
 * - default_rule: baseline rule when namespace has no preset
 * - presets: per-namespace overrides
 * - denylist: namespaces that must be forced to NoStore (wins over everything)
 */
struct NamespacePolicyConfig {
    Rule default_rule{};
    std::unordered_map<std::string, Rule> presets;
    std::vector<std::string> denylist;
};

inline PolicyRuleSet build_ruleset_from_config(const NamespacePolicyConfig& cfg) {
    PolicyRuleSet rs;
    rs.set_default(cfg.default_rule);

    std::unordered_set<std::string> deny;
    deny.reserve(cfg.denylist.size());
    for (const auto& ns : cfg.denylist) {
        deny.insert(ns);
    }

    // Apply presets (denylist wins)
    for (const auto& [ns, rule] : cfg.presets) {
        Rule r = rule;

        if (deny.contains(ns)) {
            r.scope = CacheScope::NoStore;
            r.vary_by_claims = false;
            r.require_claims_fingerprint = false;
            r.swr_ms = 0;
            r.stale_ttl_ms = 0;
        }

        rs.set_rule(ns, std::move(r));
    }

    // Ensure denylist namespaces exist even if not in presets
    for (const auto& ns : cfg.denylist) {
        if (cfg.presets.find(ns) != cfg.presets.end()) continue;

        Rule r = cfg.default_rule;
        r.scope = CacheScope::NoStore;
        r.vary_by_claims = false;
        r.require_claims_fingerprint = false;
        r.swr_ms = 0;
        r.stale_ttl_ms = 0;

        rs.set_rule(ns, std::move(r));
    }

    return rs;
}

} // namespace dbwaller::policy
