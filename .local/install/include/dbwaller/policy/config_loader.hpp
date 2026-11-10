#pragma once

#include "dbwaller/policy/presets.hpp"

namespace dbwaller::policy {

// Env override helper (functional style)
NamespacePolicyConfig apply_env_overrides(NamespacePolicyConfig base);

// Main loader: optional JSON then env overrides (env wins)
NamespacePolicyConfig load_policy_from_env_or_fallback(NamespacePolicyConfig fallback);

} // namespace dbwaller::policy




// #pragma once

// #include "dbwaller/policy/presets.hpp"

// #include <optional>
// #include <string>

// namespace dbwaller::policy {

// /**
//  * Load NamespacePolicyConfig from JSON file.
//  * Throws std::runtime_error on parse/IO errors.
//  */
// NamespacePolicyConfig load_policy_from_json_file(const std::string& path);

// /**
//  * Apply env overrides on top of an existing config.
//  *
//  * Supported env vars:
//  * - DBWALLER_DENYLIST=session,payments
//  * - DBWALLER_NS_LIST=post,timeline,adminview   (optional; lets env define namespaces not in JSON)
//  *
//  * Per-namespace overrides (ns is literal token, e.g. "post"):
//  * - DBWALLER_NS_<ns>_SCOPE=Public|PerViewer|PerTenant|NoStore
//  * - DBWALLER_NS_<ns>_TTL_MS=500
//  * - DBWALLER_NS_<ns>_MIN_TTL_MS=50
//  * - DBWALLER_NS_<ns>_MAX_TTL_MS=30000
//  * - DBWALLER_NS_<ns>_VARY_BY_CLAIMS=0|1|true|false
//  * - DBWALLER_NS_<ns>_REQUIRE_CLAIMS=0|1|true|false
//  *
//  * Default overrides:
//  * - DBWALLER_DEFAULT_SCOPE=PerViewer|Public|...
//  * - DBWALLER_DEFAULT_TTL_MS=1500
//  * - DBWALLER_DEFAULT_MIN_TTL_MS=50
//  * - DBWALLER_DEFAULT_MAX_TTL_MS=30000
//  * - DBWALLER_DEFAULT_VARY_BY_CLAIMS=0|1
//  * - DBWALLER_DEFAULT_REQUIRE_CLAIMS=0|1
//  */
// NamespacePolicyConfig apply_env_overrides(NamespacePolicyConfig base);

// /**
//  * Convenience: build config using env:
//  * - If DBWALLER_POLICY_JSON is set, load it
//  * - Else use provided fallback
//  * - Then apply env overrides
//  */
// NamespacePolicyConfig load_policy_from_env_or_fallback(NamespacePolicyConfig fallback);

// } // namespace dbwaller::policy
