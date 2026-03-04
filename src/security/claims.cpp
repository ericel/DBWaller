#include "dbwaller/security/claims.hpp"
#include "dbwaller/security/crypto.hpp"

#include <algorithm>
#include <sstream>

namespace dbwaller::security {

std::vector<std::string> normalize_list(std::vector<std::string> items) {
    // Remove empty
    items.erase(std::remove_if(items.begin(), items.end(),
                              [](const std::string& s) { return s.empty(); }),
                items.end());

    // Sort + unique
    std::sort(items.begin(), items.end());
    items.erase(std::unique(items.begin(), items.end()), items.end());
    return items;
}

static std::string join_csv(const std::vector<std::string>& items) {
    std::ostringstream oss;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) oss << ",";
        oss << items[i];
    }
    return oss.str();
}

std::string normalize_claims_string(
    const std::vector<std::string>& roles,
    const std::vector<std::string>& scopes,
    const std::map<std::string, std::string>& extra_kv
) {
    auto r = normalize_list(roles);
    auto s = normalize_list(scopes);

    // Build a deterministic string. Keep it simple and stable.
    // NOTE: Avoid putting any raw personal claims here. Only include what you *intend* to vary by.
    std::ostringstream oss;
    oss << "roles=" << join_csv(r) << ";";
    oss << "scopes=" << join_csv(s);

    // extra_kv is already sorted by key because it's std::map
    for (const auto& [k, v] : extra_kv) {
        if (k.empty() || v.empty()) continue;
        oss << ";kv." << k << "=" << v;
    }

    return oss.str();
}

std::string claims_fingerprint_hex(
    const std::vector<std::string>& roles,
    const std::vector<std::string>& scopes,
    const std::map<std::string, std::string>& extra_kv,
    size_t truncated_hex_chars
) {
    const std::string normalized = normalize_claims_string(roles, scopes, extra_kv);
    std::string hex = sha256_hex(normalized);

    if (truncated_hex_chars == 0) return hex;
    if (truncated_hex_chars >= hex.size()) return hex;
    return hex.substr(0, truncated_hex_chars);
}

} // namespace dbwaller::security
