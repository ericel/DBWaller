#include <dbwaller/remote/client.hpp>
#include <fstream>
#include <iostream>

namespace r = dbwaller::remote;
namespace {
std::string read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("fixture unavailable");
    return {std::istreambuf_iterator<char>(file), {}};
}
}
int main(int argc, char** argv) {
    try {
        r::initialize_process_tls();
        if (argc != 5) return 2;
        r::ClientOptions o; o.endpoint = argv[1]; o.root_certificates = read(argv[2]);
        o.bearer_token = read(argv[3]); o.cache_namespace = "public"; o.timeout = std::chrono::seconds(5);
        r::Client client(std::move(o));
        const std::string mode = argv[4];
        if (mode == "seed") {
            auto grant = client.begin_load("process-shared").get(); if (!grant) return 1;
            auto stored = client.publish(grant.value().ticket, "shared across processes", {std::chrono::seconds(60), {}, {}}).get();
            if (!stored || stored.value() != r::PublishStatus::Stored) return 1;
        } else {
            auto result = client.lookup("process-shared").get(); if (!result) return 1;
            if (mode == "read" && (result.value().kind != r::LookupKind::Fresh || result.value().value != "shared across processes")) return 1;
            if (mode == "cold" && result.value().kind != r::LookupKind::Miss) return 1;
            if (mode != "read" && mode != "cold") return 2;
        }
        client.shutdown(); std::cout << "Public SDK process check passed\n"; return 0;
    } catch (...) { return 1; }
}
