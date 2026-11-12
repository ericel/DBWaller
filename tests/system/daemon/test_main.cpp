#include <catch2/catch_session.hpp>
#include <openssl/ssl.h>

int main(int argc, char** argv) {
    // Match dbwallerd's explicit process policy, before tests hash credentials.
    if (OPENSSL_init_ssl(OPENSSL_INIT_NO_ATEXIT, nullptr) != 1) return 1;
    return Catch::Session().run(argc, argv);
}
