#pragma once

#include <string>
#include <string_view>

namespace dbwaller::security {

// SHA-256 digest as lowercase hex string.
std::string sha256_hex(std::string_view data);

} // namespace dbwaller::security
