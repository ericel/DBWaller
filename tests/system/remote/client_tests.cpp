#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "dbwaller/remote/client.hpp"
#include "dbwaller/daemon/rpc_service.hpp"
#include "../../unit/concurrency/test_support.hpp"
#include "../../../examples/remote_cpp/cache_aside.hpp"

#include <cstdlib>
#include <deque>
#include <limits>

using namespace dbwaller;
using namespace lifecycle_test;
namespace r = dbwaller::remote;
namespace {
const std::string token(64, 'a'), reader_token(64, 'b'), other_token(64, 'c');
std::filesystem::path fixtures() { return std::getenv("DBWALLER_TEST_DIR"); }
daemon::ServerConfig server_config(bool tls = true) {
    daemon::ServerConfig cfg;
    cfg.address = "127.0.0.1:0"; cfg.insecure_loopback = !tls; cfg.workers = 1;
    if (tls) {
        cfg.certificate = daemon::read_bounded_file(fixtures() / "server.crt", 65536);
        cfg.private_key = daemon::read_bounded_file(fixtures() / "server.key", 65536);
    }
    cfg.credentials = {{"writer", daemon::token_digest(token), {"public"}, true, true, true},
                       {"reader", daemon::token_digest(reader_token), {"public"}, false, false, false},
                       {"other", daemon::token_digest(other_token), {"public"}, true, true, false}};
    return cfg;
}
struct Server {
    daemon::ServerConfig cfg;
    daemon::CacheService cache;
    concurrency::ThreadPool workers;
    daemon::RpcService rpc;
    int port = 0;
    std::unique_ptr<grpc::Server> server;
    explicit Server(bool tls = true) : cfg(server_config(tls)), cache(cfg.cache, cfg.limits),
        workers(cfg.workers, cfg.queue_capacity), rpc(cache, workers, cfg), server(daemon::start_server(cfg, rpc, port)) {}
    ~Server() { stop(); }
    void stop() {
        if (!server) return;
        rpc.close_admission(); server->Shutdown(std::chrono::system_clock::now() + 1s);
        workers.shutdown(); server->Wait(); server.reset(); cache.shutdown();
    }
    r::ClientOptions client_options() const {
        r::ClientOptions o;
        o.endpoint = "127.0.0.1:" + std::to_string(port); o.cache_namespace = "public"; o.bearer_token = token;
        o.insecure_loopback = cfg.insecure_loopback; o.root_certificates = cfg.certificate; o.timeout = 5s;
        return o;
    }
};
template<class T> r::Result<T> await(r::Request<T> request) {
    auto future = request.future(); REQUIRE(future.wait_for(watchdog) == std::future_status::ready); return future.get();
}
template<class T> T success(r::Request<T> request) {
    auto result = await(std::move(request));
    if (!result) INFO(result.error().message);
    REQUIRE(result.has_value()); return result.value();
}
template<class T> void failure(r::Request<T> request, r::ErrorCode code, bool unknown = false) {
    auto result = await(std::move(request)); REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == code); CHECK(result.error().outcome_unknown == unknown);
}
r::PublishOptions publish_options() { return {60s, 0ms, {"posts"}}; }

// An application-owned event queue. Posting never runs user code inline.
struct Events {
    std::mutex mu;
    std::deque<std::function<void()>> tasks;
    r::Executor executor() { return [this](auto task) { std::lock_guard lock(mu); tasks.push_back(std::move(task)); }; }
    bool pending() { std::lock_guard lock(mu); return !tasks.empty(); }
    void run_one() {
        std::function<void()> task;
        { std::lock_guard lock(mu); REQUIRE_FALSE(tasks.empty()); task = std::move(tasks.front()); tasks.pop_front(); }
        task();
    }
};
}

