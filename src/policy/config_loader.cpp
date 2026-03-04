#include "dbwaller/policy/config_loader.hpp"

#include "dbwaller/policy/presets.hpp"
#include "dbwaller/policy/rules.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace dbwaller::policy {

static std::optional<std::string> getenv_str(const char* k) {
    if (const char* v = std::getenv(k)) {
        if (*v == '\0') return std::nullopt;
        return std::string(v);
    }
    return std::nullopt;
}

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            if (!std::isspace(static_cast<unsigned char>(c))) cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

static CacheScope parse_scope(std::string_view s, CacheScope fallback) {
    if (ieq(s, "Public")) return CacheScope::Public;
    if (ieq(s, "PerViewer")) return CacheScope::PerViewer;
    if (ieq(s, "PerTenant")) return CacheScope::PerTenant;
    if (ieq(s, "NoStore")) return CacheScope::NoStore;
    return fallback;
}

static bool parse_bool(std::string_view s, bool fallback) {
    if (ieq(s, "1") || ieq(s, "true") || ieq(s, "yes") || ieq(s, "on")) return true;
    if (ieq(s, "0") || ieq(s, "false") || ieq(s, "no") || ieq(s, "off")) return false;
    return fallback;
}

static uint64_t parse_u64(std::string_view s, uint64_t fallback) {
    try {
        return static_cast<uint64_t>(std::stoull(std::string(s)));
    } catch (...) {
        return fallback;
    }
}

