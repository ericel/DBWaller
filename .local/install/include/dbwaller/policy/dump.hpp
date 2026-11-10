#pragma once

#include "dbwaller/policy/presets.hpp"
#include "dbwaller/policy/rules.hpp"

#include <algorithm>
#include <iostream>
#include <string>

namespace dbwaller::policy {

inline const char* scope_to_string(CacheScope s) {
    switch (s) {
        case CacheScope::Public: return "Public";
        case CacheScope::PerViewer: return "PerViewer";
        case CacheScope::PerTenant: return "PerTenant";
        case CacheScope::NoStore: return "NoStore";
    }
    return "Unknown";
}

inline void dump_rule(std::ostream& os, const Rule& r) {
    os << "scope=" << scope_to_string(r.scope)
       << " ttl_ms=" << r.ttl_ms
       << " min_ttl_ms=" << r.min_ttl_ms
       << " max_ttl_ms=" << r.max_ttl_ms
       << " swr_ms=" << r.swr_ms
       << " stale_ttl_ms=" << r.stale_ttl_ms
       << " serve_stale_on_error=" << (r.serve_stale_on_error ? "true" : "false")
       << " vary_by_claims=" << (r.vary_by_claims ? "true" : "false")
       << " require_claims_fingerprint=" << (r.require_claims_fingerprint ? "true" : "false");
}

inline void dump_config(std::ostream& os, const NamespacePolicyConfig& cfg) {
    os << "DBWaller Policy Config (raw)\n";
    os << "- default:\n  ";
    dump_rule(os, cfg.default_rule);
    os << "\n";

    os << "- denylist (" << cfg.denylist.size() << "): ";
    for (size_t i = 0; i < cfg.denylist.size(); ++i) {
        os << cfg.denylist[i];
        if (i + 1 < cfg.denylist.size()) os << ",";
    }
    os << "\n";

    os << "- presets (" << cfg.presets.size() << "):\n";
    for (const auto& [ns, r] : cfg.presets) {
        os << "  [" << ns << "] ";
        dump_rule(os, r);
        os << "\n";
    }
}

inline void dump_effective_rules(std::ostream& os, const NamespacePolicyConfig& cfg) {
    os << "DBWaller Policy Effective Rules (denylist wins)\n";
    os << "- default:\n  ";
    dump_rule(os, cfg.default_rule);
    os << "\n";

    os << "- namespaces (" << cfg.presets.size() << "):\n";
    for (const auto& [ns, rule] : cfg.presets) {
        Rule r = rule;

        const bool denied =
            std::find(cfg.denylist.begin(), cfg.denylist.end(), ns) != cfg.denylist.end();

        if (denied) {
            r.scope = CacheScope::NoStore;
            r.vary_by_claims = false;
            r.require_claims_fingerprint = false;
            r.swr_ms = 0;
            r.stale_ttl_ms = 0;
            r.serve_stale_on_error = false;
        }

        os << "  [" << ns << "] ";
        dump_rule(os, r);
        os << "\n";
    }
}

} // namespace dbwaller::policy

