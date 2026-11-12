#include "cache_aside.hpp"
#include <fstream>
#include <iostream>
#include <map>

namespace {
std::string file(const std::string& path, size_t max = 65536) {
    std::ifstream in(path, std::ios::binary); if (!in) throw std::runtime_error("cannot open configured file");
    std::string value(max + 1, '\0'); in.read(value.data(), value.size());
    if (in.bad() || static_cast<size_t>(in.gcount()) > max) throw std::runtime_error("configured file too large");
    value.resize(in.gcount()); return value;
}
template<class Store> int demo(Store& store, bool disabled) {
    unsigned reads = 0;
    std::string durable_value = "version 1";
    auto backend = [&](auto) { ++reads; return durable_value; };
    const std::string key = "public-demo-post";
    store.invalidate(key);
    const auto first = example::get(store, key, backend);
    const auto second = example::get(store, key, backend);
    if (first != "version 1" || second != "version 1") return 1;
    std::cout << "Two reads completed; backend fetches: " << reads << '\n';
    if (disabled && reads != 2) return 1;
    durable_value = "version 2"; // Authoritative write precedes invalidation.
    const bool invalidated = store.invalidate(key);
    if (!invalidated) std::cout << "Invalidation unavailable; application must retry/reconcile it before trusting cache hits.\n";
    // On invalidation failure this demo bypasses the cache for the changed value.
    const auto updated = invalidated ? example::get(store, key, backend) : backend(example::r::Clock::now());
    if (updated != "version 2") return 1;
    std::cout << "Updated value read successfully; durable backend remains authoritative.\n";
    return 0;
}
}
int main(int argc, char** argv) {
    try {
        std::map<std::string, std::string> args;
        for (int i = 1; i < argc; ++i) {
            std::string name = argv[i];
            if (name == "--help") {
                std::cout << "--mode disabled|remote|embedded [--endpoint HOST:PORT --namespace NAME --ca FILE --token-file FILE]\n";
                return 0;
            }
            if (name != "--mode" && name != "--endpoint" && name != "--namespace" && name != "--ca" && name != "--token-file")
                throw std::invalid_argument("unknown option");
            if (++i == argc || !args.emplace(name, argv[i]).second) throw std::invalid_argument("invalid option");
        }
        const auto mode = args.contains("--mode") ? args.at("--mode") : "disabled";
        if (mode == "disabled") { example::DisabledStore store; return demo(store, true); }
#ifdef DBWALLER_EXAMPLE_EMBEDDED
        if (mode == "embedded") { example::EmbeddedStore store; return demo(store, false); }
#endif
        if (mode != "remote") throw std::invalid_argument("mode unavailable in this build");
        example::r::ClientOptions options;
        options.endpoint = args.at("--endpoint"); options.cache_namespace = args.at("--namespace");
        options.root_certificates = file(args.at("--ca")); options.bearer_token = file(args.at("--token-file"), 1024);
        if (!options.bearer_token.empty() && options.bearer_token.back() == '\n') options.bearer_token.pop_back();
        example::RemoteStore store(std::move(options)); return demo(store, false);
    } catch (...) { std::cerr << "Example failed; check configuration, authorization, and the application budget.\n"; return 1; }
}
