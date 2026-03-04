#include "dbwaller/security/crypto.hpp"

#include <openssl/evp.h>
#include <array>
#include <stdexcept>

namespace dbwaller::security {

static inline char hex_nibble(unsigned v) {
    static const char* kHex = "0123456789abcdef";
    return kHex[v & 0xF];
}

std::string sha256_hex(std::string_view data) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;

    const EVP_MD* md = EVP_sha256();
    if (!md) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_sha256 failed");
    }

    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestInit_ex failed");
    }

    if (!data.empty()) {
        if (EVP_DigestUpdate(ctx, data.data(), data.size()) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("EVP_DigestUpdate failed");
        }
    }

    if (EVP_DigestFinal_ex(ctx, digest.data(), &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestFinal_ex failed");
    }

    EVP_MD_CTX_free(ctx);

    std::string out;
    out.resize(digest_len * 2);
    for (unsigned i = 0; i < digest_len; ++i) {
        out[i * 2 + 0] = hex_nibble(digest[i] >> 4);
        out[i * 2 + 1] = hex_nibble(digest[i] & 0x0F);
    }
    return out;
}

} // namespace dbwaller::security
