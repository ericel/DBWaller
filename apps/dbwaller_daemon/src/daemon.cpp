#include "dbwaller/daemon/rpc_service.hpp"
#include <grpc/grpc.h>

#ifndef GRPC_ARG_MAX_ALLOWED_INCOMING_CONNECTIONS
#error "dbwallerd requires gRPC with GRPC_ARG_MAX_ALLOWED_INCOMING_CONNECTIONS support"
#endif

namespace dbwaller::daemon {
namespace {
ServerConfig checked(ServerConfig config) { validate_server_config(config); return config; }
}
std::unique_ptr<grpc::Server> start_server(const ServerConfig& config, RpcService& rpc, int& port) {
    validate_server_config(config);
    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(config.max_message_bytes);
    builder.SetMaxSendMessageSize(config.max_message_bytes);
    grpc::ResourceQuota quota;
    quota.Resize(config.transport_memory_bytes);
    quota.SetMaxThreads(16);
    builder.SetResourceQuota(quota);
    builder.AddChannelArgument(GRPC_ARG_MAX_ALLOWED_INCOMING_CONNECTIONS, config.max_connections);
    builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS, config.max_streams_per_connection);
    builder.AddChannelArgument(GRPC_ARG_MAX_CONNECTION_IDLE_MS, 60'000);
    builder.AddChannelArgument(GRPC_ARG_SERVER_HANDSHAKE_TIMEOUT_MS, 10'000);
    builder.AddChannelArgument(GRPC_ARG_MAX_METADATA_SIZE, 8192);
    builder.AddChannelArgument(GRPC_ARG_ABSOLUTE_MAX_METADATA_SIZE, 8192);
    std::shared_ptr<grpc::ServerCredentials> credentials;
    if (config.insecure_loopback) credentials = grpc::InsecureServerCredentials();
    else {
        grpc::SslServerCredentialsOptions ssl;
        ssl.pem_key_cert_pairs.push_back({config.private_key, config.certificate});
        credentials = grpc::SslServerCredentials(ssl);
    }
    builder.AddListeningPort(config.address, std::move(credentials), &port);
    builder.RegisterService(&rpc);
    auto server = builder.BuildAndStart();
    if (!server || !port) throw std::runtime_error("unable to start daemon listener");
    return server;
}
Daemon::Daemon(ServerConfig config)
    : config_(checked(std::move(config))), cache_(config_.cache, config_.limits),
      workers_(config_.workers, config_.queue_capacity), rpc_(cache_, workers_, config_),
      server_(start_server(config_, rpc_, port_)) {}
Daemon::~Daemon() { shutdown(); }
void Daemon::shutdown() {
    std::lock_guard lock(shutdown_mu_);
    if (!server_) return;
    rpc_.close_admission();
    server_->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(config_.shutdown_grace_ms));
    workers_.shutdown();
    server_->Wait();
    server_.reset(); // All RPC contexts/reactors have finished before destroying their dependencies.
    cache_.shutdown();
}
} // namespace dbwaller::daemon
