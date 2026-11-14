#include <dbwaller/remote/client.hpp>
#include <fstream>
#include <iostream>

namespace r = dbwaller::remote;
namespace {
template<class T> bool succeeded(const r::Result<T>& result, const char* operation) {
    if (result) return true;
    std::cerr << operation << " failed with SDK error code " << static_cast<int>(result.error().code) << '\n';
    return false;
}
std::string file(const char* path, size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unreadable file");
    std::string value(limit + 1, '\0');
    input.read(value.data(), value.size());
    if (input.bad() || static_cast<size_t>(input.gcount()) > limit)
        throw std::runtime_error("invalid file");
    value.resize(input.gcount());
    return value;
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 6) {
            std::cerr << "Usage: dbwaller-probe health|seed|read|cold ENDPOINT CA_FILE TOKEN_FILE NAMESPACE\n";
            return 2;
        }
        r::initialize_process_tls();
        r::ClientOptions options;
        options.endpoint = argv[2]; options.root_certificates = file(argv[3], 65536);
        options.bearer_token = file(argv[4], 1024); options.cache_namespace = argv[5];
        if (!options.bearer_token.empty() && options.bearer_token.back() == '\n') options.bearer_token.pop_back();
        options.timeout = std::chrono::seconds(2);
        r::Client client(std::move(options));
        const std::string mode = argv[1];
        if (mode == "health") {
            const auto result = client.health().get();
            if (!succeeded(result, "health") || !result.value().ready) return 1;
        } else if (mode == "seed") {
            const auto load = client.begin_load("container-restart-probe").get();
            if (!succeeded(load, "begin_load")) return 1;
            const auto stored = client.publish(load.value().ticket, "ephemeral", {std::chrono::minutes(5), {}, {}}).get();
            if (!succeeded(stored, "publish") || stored.value() != r::PublishStatus::Stored) return 1;
        } else if (mode == "read" || mode == "cold") {
            const auto result = client.lookup("container-restart-probe").get();
            if (!succeeded(result, "lookup")) return 1;
            if (mode == "read" && (result.value().kind != r::LookupKind::Fresh || result.value().value != "ephemeral")) return 1;
            if (mode == "cold" && result.value().kind != r::LookupKind::Miss) return 1;
        } else return 2;
        std::cout << "probe succeeded\n";
        return 0;
    } catch (...) {
        // Do not echo credentials, cached values, or transport diagnostics.
        std::cerr << "probe failed\n";
        return 1;
    }
}