TEST_CASE("public client shares binary values and fences old loads over TLS", "[remote]") {
    const bool by_tag = GENERATE(false, true);
    Server server; r::Client a(server.client_options()), b(server.client_options());
    const std::string key("post\0binary", 11), value("value\0\xff", 7);
    auto old = success(a.lookup(key, true)); REQUIRE(old.kind == r::LookupKind::Miss); REQUIRE(old.load);
    if (by_tag) success(b.invalidate_tag("posts")); else success(b.invalidate_key(key));
    auto newer = success(b.begin_load(key));
    CHECK(success(b.publish(newer.ticket, value, publish_options())) == r::PublishStatus::Stored);
    CHECK(success(a.publish(old.load->ticket, "old", publish_options())) == r::PublishStatus::Invalidated);
    CHECK(success(b.publish(newer.ticket, "replay", publish_options())) == r::PublishStatus::Consumed);
    auto read = success(a.lookup(key)); REQUIRE(read.kind == r::LookupKind::Fresh); CHECK(read.value == value);
    CHECK(read.fresh_for > 0ms); CHECK(read.fresh_for <= read.servable_for); CHECK(read.servable_for <= 60s);
    CHECK_FALSE(read.load);
    auto empty = success(a.begin_load("empty")); success(a.publish(empty.ticket, "", publish_options()));
    CHECK(success(b.lookup("empty")).kind == r::LookupKind::Fresh);
    CHECK(success(b.lookup("empty")).value.empty());
    auto stale = success(a.begin_load("stale")); success(a.publish(stale.ticket, "stale value", {0ms, 60s, {}}));
    auto snapshot = success(b.lookup("stale", true)); CHECK(snapshot.kind == r::LookupKind::Stale);
    CHECK(snapshot.fresh_for == 0ms); REQUIRE(snapshot.load); CHECK(snapshot.load->ticket.valid());
    CHECK(success(a.health()).ready); CHECK(success(a.health()).protocol_version == "dbwaller.v1");
    CHECK(success(a.stats()).puts >= 3);
}

TEST_CASE("client enforces credential boundaries and rejects incompatible local tickets", "[remote]") {
    Server server; auto options = server.client_options(); r::Client owner(options);
    auto ticket = success(owner.begin_load("post")).ticket;
    options.bearer_token = other_token; r::Client other(options);
    failure(other.publish(ticket, "wrong owner", publish_options()), r::ErrorCode::PermissionDenied, true);
    options.bearer_token = reader_token; r::Client reader(options);
    success(reader.lookup("post")); failure(reader.begin_load("post"), r::ErrorCode::PermissionDenied);
    failure(reader.health(), r::ErrorCode::PermissionDenied);
    options.bearer_token = std::string(64, 'x'); r::Client invalid(options);
    failure(invalid.lookup("post"), r::ErrorCode::Unauthenticated);
    options = server.client_options(); options.cache_namespace = "private"; r::Client other_namespace(options);
    failure(other_namespace.lookup("post"), r::ErrorCode::PermissionDenied);
    failure(other_namespace.publish(ticket, "wrong namespace", publish_options()), r::ErrorCode::InvalidArgument);
    failure(owner.publish({}, "no ticket", publish_options()), r::ErrorCode::InvalidArgument);
    CHECK(success(owner.publish(ticket, "allowed", publish_options())) == r::PublishStatus::Stored);
    options = server.client_options(); options.root_certificates.clear(); r::Client untrusted(options);
    failure(untrusted.lookup("post"), r::ErrorCode::Unavailable);
}

TEST_CASE("bounded client admission and cancellation preserve independent requests", "[remote][concurrency]") {
    Gate gate; Server server(false); auto options = server.client_options(); options.max_in_flight = 1;
    r::Client a(options), b(options); success(a.health()); success(b.health());
    const auto before = server.rpc.metrics().accepted;
    REQUIRE(server.workers.try_submit([&](std::stop_token) { gate.block(); }));
    ReleaseOnExit release{gate.release}; REQUIRE(gate.entered.wait());
    auto first = a.lookup("queued"), independent = b.lookup("other");
    REQUIRE(eventually([&] { return server.rpc.metrics().accepted == before + 2; }));
    CHECK(a.in_flight() == 1);
    failure(a.lookup("over-capacity"), r::ErrorCode::ResourceExhausted);
    first.cancel(); failure(first, r::ErrorCode::Cancelled);
    CHECK(a.in_flight() == 0);
    gate.release.set(); CHECK(success(independent).kind == r::LookupKind::Miss);
    CHECK(success(a.lookup("recovered")).kind == r::LookupKind::Miss);
    first.cancel(); // Retained handles are harmless after the call has retired.
}

