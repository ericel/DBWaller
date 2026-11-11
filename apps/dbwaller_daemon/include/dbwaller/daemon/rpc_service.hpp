#pragma once

#include "dbwaller/daemon/server_config.hpp"
#include "dbwaller/v1/cache.grpc.pb.h"
#include <grpcpp/grpcpp.h>

namespace dbwaller::daemon {
class RpcService final : public v1::Cache::CallbackService {
public:
    struct Metrics { uint64_t active, accepted, rejected, cancelled; };
    // Dependencies outlive the server and all OnDone reactions. The executor is
    // bounded and runs only local cache work in production.
    RpcService(CacheService& cache, concurrency::ThreadPool& workers, const ServerConfig& config);
    void close_admission() noexcept { accepting_.store(false); }
    bool accepting() const noexcept { return accepting_.load(); }
    Metrics metrics() const noexcept;

    grpc::ServerUnaryReactor* Lookup(grpc::CallbackServerContext*, const v1::LookupRequest*, v1::LookupResponse*) override;
    grpc::ServerUnaryReactor* BeginLoad(grpc::CallbackServerContext*, const v1::BeginLoadRequest*, v1::LoadGrant*) override;
    grpc::ServerUnaryReactor* Publish(grpc::CallbackServerContext*, const v1::PublishRequest*, v1::PublishResponse*) override;
    grpc::ServerUnaryReactor* InvalidateKey(grpc::CallbackServerContext*, const v1::InvalidateKeyRequest*, v1::InvalidationResponse*) override;
    grpc::ServerUnaryReactor* InvalidateTag(grpc::CallbackServerContext*, const v1::InvalidateTagRequest*, v1::InvalidationResponse*) override;
    grpc::ServerUnaryReactor* Health(grpc::CallbackServerContext*, const v1::HealthRequest*, v1::HealthResponse*) override;
    grpc::ServerUnaryReactor* Stats(grpc::CallbackServerContext*, const v1::StatsRequest*, v1::StatsResponse*) override;
private:
    class Call;
    enum class Action { Read, Write, Invalidate, Admin };
    using Work = std::function<void(const AccessContext&)>;
    grpc::ServerUnaryReactor* dispatch(grpc::CallbackServerContext*, std::string_view ns, Action, Work);
    AccessContext authorize(grpc::CallbackServerContext*, std::string_view ns, Action) const;
    CacheService& cache_;
    concurrency::ThreadPool& workers_;
    const std::vector<Credential> credentials_;
    const size_t max_active_;
    std::atomic<bool> accepting_{true};
    std::atomic<uint64_t> active_{0}, accepted_{0}, rejected_{0}, cancelled_{0};
};

std::unique_ptr<grpc::Server> start_server(const ServerConfig&, RpcService&, int& selected_port);

class Daemon {
public:
    explicit Daemon(ServerConfig config);
    ~Daemon();
    void shutdown();
    int port() const noexcept { return port_; }
private:
    ServerConfig config_;
    CacheService cache_;
    concurrency::ThreadPool workers_;
    RpcService rpc_;
    int port_ = 0;
    std::unique_ptr<grpc::Server> server_;
    std::mutex shutdown_mu_;
};
} // namespace dbwaller::daemon
