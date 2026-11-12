#include "dbwaller/daemon/rpc_service.hpp"
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <openssl/ssl.h>

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: dbwallerd --config FILE [--check-config]\n";
        return 0;
    }
    if ((argc != 3 && argc != 4) || std::string_view(argv[1]) != "--config" ||
        (argc == 4 && std::string_view(argv[3]) != "--check-config")) {
        std::cerr << "Usage: dbwallerd --config FILE [--check-config]\n";
        return 2;
    }
    try {
        // This executable owns process startup. Configure cleanup before config
        // validation/hashing or any threads can initialize OpenSSL first.
        if (OPENSSL_init_ssl(OPENSSL_INIT_NO_ATEXIT, nullptr) != 1)
            throw std::runtime_error("TLS process initialization failed");
        const auto config = dbwaller::daemon::load_server_config(argv[2]);
        if (argc == 4) { std::cout << "configuration valid\n"; return 0; }
        sigset_t signals;
        sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
        // Block before creating any engine/gRPC threads; no unsafe signal handler.
        if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) throw std::runtime_error("signal setup failed");
        dbwaller::daemon::Daemon daemon(config);
        const auto host = config.address.substr(0, config.address.rfind(':'));
        std::cout << "dbwallerd ready on " << host << ':' << daemon.port() << std::endl;
        int signal = 0;
        if (sigwait(&signals, &signal) != 0) throw std::runtime_error("signal wait failed");
        daemon.shutdown();
        std::cout << "dbwallerd stopped\n";
        return 0;
    } catch (...) {
        std::cerr << "dbwallerd failed: check configuration, credentials, TLS files, and listen address\n";
        return 1;
    }
}