TEST_CASE("whole-call deadlines retire blocked work without a sleeping test thread", "[remote][concurrency]") {
    Gate gate; Server server(false); r::Client client(server.client_options()); success(client.health());
    const auto before = server.rpc.metrics().accepted;
    failure(client.lookup("past", false, {r::Clock::now() - 1ms}), r::ErrorCode::DeadlineExceeded);
    CHECK(server.rpc.metrics().accepted == before);
    REQUIRE(server.workers.try_submit([&](std::stop_token) { gate.block(); }));
    ReleaseOnExit release{gate.release}; REQUIRE(gate.entered.wait());
    auto blocked = client.begin_load("blocked", {r::Clock::now() + 2s});
    REQUIRE(eventually([&] { return server.rpc.metrics().accepted == before + 1; }));
    failure(blocked, r::ErrorCode::DeadlineExceeded);
    REQUIRE(eventually([&] { return server.rpc.metrics().cancelled >= 1; }));
    gate.release.set(); success(client.lookup("barrier"));
    CHECK(server.cache.stats().ticket_records == 0);
}

TEST_CASE("repeated and concurrent shutdown settle requests that outlive the client", "[remote][concurrency]") {
    Gate gate; Server server(false);
    auto client = std::make_unique<r::Client>(server.client_options()); success(client->health());
    REQUIRE(server.workers.try_submit([&](std::stop_token) { gate.block(); }));
    ReleaseOnExit release{gate.release}; REQUIRE(gate.entered.wait());
    auto one = client->lookup("one"), two = client->lookup("two");
    auto closing_a = std::async(std::launch::async, [&] { client->shutdown(); });
    auto closing_b = std::async(std::launch::async, [&] { client->shutdown(); });
    REQUIRE(ready(closing_a)); closing_a.get(); REQUIRE(ready(closing_b)); closing_b.get();
    failure(one, r::ErrorCode::ClientClosed); failure(two, r::ErrorCode::ClientClosed);
    failure(client->lookup("after shutdown"), r::ErrorCode::ClientClosed);
    client.reset(); one.cancel(); two.cancel(); gate.release.set();
    r::Client independent(server.client_options()); success(independent.lookup("still alive"));
}

TEST_CASE("cancelled publication is not retried and reports an uncertain outcome", "[remote][concurrency]") {
    Gate gate; Server server(false); r::Client client(server.client_options());
    auto ticket = success(client.begin_load("post")).ticket;
    const auto before = server.rpc.metrics().accepted;
    REQUIRE(server.workers.try_submit([&](std::stop_token) { gate.block(); }));
    ReleaseOnExit release{gate.release}; REQUIRE(gate.entered.wait());
    auto publishing = client.publish(ticket, "old", publish_options());
    REQUIRE(eventually([&] { return server.rpc.metrics().accepted == before + 1; }));
    publishing.cancel(); failure(publishing, r::ErrorCode::Cancelled, true);
    REQUIRE(eventually([&] { return server.rpc.metrics().cancelled >= 1; }));
    gate.release.set();
    CHECK(success(client.lookup("post")).kind == r::LookupKind::Miss);
    CHECK(server.rpc.metrics().accepted == before + 2); // One publish, one lookup, no replay.
}

TEST_CASE("submissions racing shutdown all settle without leaking admission", "[remote][concurrency]") {
    Gate gate; Server server(false); auto options = server.client_options(); options.max_in_flight = 64;
    r::Client client(options); success(client.health());
    REQUIRE(server.workers.try_submit([&](std::stop_token) { gate.block(); }));
    ReleaseOnExit release{gate.release}; REQUIRE(gate.entered.wait());
    Signal start;
    auto submitter = std::async(std::launch::async, [&] {
        if (!start.wait()) throw std::runtime_error("start watchdog");
        std::vector<r::Request<r::LookupResult>> requests;
        for (unsigned i = 0; i < 32; ++i) requests.push_back(client.lookup("race"));
        return requests;
    });
    start.set(); client.shutdown();
    REQUIRE(ready(submitter));
    for (const auto& request : submitter.get()) failure(request, r::ErrorCode::ClientClosed);
    CHECK(client.in_flight() == 0);
}

