#include <dbwaller/remote/client.hpp>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <cstdio>
#include <cstring>

namespace {
const char* marker = nullptr; // argv storage remains valid through exit handlers.
void cleanup_observed() {
    if (auto* file = std::fopen(marker, "w")) {
        std::fputs("OpenSSL global cleanup ran\n", file);
        std::fclose(file);
    }
}
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    marker = argv[2];
    const bool early = std::strcmp(argv[1], "early") == 0;
    const bool late = std::strcmp(argv[1], "late") == 0;
    if (!early && !late && std::strcmp(argv[1], "baseline") != 0) return 2;
    if (early) dbwaller::remote::initialize_process_tls();
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (!SHA256(reinterpret_cast<const unsigned char*>("probe"), 5, digest)) return 3;
    if (early || late) dbwaller::remote::initialize_process_tls();
    if (OPENSSL_atexit(cleanup_observed) != 1) return 4;
    // Exit normally: the parent checks whether the registered cleanup ran.
}
