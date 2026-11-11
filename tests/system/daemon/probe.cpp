#include "dbwaller/v1/cache.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <fstream>
#include <filesystem>
#include <iostream>

namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("probe input missing");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    try {
        grpc::SslCredentialsOptions ssl; ssl.pem_root_certs = read(argv[2]);
        const auto token = read(argv[3]);
        auto client = dbwaller::v1::Cache::NewStub(grpc::CreateChannel(argv[1], grpc::SslCredentials(ssl)));
        const auto setup = [&](grpc::ClientContext& ctx) {
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
            ctx.AddMetadata("authorization", "Bearer " + token);
        };
        const auto ticket_path = std::filesystem::path(argv[3]).parent_path() / "old.ticket";
        const std::string mode = argv[4];
        if (mode == "seed") {
            for (bool publish : {true, false}) {
                grpc::ClientContext ctx; setup(ctx);
                dbwaller::v1::BeginLoadRequest req; req.set_cache_namespace("public"); req.set_key("probe");
                dbwaller::v1::LoadGrant load;
                if (!client->BeginLoad(&ctx, req, &load).ok()) return 1;
                if (!publish) { std::ofstream f(ticket_path, std::ios::binary); f << load.ticket(); continue; }
                grpc::ClientContext write; setup(write);
                dbwaller::v1::PublishRequest put; put.set_cache_namespace("public"); put.set_ticket(load.ticket());
                put.set_value("shared across processes"); put.mutable_options()->set_ttl_ms(60'000);
                dbwaller::v1::PublishResponse out;
                if (!client->Publish(&write, put, &out).ok() || out.outcome() != dbwaller::v1::PUBLICATION_STORED) return 1;
            }
        } else {
            grpc::ClientContext ctx; setup(ctx);
            dbwaller::v1::LookupRequest req; req.set_cache_namespace("public"); req.set_key("probe");
            dbwaller::v1::LookupResponse out;
            if (!client->Lookup(&ctx, req, &out).ok()) return 1;
            if (mode == "read") return out.kind() == dbwaller::v1::LOOKUP_KIND_FRESH && out.value() == "shared across processes" ? 0 : 1;
            if (mode != "cold" || out.kind() != dbwaller::v1::LOOKUP_KIND_MISS) return 1;
            grpc::ClientContext write; setup(write);
            dbwaller::v1::PublishRequest put; put.set_cache_namespace("public"); put.set_ticket(read(ticket_path));
            put.set_value("old"); put.mutable_options()->set_ttl_ms(60'000); dbwaller::v1::PublishResponse result;
            if (!client->Publish(&write, put, &result).ok() || result.outcome() != dbwaller::v1::PUBLICATION_UNKNOWN_TICKET) return 1;
        }
        return 0;
    } catch (...) { std::cerr << "daemon probe failed\n"; return 1; }
}