static void dedup_vector(std::vector<std::string>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

static bool file_exists_readable(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

static NamespacePolicyConfig load_policy_json_file(const std::string& path, NamespacePolicyConfig base) {
    if (!file_exists_readable(path)) return base;

    std::ifstream f(path);
    nlohmann::json j;
    f >> j;

    // default
    if (j.contains("default") && j["default"].is_object()) {
        auto& d = j["default"];

        if (d.contains("scope") && d["scope"].is_string())
            base.default_rule.scope = parse_scope(d["scope"].get<std::string>(), base.default_rule.scope);

        if (d.contains("ttl_ms") && d["ttl_ms"].is_number_unsigned())
            base.default_rule.ttl_ms = d["ttl_ms"].get<uint64_t>();

        if (d.contains("min_ttl_ms") && d["min_ttl_ms"].is_number_unsigned())
            base.default_rule.min_ttl_ms = d["min_ttl_ms"].get<uint64_t>();

        if (d.contains("max_ttl_ms") && d["max_ttl_ms"].is_number_unsigned())
            base.default_rule.max_ttl_ms = d["max_ttl_ms"].get<uint64_t>();

        if (d.contains("swr_ms") && d["swr_ms"].is_number_unsigned())
            base.default_rule.swr_ms = d["swr_ms"].get<uint64_t>();

        if (d.contains("stale_ttl_ms") && d["stale_ttl_ms"].is_number_unsigned())
            base.default_rule.stale_ttl_ms = d["stale_ttl_ms"].get<uint64_t>();

        if (d.contains("serve_stale_on_error") && d["serve_stale_on_error"].is_boolean())
            base.default_rule.serve_stale_on_error = d["serve_stale_on_error"].get<bool>();

        if (d.contains("vary_by_claims") && d["vary_by_claims"].is_boolean())
            base.default_rule.vary_by_claims = d["vary_by_claims"].get<bool>();

        if (d.contains("require_claims_fingerprint") && d["require_claims_fingerprint"].is_boolean())
            base.default_rule.require_claims_fingerprint = d["require_claims_fingerprint"].get<bool>();
    }

    // denylist
    if (j.contains("denylist") && j["denylist"].is_array()) {
        for (const auto& ns : j["denylist"]) {
            if (ns.is_string()) base.denylist.push_back(ns.get<std::string>());
        }
    }

    // presets
    if (j.contains("presets") && j["presets"].is_object()) {
        for (auto it = j["presets"].begin(); it != j["presets"].end(); ++it) {
            const std::string ns = it.key();
            const auto& o = it.value();
            if (!o.is_object()) continue;

            Rule r = base.default_rule;

            if (o.contains("scope") && o["scope"].is_string())
                r.scope = parse_scope(o["scope"].get<std::string>(), r.scope);

            if (o.contains("ttl_ms") && o["ttl_ms"].is_number_unsigned())
                r.ttl_ms = o["ttl_ms"].get<uint64_t>();

            if (o.contains("min_ttl_ms") && o["min_ttl_ms"].is_number_unsigned())
                r.min_ttl_ms = o["min_ttl_ms"].get<uint64_t>();

            if (o.contains("max_ttl_ms") && o["max_ttl_ms"].is_number_unsigned())
                r.max_ttl_ms = o["max_ttl_ms"].get<uint64_t>();

            if (o.contains("swr_ms") && o["swr_ms"].is_number_unsigned())
                r.swr_ms = o["swr_ms"].get<uint64_t>();

            if (o.contains("stale_ttl_ms") && o["stale_ttl_ms"].is_number_unsigned())
                r.stale_ttl_ms = o["stale_ttl_ms"].get<uint64_t>();

            if (o.contains("serve_stale_on_error") && o["serve_stale_on_error"].is_boolean())
                r.serve_stale_on_error = o["serve_stale_on_error"].get<bool>();

            if (o.contains("vary_by_claims") && o["vary_by_claims"].is_boolean())
                r.vary_by_claims = o["vary_by_claims"].get<bool>();

            if (o.contains("require_claims_fingerprint") && o["require_claims_fingerprint"].is_boolean())
                r.require_claims_fingerprint = o["require_claims_fingerprint"].get<bool>();

            base.presets[ns] = std::move(r);
        }
    }

    dedup_vector(base.denylist);
    return base;
}

// internal helper (in-place)
static void apply_env_overrides_inplace(NamespacePolicyConfig& base) {
    // Denylist CSV env: DBWALLER_DENYLIST=session,adminview
    if (auto v = getenv_str("DBWALLER_DENYLIST")) {
        for (const auto& ns : split_csv(*v)) base.denylist.push_back(ns);
    }

    // Namespace list env: DBWALLER_NS_LIST=profile,timeline
    if (auto ns_list = getenv_str("DBWALLER_NS_LIST")) {
        for (const auto& ns : split_csv(*ns_list)) {
            // Ensure preset exists
            if (!base.presets.contains(ns)) {
                base.presets[ns] = base.default_rule;
            }

            Rule& r = base.presets[ns];

            const std::string scope_k = "DBWALLER_NS_" + ns + "_SCOPE";
            const std::string ttl_k   = "DBWALLER_NS_" + ns + "_TTL_MS";
            const std::string swr_k   = "DBWALLER_NS_" + ns + "_SWR_MS";
            const std::string stale_k = "DBWALLER_NS_" + ns + "_STALE_TTL_MS";
            const std::string vary_k  = "DBWALLER_NS_" + ns + "_VARY_BY_CLAIMS";
            const std::string req_k   = "DBWALLER_NS_" + ns + "_REQUIRE_CLAIMS_FP";
            const std::string soe_k   = "DBWALLER_NS_" + ns + "_SERVE_STALE_ON_ERROR";

            if (auto sv = getenv_str(scope_k.c_str())) r.scope = parse_scope(*sv, r.scope);
            if (auto tv = getenv_str(ttl_k.c_str()))   r.ttl_ms = parse_u64(*tv, r.ttl_ms);
            if (auto vv = getenv_str(swr_k.c_str()))   r.swr_ms = parse_u64(*vv, r.swr_ms);
            if (auto st = getenv_str(stale_k.c_str())) r.stale_ttl_ms = parse_u64(*st, r.stale_ttl_ms);
            if (auto vb = getenv_str(vary_k.c_str()))  r.vary_by_claims = parse_bool(*vb, r.vary_by_claims);
            if (auto rq = getenv_str(req_k.c_str()))   r.require_claims_fingerprint = parse_bool(*rq, r.require_claims_fingerprint);
            if (auto so = getenv_str(soe_k.c_str()))   r.serve_stale_on_error = parse_bool(*so, r.serve_stale_on_error);
        }
    }

    dedup_vector(base.denylist);
}

// public API (matches header)
NamespacePolicyConfig apply_env_overrides(NamespacePolicyConfig base) {
    apply_env_overrides_inplace(base);
    return base;
}

NamespacePolicyConfig load_policy_from_env_or_fallback(NamespacePolicyConfig fallback) {
    // 1) Optional JSON file
    if (auto json_path = getenv_str("DBWALLER_POLICY_JSON")) {
        fallback = load_policy_json_file(*json_path, std::move(fallback));
    }

    // 2) Env overrides win
    fallback = apply_env_overrides(std::move(fallback));
    return fallback;
}

} // namespace dbwaller::policy