TEST_CASE("completion notifications use the caller executor and survive client destruction", "[remote][concurrency]") {
    Server server(false); Events events; auto client = std::make_unique<r::Client>(server.client_options());
    auto request = client->lookup("post"); bool called = false;
    const auto caller = std::this_thread::get_id();
    auto receipt = request.on_complete(events.executor(), [&](auto result) {
        CHECK(std::this_thread::get_id() == caller); CHECK(result.has_value()); called = true;
    });
    REQUIRE(eventually([&] { return events.pending(); }));
    CHECK_FALSE(called); CHECK(success(request).kind == r::LookupKind::Miss);
    client.reset(); events.run_one(); REQUIRE(ready(receipt)); receipt.get(); CHECK(called);
    CHECK_THROWS_AS(request.on_complete(events.executor(), [](auto) {}), std::logic_error);
    r::Client another(server.client_options());
    auto finished = another.lookup("ready"); success(finished);
    auto rejected = finished.on_complete([](auto) { throw std::runtime_error("queue closed"); }, [](auto) {});
    REQUIRE(ready(rejected)); CHECK_THROWS_AS(rejected.get(), std::runtime_error);
    CHECK(finished.get().has_value()); // Dispatch failure cannot rewrite the RPC result.
    auto throws = another.lookup("handler");
    auto bad_callback = throws.on_complete(events.executor(), [](auto) { throw std::runtime_error("application failure"); });
    REQUIRE(eventually([&] { return events.pending(); })); events.run_one();
    REQUIRE(ready(bad_callback)); CHECK_THROWS_AS(bad_callback.get(), std::runtime_error);
    success(another.lookup("healthy"));
}

namespace {
class FaultyService final : public v1::Cache::CallbackService {
public:
    enum class Mode { UnknownKind, BadDuration, Oversized, LostPublish, UnknownPublish, NewProtocol };
    explicit FaultyService(Mode m) : mode(m) {}
    Mode mode;
    std::atomic<unsigned> lookups{0}, publications{0};
    grpc::ServerUnaryReactor* Lookup(grpc::CallbackServerContext* ctx, const v1::LookupRequest*, v1::LookupResponse* out) override {
        lookups++;
        out->set_kind(mode == Mode::UnknownKind ? static_cast<v1::LookupKind>(99) : v1::LOOKUP_KIND_FRESH);
        out->set_fresh_remaining_ms(mode == Mode::BadDuration ? std::numeric_limits<uint64_t>::max() : 1000);
        out->set_servable_remaining_ms(mode == Mode::BadDuration ? std::numeric_limits<uint64_t>::max() : 1000);
        if (mode == Mode::Oversized) out->set_value(std::string(8192, 'x'));
        auto* reactor = ctx->DefaultReactor(); reactor->Finish(grpc::Status::OK); return reactor;
    }
    grpc::ServerUnaryReactor* BeginLoad(grpc::CallbackServerContext* ctx, const v1::BeginLoadRequest*, v1::LoadGrant* out) override {
        out->set_ticket("opaque-test-ticket"); out->set_valid_for_ms(30000);
        auto* reactor = ctx->DefaultReactor(); reactor->Finish(grpc::Status::OK); return reactor;
    }
    grpc::ServerUnaryReactor* Publish(grpc::CallbackServerContext* ctx, const v1::PublishRequest*, v1::PublishResponse* out) override {
        publications++; out->set_outcome(static_cast<v1::Publication>(99));
        auto* reactor = ctx->DefaultReactor();
        reactor->Finish(mode == Mode::LostPublish ? grpc::Status(grpc::StatusCode::UNAVAILABLE, "private server diagnostic") : grpc::Status::OK);
        return reactor;
    }
    grpc::ServerUnaryReactor* Health(grpc::CallbackServerContext* ctx, const v1::HealthRequest*, v1::HealthResponse* out) override {
        out->set_ready(true); out->set_protocol_version("dbwaller.v99"); out->set_instance_id("other");
        auto* reactor = ctx->DefaultReactor(); reactor->Finish(grpc::Status::OK); return reactor;
    }
};
struct FaultyServer {
    FaultyService service;
    int port = 0;
    std::unique_ptr<grpc::Server> server;
    explicit FaultyServer(FaultyService::Mode m) : service(m) {
        grpc::ServerBuilder builder; builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service); server = builder.BuildAndStart(); REQUIRE(server); REQUIRE(port);
    }
    ~FaultyServer() { server->Shutdown(); server->Wait(); }
    r::ClientOptions options() const {
        r::ClientOptions o; o.endpoint = "127.0.0.1:" + std::to_string(port);
        o.bearer_token = token; o.cache_namespace = "public"; o.insecure_loopback = true; o.max_message_bytes = 4096; o.timeout = 5s; return o;
    }
};
}

