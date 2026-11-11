#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>
#include "dbwaller/daemon/rpc_service.hpp"
#include "../../unit/concurrency/test_support.hpp"
#include <grpcpp/generic/generic_stub.h>

#include <cstdlib>
#include <fstream>

using namespace dbwaller;
using namespace dbwaller::daemon;
using namespace lifecycle_test;

namespace {
const std::string writer_token(64, 'a'), reader_token(64, 'b'), other_token(64, 'c');
std::filesystem::path fixtures() { return std::getenv("DBWALLER_TEST_DIR"); }
ServerConfig config(bool tls = false) {
    ServerConfig c;
    c.address = "127.0.0.1:0";
    c.insecure_loopback = !tls;
    if (tls) {
        c.certificate = read_bounded_file(fixtures() / "server.crt", 65536);
        c.private_key = read_bounded_file(fixtures() / "server.key", 65536);
    }
    c.credentials = {
        {"writer", token_digest(writer_token), {"public", "private"}, true, true, true},
        {"reader", token_digest(reader_token), {"public"}, false, false, false},
        {"other", token_digest(other_token), {"private"}, true, true, false}};
    c.shutdown_grace_ms = 1000;
    return c;
}
struct Harness {
    ServerConfig cfg;
    CacheService cache;
    concurrency::ThreadPool workers;
    RpcService rpc;
    int port = 0;
    std::unique_ptr<grpc::Server> server;
    explicit Harness(ServerConfig c) : cfg(std::move(c)), cache(cfg.cache, cfg.limits),
        workers(cfg.workers, cfg.queue_capacity), rpc(cache, workers, cfg), server(start_server(cfg, rpc, port)) {}
    ~Harness() { stop(); }
    void stop() {
        if (!server) return;
        rpc.close_admission();
        server->Shutdown(std::chrono::system_clock::now() + 1s);
        workers.shutdown();
        server->Wait(); server.reset(); cache.shutdown();
    }
    std::unique_ptr<v1::Cache::Stub> client(bool trust = true) const {
        std::shared_ptr<grpc::ChannelCredentials> creds;
        if (cfg.insecure_loopback) creds = grpc::InsecureChannelCredentials();
        else {
            grpc::SslCredentialsOptions ssl;
            if (trust) ssl.pem_root_certs = cfg.certificate;
            creds = grpc::SslCredentials(ssl);
        }
        grpc::ChannelArguments args;
        args.SetInt("grpc.enable_retries", 0);
        args.SetInt("grpc.use_local_subchannel_pool", 1); // Independent connections.
        return v1::Cache::NewStub(grpc::CreateCustomChannel("127.0.0.1:" + std::to_string(port), creds, args));
    }
};
void setup(grpc::ClientContext& ctx, const std::string& token = writer_token) {
    ctx.set_deadline(std::chrono::system_clock::now() + 10s);
    if (!token.empty()) ctx.AddMetadata("authorization", "Bearer " + token);
}
v1::LoadGrant begin(v1::Cache::Stub& client, const std::string& key, const std::string& ns = "public") {
    grpc::ClientContext ctx; setup(ctx);
    v1::BeginLoadRequest req; req.set_cache_namespace(ns); req.set_key(key);
    v1::LoadGrant out;
    REQUIRE(client.BeginLoad(&ctx, req, &out).ok());
    return out;
}
v1::Publication publish(v1::Cache::Stub& client, const v1::LoadGrant& load, const std::string& value, const std::string& ns = "public") {
    grpc::ClientContext ctx; setup(ctx);
    v1::PublishRequest req; req.set_cache_namespace(ns); req.set_ticket(load.ticket()); req.set_value(value);
    req.mutable_options()->set_ttl_ms(60'000); req.mutable_options()->add_tags("posts");
    v1::PublishResponse out;
    REQUIRE(client.Publish(&ctx, req, &out).ok());
    return out.outcome();
}
v1::LookupResponse lookup(v1::Cache::Stub& client, const std::string& key, bool ticket = false) {
    grpc::ClientContext ctx; setup(ctx);
    v1::LookupRequest req; req.set_cache_namespace("public"); req.set_key(key); req.set_acquire_ticket(ticket);
    v1::LookupResponse out;
    REQUIRE(client.Lookup(&ctx, req, &out).ok());
    return out;
}
} // namespace

TEST_CASE("two socket clients share values and fence old loads", "[daemon]") {
    const bool tls = GENERATE(false, true);
    const bool tag = GENERATE(false, true);
    Harness h(config(tls));
    auto a = h.client(), b = h.client();
    const std::string key("post\0key", 8), value("v2\0\xff", 4);
    auto old = begin(*a, key);
    grpc::ClientContext ctx; setup(ctx);
    v1::InvalidationResponse out;
    if (tag) {
        v1::InvalidateTagRequest req; req.set_cache_namespace("public"); req.set_tag("posts");
        REQUIRE(b->InvalidateTag(&ctx, req, &out).ok());
    } else {
        v1::InvalidateKeyRequest req; req.set_cache_namespace("public"); req.set_key(key);
        REQUIRE(b->InvalidateKey(&ctx, req, &out).ok());
    }
    CHECK(publish(*b, begin(*b, key), value) == v1::PUBLICATION_STORED);
    CHECK(publish(*a, old, "v1") == v1::PUBLICATION_INVALIDATED);
    const auto read = lookup(*a, key, true);
    CHECK(read.kind() == v1::LOOKUP_KIND_FRESH); CHECK(read.value() == value); REQUIRE(read.has_load());
    CHECK(publish(*a, read.load(), "refresh") == v1::PUBLICATION_STORED);
    CHECK(publish(*b, read.load(), "replay") == v1::PUBLICATION_CONSUMED);
    CHECK(lookup(*b, key).value() == "refresh");
}

TEST_CASE("socket credentials enforce namespaces roles and operator access", "[daemon]") {
    Harness h(config()); auto c = h.client();
    for (const auto& token : {std::string{}, std::string(64, 'x')}) {
        grpc::ClientContext ctx; setup(ctx, token);
        v1::LookupRequest req; req.set_cache_namespace("public"); req.set_key("post"); v1::LookupResponse out;
        CHECK(c->Lookup(&ctx, req, &out).error_code() == grpc::StatusCode::UNAUTHENTICATED);
    }
    {
        grpc::ClientContext ctx; setup(ctx); ctx.AddMetadata("authorization", "Bearer " + writer_token);
        v1::HealthRequest req; v1::HealthResponse out;
        CHECK(c->Health(&ctx, req, &out).error_code() == grpc::StatusCode::UNAUTHENTICATED);
    }
    {
        grpc::ClientContext ctx; setup(ctx, reader_token);
        v1::LookupRequest req; req.set_cache_namespace("private"); req.set_key("post"); v1::LookupResponse out;
        CHECK(c->Lookup(&ctx, req, &out).error_code() == grpc::StatusCode::PERMISSION_DENIED);
    }
    {
        grpc::ClientContext ctx; setup(ctx, reader_token);
        v1::BeginLoadRequest req; req.set_cache_namespace("public"); req.set_key("post"); v1::LoadGrant out;
        CHECK(c->BeginLoad(&ctx, req, &out).error_code() == grpc::StatusCode::PERMISSION_DENIED);
    }
    {
        grpc::ClientContext ctx; setup(ctx, reader_token);
        v1::InvalidateTagRequest req; req.set_cache_namespace("public"); req.set_tag("posts"); v1::InvalidationResponse out;
        CHECK(c->InvalidateTag(&ctx, req, &out).error_code() == grpc::StatusCode::PERMISSION_DENIED);
    }
    {
        grpc::ClientContext ctx; setup(ctx, reader_token);
        v1::StatsRequest req; v1::StatsResponse out;
        CHECK(c->Stats(&ctx, req, &out).error_code() == grpc::StatusCode::PERMISSION_DENIED);
    }
    {
        grpc::ClientContext ctx; setup(ctx);
        v1::HealthRequest req; v1::HealthResponse out;
        REQUIRE(c->Health(&ctx, req, &out).ok()); CHECK(out.ready()); CHECK(out.protocol_version() == "dbwaller.v1");
        CHECK_FALSE(out.build_version().empty()); CHECK_FALSE(out.instance_id().empty());
    }
    const auto load = begin(*c, "post", "private");
    grpc::ClientContext ctx; setup(ctx, other_token);
    v1::PublishRequest req; req.set_cache_namespace("private"); req.set_ticket(load.ticket()); req.set_value("wrong owner");
    req.mutable_options()->set_ttl_ms(1000); v1::PublishResponse out;
    CHECK(c->Publish(&ctx, req, &out).error_code() == grpc::StatusCode::PERMISSION_DENIED);
    CHECK(publish(*c, load, "right owner", "private") == v1::PUBLICATION_STORED);
}

TEST_CASE("socket rejects bad input oversized messages and missing deadlines", "[daemon]") {
    auto cfg = config(); cfg.max_message_bytes = 4096; cfg.limits.max_value_bytes = 1024;
    Harness h(cfg); auto c = h.client();
    {
        grpc::ClientContext ctx; setup(ctx);
        v1::BeginLoadRequest req; req.set_cache_namespace("public"); v1::LoadGrant out;
        CHECK(c->BeginLoad(&ctx, req, &out).error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    }
    const auto load = begin(*c, "post");
    for (size_t bytes : {0u, 1025u, 8192u}) {
        grpc::ClientContext ctx; setup(ctx);
        v1::PublishRequest req; req.set_cache_namespace("public"); req.set_ticket(load.ticket()); req.set_value(std::string(bytes, 'v'));
        if (bytes) req.mutable_options()->set_ttl_ms(1000);
        v1::PublishResponse out;
        const auto status = c->Publish(&ctx, req, &out);
        CHECK(status.error_code() == (bytes > 4096 ? grpc::StatusCode::RESOURCE_EXHAUSTED : grpc::StatusCode::INVALID_ARGUMENT));
    }
    {
        grpc::ClientContext ctx; ctx.AddMetadata("authorization", "Bearer " + writer_token);
        v1::HealthRequest req; v1::HealthResponse out;
        CHECK(c->Health(&ctx, req, &out).error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    }
    CHECK(publish(*c, load, "valid") == v1::PUBLICATION_STORED);
}

TEST_CASE("cancelled queued RPCs retain capacity until work is retired", "[daemon][concurrency]") {
    const bool active_limit = GENERATE(false, true);
    auto cfg = config(); cfg.workers = 1; cfg.queue_capacity = 1; cfg.max_active_rpcs = active_limit ? 1 : 8;
    Gate gate; Harness h(cfg); auto c = h.client();
    REQUIRE(h.workers.try_submit([&](std::stop_token) { gate.block(); }));
    grpc::ClientContext pending_ctx; setup(pending_ctx);
    v1::BeginLoadRequest req; req.set_cache_namespace("public"); req.set_key("queued"); v1::LoadGrant out;
    auto pending = std::async(std::launch::async, [&] { return c->BeginLoad(&pending_ctx, req, &out); });
    ReleaseOnExit release{gate.release};
    REQUIRE(gate.entered.wait());
    REQUIRE(eventually([&] { return h.rpc.metrics().accepted == 1; }));
    grpc::ClientContext overflow_ctx; setup(overflow_ctx); v1::LoadGrant overflow;
    CHECK(c->BeginLoad(&overflow_ctx, req, &overflow).error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED);
    pending_ctx.TryCancel();
    REQUIRE(ready(pending)); CHECK(pending.get().error_code() == grpc::StatusCode::CANCELLED);
    // Local cancellation completion is not a delivery acknowledgment. Wait for
    // the server's OnCancel before releasing work that must observe cancellation.
    REQUIRE(eventually([&] { return h.rpc.metrics().cancelled == 1; }));
    gate.release.set();
    REQUIRE(eventually([&] { return h.rpc.metrics().active == 0; }));
    CHECK(h.cache.stats().ticket_records == 0); // No queued mutation after cancellation.
    CHECK_FALSE(begin(*c, "after-cancel").ticket().empty());
}

TEST_CASE("shutdown drains accepted RPC work and closes admission", "[daemon][concurrency]") {
    auto cfg = config(); cfg.workers = 1;
    Gate gate; Harness h(cfg); auto c = h.client();
    REQUIRE(h.workers.try_submit([&](std::stop_token) { gate.block(); }));
    grpc::ClientContext ctx; setup(ctx);
    v1::BeginLoadRequest req; req.set_cache_namespace("public"); req.set_key("queued"); v1::LoadGrant out;
    auto pending = std::async(std::launch::async, [&] { return c->BeginLoad(&ctx, req, &out); });
    ReleaseOnExit release{gate.release};
    REQUIRE(gate.entered.wait());
    REQUIRE(eventually([&] { return h.rpc.metrics().accepted == 1; }));
    auto closing = std::async(std::launch::async, [&] { h.stop(); });
    REQUIRE(eventually([&] { return !h.rpc.accepting(); }));
    gate.release.set();
    REQUIRE(ready(pending)); const auto result = pending.get();
    CHECK((result.ok() || result.error_code() == grpc::StatusCode::CANCELLED || result.error_code() == grpc::StatusCode::UNAVAILABLE));
    REQUIRE(ready(closing)); closing.get();
    CHECK_FALSE(h.cache.stats().ready); CHECK(h.rpc.metrics().active == 0);
    grpc::ClientContext stopped; setup(stopped); v1::HealthRequest health; v1::HealthResponse response;
    CHECK(c->Health(&stopped, health, &response).error_code() == grpc::StatusCode::UNAVAILABLE);
}

TEST_CASE("TLS clients must trust the server certificate", "[daemon][tls]") {
    Harness h(config(true)); auto bad = h.client(false);
    grpc::ClientContext ctx; setup(ctx); v1::HealthRequest req; v1::HealthResponse out;
    CHECK(bad->Health(&ctx, req, &out).error_code() == grpc::StatusCode::UNAVAILABLE);
    auto good = h.client(); grpc::ClientContext ok; setup(ok);
    REQUIRE(good->Health(&ok, req, &out).ok()); CHECK(out.ready());
}

TEST_CASE("connection admission is bounded and malformed protobuf cannot mutate the cache", "[daemon]") {
    SECTION("one connection") {
        auto cfg = config(); cfg.max_connections = 1;
        Harness h(cfg); auto first = h.client();
        CHECK(lookup(*first, "post").kind() == v1::LOOKUP_KIND_MISS);
        auto second = h.client(); grpc::ClientContext ctx; setup(ctx);
        v1::HealthRequest req; v1::HealthResponse out;
        CHECK(second->Health(&ctx, req, &out).error_code() == grpc::StatusCode::UNAVAILABLE);
        CHECK(lookup(*first, "post").kind() == v1::LOOKUP_KIND_MISS);
    }
    SECTION("malformed payload") {
        Harness h(config());
        grpc::GenericStub client(grpc::CreateChannel("127.0.0.1:" + std::to_string(h.port), grpc::InsecureChannelCredentials()));
        grpc::ClientContext ctx; setup(ctx);
        grpc::Slice slice("\x0a\xff\xff", 3);
        grpc::ByteBuffer req(&slice, 1), out;
        grpc::CompletionQueue queue;
        grpc::Status status;
        auto call = client.PrepareUnaryCall(&ctx, "/dbwaller.v1.Cache/Publish", req, &queue);
        call->StartCall(); call->Finish(&out, &status, reinterpret_cast<void*>(1));
        void* tag = nullptr; bool ok = false;
        REQUIRE(queue.Next(&tag, &ok));
        CHECK(tag == reinterpret_cast<void*>(1)); CHECK_FALSE(status.ok());
        queue.Shutdown(); while (queue.Next(&tag, &ok)) {}
        CHECK(h.cache.stats().engine.puts == 0);
        auto normal = h.client(); CHECK_FALSE(begin(*normal, "healthy").ticket().empty());
    }
}

TEST_CASE("daemon configuration fails closed for unsafe or mistyped settings", "[daemon][configuration]") {
    auto cfg = config();
    CHECK_NOTHROW(validate_server_config(cfg));
    cfg.address = "0.0.0.0:7447"; CHECK_THROWS(validate_server_config(cfg));
    cfg = config(); cfg.address = "localhost:7447"; CHECK_THROWS(validate_server_config(cfg));
    cfg = config(); cfg.address = "127.0.0.1:-1"; CHECK_THROWS(validate_server_config(cfg));
    cfg = config(); cfg.queue_capacity = 0; CHECK_THROWS(validate_server_config(cfg));
    cfg = config(); cfg.cache.max_entries_total = 0; CHECK_THROWS(validate_server_config(cfg));
    cfg = config(); cfg.credentials.clear(); CHECK_THROWS(validate_server_config(cfg));
    cfg = config(); cfg.credentials.push_back(cfg.credentials.front()); CHECK_THROWS(validate_server_config(cfg));
    cfg = config(true); cfg.private_key = "invalid"; CHECK_THROWS(validate_server_config(cfg));
    auto original = nlohmann::json::parse(read_bounded_file(fixtures() / "daemon.json", 65536));
    for (const auto& bad_value : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json("4"), nlohmann::json(0)}) {
        auto value = original; value["rpc"]["workers"] = bad_value;
        const auto file = fixtures() / "invalid.json";
        { std::ofstream output(file); output << value.dump(); }
        CHECK_THROWS(load_server_config(file));
    }
    original["insecure_loopbak"] = true;
    const auto file = fixtures() / "invalid.json";
    { std::ofstream output(file); output << original.dump(); }
    CHECK_THROWS(load_server_config(file));
    CHECK_NOTHROW(load_server_config(fixtures() / "daemon.json"));
}