TEST_CASE("client rejects invalid protocol responses without exposing server diagnostics", "[remote]") {
    const auto mode = GENERATE(FaultyService::Mode::UnknownKind, FaultyService::Mode::BadDuration, FaultyService::Mode::Oversized);
    FaultyServer server(mode); r::Client client(server.options());
    failure(client.lookup("post"), mode == FaultyService::Mode::Oversized ? r::ErrorCode::ResourceExhausted : r::ErrorCode::ProtocolError);
    CHECK(server.service.lookups == 1);
    failure(client.health(), r::ErrorCode::ProtocolError);
}

TEST_CASE("publication has typed outcomes and never retries an unknown result", "[remote]") {
    const auto mode = GENERATE(FaultyService::Mode::LostPublish, FaultyService::Mode::UnknownPublish);
    FaultyServer server(mode); r::Client client(server.options());
    auto ticket = success(client.begin_load("post")).ticket;
    auto result = await(client.publish(ticket, "fetched data", publish_options())); REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == (mode == FaultyService::Mode::LostPublish ? r::ErrorCode::Unavailable : r::ErrorCode::ProtocolError));
    CHECK(result.error().outcome_unknown); CHECK(result.error().message.find("private") == std::string::npos);
    CHECK(server.service.publications == 1);
}

TEST_CASE("client options fail closed and closed servers yield bounded errors", "[remote]") {
    Server server(false); auto valid = server.client_options();
    auto invalid = valid; invalid.endpoint = "0.0.0.0:7447"; CHECK_THROWS_AS(r::Client(invalid), std::invalid_argument);
    invalid = valid; invalid.endpoint = "localhost:7447"; CHECK_THROWS_AS(r::Client(invalid), std::invalid_argument);
    invalid = valid; invalid.bearer_token += '\n'; CHECK_THROWS_AS(r::Client(invalid), std::invalid_argument);
    invalid = valid; invalid.max_in_flight = 0; CHECK_THROWS_AS(r::Client(invalid), std::invalid_argument);
    invalid = valid; invalid.timeout = 0ms; CHECK_THROWS_AS(r::Client(invalid), std::invalid_argument);
    r::Client client(valid); success(client.lookup("warm channel"));
    failure(client.lookup(""), r::ErrorCode::InvalidArgument);
    server.stop(); failure(client.lookup("cache outage"), r::ErrorCode::Unavailable);
}

namespace {
template<class Store> void cache_policy(Store& store, bool caches) {
    std::string version = "v1"; unsigned fetches = 0;
    auto backend = [&](auto deadline) { CHECK(deadline > r::Clock::now()); ++fetches; return version; };
    example::Policy policy; policy.budget = 5s; policy.cache_budget = 1s;
    CHECK(example::get(store, "policy", backend, policy) == "v1");
    CHECK(example::get(store, "policy", backend, policy) == "v1");
    CHECK(fetches == (caches ? 1 : 2));
    version = "v2"; REQUIRE(store.invalidate("policy"));
    CHECK(example::get(store, "policy", backend, policy) == "v2");
    if (!caches) return;
    unsigned raced = 0;
    CHECK(example::get(store, "race", [&](auto) {
        if (++raced == 1) { REQUIRE(store.invalidate("race")); return std::string("old fetch"); }
        return std::string("new fetch");
    }, policy) == "new fetch");
    CHECK(raced == 2);
    policy.max_backend_fetches = 1;
    CHECK_THROWS(example::get(store, "one-attempt", [&](auto) {
        store.invalidate("one-attempt"); return std::string("must be discarded");
    }, policy));
}
}
TEST_CASE("generic example tests disabled embedded and remote policies", "[remote]") {
    SECTION("disabled") { example::DisabledStore store; cache_policy(store, false); }
    SECTION("embedded") { example::EmbeddedStore store; cache_policy(store, true); }
    SECTION("remote") { Server server; example::RemoteStore store(server.client_options()); cache_policy(store, true); }
    SECTION("outage fallback") {
        Server server; example::RemoteStore store(server.client_options()); success(store.client.health()); server.stop();
        unsigned fetches = 0;
        CHECK(example::get(store, "outage", [&](auto) { ++fetches; return std::string("backend"); }) == "backend");
        CHECK(fetches == 1); // No silently-created embedded cache or unbounded retry.
    }
}
