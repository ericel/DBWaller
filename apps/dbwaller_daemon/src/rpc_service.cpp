#include "dbwaller/daemon/rpc_service.hpp"
#include <openssl/crypto.h>

namespace dbwaller::daemon {
namespace {
grpc::Status translate(const ServiceError& e) {
    using C = grpc::StatusCode;
    switch (e.code()) {
        case ErrorCode::InvalidArgument: return {C::INVALID_ARGUMENT, e.what()};
        case ErrorCode::PermissionDenied: return {C::PERMISSION_DENIED, e.what()};
        case ErrorCode::ResourceExhausted: return {C::RESOURCE_EXHAUSTED, e.what()};
        case ErrorCode::Unavailable: return {C::UNAVAILABLE, e.what()};
        case ErrorCode::Internal: return {C::INTERNAL, "cache operation failed"};
    }
    return {C::INTERNAL, "cache operation failed"};
}
void grant(const LoadGrant& load, v1::LoadGrant* out) {
    out->set_ticket(load.ticket);
    out->set_valid_for_ms(load.valid_for_ms);
}
grpc::Status cancellation(grpc::CallbackServerContext* ctx) {
    if (std::chrono::system_clock::now() >= ctx->deadline())
        return {grpc::StatusCode::DEADLINE_EXCEEDED, "RPC deadline expired"};
    if (ctx->IsCancelled()) return {grpc::StatusCode::CANCELLED, "RPC cancelled"};
    return grpc::Status::OK;
}
} // namespace

class RpcService::Call final : public grpc::ServerUnaryReactor {
public:
    Call(RpcService& service, bool counted) : service_(service), counted_(counted) {}
    void OnCancel() override { service_.cancelled_.fetch_add(1); }
    void OnDone() override {
        if (counted_) service_.active_.fetch_sub(1);
        delete this;
    }
private:
    RpcService& service_;
    bool counted_;
};

RpcService::RpcService(CacheService& cache, concurrency::ThreadPool& workers, const ServerConfig& config)
    : cache_(cache), workers_(workers), credentials_(config.credentials), max_active_(config.max_active_rpcs) {}
RpcService::Metrics RpcService::metrics() const noexcept {
    return {active_.load(), accepted_.load(), rejected_.load(), cancelled_.load()};
}
AccessContext RpcService::authorize(grpc::CallbackServerContext* ctx, std::string_view ns, Action action) const {
    const auto& metadata = ctx->client_metadata();
    const auto range = metadata.equal_range("authorization");
    if (range.first == range.second || std::next(range.first) != range.second)
        throw grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "bearer credential required");
    const auto& value = range.first->second;
    const std::string_view header(value.data(), value.size());
    if (!header.starts_with("Bearer ") || header.size() < 39 || header.size() > 1031)
        throw grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "invalid bearer credential");
    const auto digest = token_digest(std::string(header.substr(7)));
    const Credential* identity = nullptr;
    for (const auto& candidate : credentials_) {
        if (CRYPTO_memcmp(digest.data(), candidate.token_hash.data(), digest.size()) == 0) identity = &candidate;
    }
    if (!identity) throw grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "invalid bearer credential");
    const bool permitted = action == Action::Admin ? identity->admin :
        identity->namespaces.contains(std::string(ns)) &&
        (action != Action::Write || identity->write) && (action != Action::Invalidate || identity->invalidate);
    if (!permitted) throw grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "operation not authorized");
    return {identity->principal, std::string(ns)};
}
grpc::ServerUnaryReactor* RpcService::dispatch(grpc::CallbackServerContext* ctx, std::string_view ns,
                                              Action action, Work work) {
    if (!accepting_.load()) {
        auto* call = new Call(*this, false);
        rejected_.fetch_add(1);
        call->Finish({grpc::StatusCode::UNAVAILABLE, "daemon stopping"});
        return call;
    }
    if (active_.fetch_add(1) >= max_active_) {
        active_.fetch_sub(1);
        auto* call = new Call(*this, false);
        rejected_.fetch_add(1);
        call->Finish({grpc::StatusCode::RESOURCE_EXHAUSTED, "RPC capacity exhausted"});
        return call;
    }
    auto* call = new Call(*this, true);
    try {
        if (ctx->deadline() == std::chrono::system_clock::time_point::max()) {
            rejected_.fetch_add(1);
            call->Finish({grpc::StatusCode::INVALID_ARGUMENT, "finite RPC deadline required"});
            return call;
        }
        auto task = [this, ctx, ns, action, work = std::move(work), call](std::stop_token) {
            grpc::Status status;
            try {
                status = cancellation(ctx);
                if (status.ok()) {
                    const auto access = authorize(ctx, ns, action);
                    status = cancellation(ctx);
                    if (status.ok()) {
                        work(access);
                        status = cancellation(ctx); // A completed write is not rolled back.
                    }
                }
            } catch (const grpc::Status& e) { status = e; }
            catch (const ServiceError& e) { status = translate(e); }
            catch (...) { status = {grpc::StatusCode::INTERNAL, "cache operation failed"}; }
            call->Finish(status); // OnDone may delete call; do not use it afterwards.
        };
        if (!workers_.try_submit(std::move(task))) {
            rejected_.fetch_add(1);
            call->Finish({grpc::StatusCode::RESOURCE_EXHAUSTED, "RPC queue full"});
        } else {
            accepted_.fetch_add(1);
        }
    } catch (...) {
        rejected_.fetch_add(1);
        call->Finish({grpc::StatusCode::INTERNAL, "RPC admission failed"});
    }
    return call;
}
grpc::ServerUnaryReactor* RpcService::Lookup(grpc::CallbackServerContext* ctx, const v1::LookupRequest* req, v1::LookupResponse* out) {
    return dispatch(ctx, req->cache_namespace(), req->acquire_ticket() ? Action::Write : Action::Read,
        [this, req, out](const AccessContext& access) {
            const auto read = cache_.lookup(access, req->key(), req->acquire_ticket());
            switch (read.snapshot.result.kind) {
                case core::ShardedEngine::GetKind::Miss: out->set_kind(v1::LOOKUP_KIND_MISS); break;
                case core::ShardedEngine::GetKind::Fresh: out->set_kind(v1::LOOKUP_KIND_FRESH); break;
                case core::ShardedEngine::GetKind::Stale: out->set_kind(v1::LOOKUP_KIND_STALE); break;
            }
            if (read.snapshot.result.value) out->set_value(*read.snapshot.result.value);
            out->set_fresh_remaining_ms(read.snapshot.fresh_remaining_ms);
            out->set_servable_remaining_ms(read.snapshot.servable_remaining_ms);
            if (read.load) grant(*read.load, out->mutable_load());
        });
}
grpc::ServerUnaryReactor* RpcService::BeginLoad(grpc::CallbackServerContext* ctx, const v1::BeginLoadRequest* req, v1::LoadGrant* out) {
    return dispatch(ctx, req->cache_namespace(), Action::Write, [this, req, out](const AccessContext& access) {
        grant(cache_.begin_load(access, req->key()), out);
    });
}
grpc::ServerUnaryReactor* RpcService::Publish(grpc::CallbackServerContext* ctx, const v1::PublishRequest* req, v1::PublishResponse* out) {
    return dispatch(ctx, req->cache_namespace(), Action::Write, [this, req, out](const AccessContext& access) {
        if (!req->has_options()) throw ServiceError(ErrorCode::InvalidArgument, "publication options required");
        PublishOptions opt{req->options().ttl_ms(), req->options().swr_ms(), {}};
        opt.tags.assign(req->options().tags().begin(), req->options().tags().end());
        const auto result = cache_.publish(access, req->ticket(), req->value(), opt);
        switch (result) {
            case Publication::Stored: out->set_outcome(v1::PUBLICATION_STORED); break;
            case Publication::Invalidated: out->set_outcome(v1::PUBLICATION_INVALIDATED); break;
            case Publication::Expired: out->set_outcome(v1::PUBLICATION_EXPIRED); break;
            case Publication::Consumed: out->set_outcome(v1::PUBLICATION_CONSUMED); break;
            case Publication::UnknownTicket: out->set_outcome(v1::PUBLICATION_UNKNOWN_TICKET); break;
        }
    });
}
grpc::ServerUnaryReactor* RpcService::InvalidateKey(grpc::CallbackServerContext* ctx, const v1::InvalidateKeyRequest* req, v1::InvalidationResponse*) {
    return dispatch(ctx, req->cache_namespace(), Action::Invalidate, [this, req](const AccessContext& access) { cache_.invalidate_key(access, req->key()); });
}
grpc::ServerUnaryReactor* RpcService::InvalidateTag(grpc::CallbackServerContext* ctx, const v1::InvalidateTagRequest* req, v1::InvalidationResponse*) {
    return dispatch(ctx, req->cache_namespace(), Action::Invalidate, [this, req](const AccessContext& access) { cache_.invalidate_tag(access, req->tag()); });
}
grpc::ServerUnaryReactor* RpcService::Health(grpc::CallbackServerContext* ctx, const v1::HealthRequest*, v1::HealthResponse* out) {
    return dispatch(ctx, {}, Action::Admin, [this, out](const AccessContext&) {
        out->set_ready(accepting() && cache_.stats().ready);
        out->set_protocol_version("dbwaller.v1");
        out->set_build_version(DBWALLER_BUILD_VERSION);
        out->set_instance_id(cache_.instance_id());
    });
}
grpc::ServerUnaryReactor* RpcService::Stats(grpc::CallbackServerContext* ctx, const v1::StatsRequest*, v1::StatsResponse* out) {
    return dispatch(ctx, {}, Action::Admin, [this, out](const AccessContext&) {
        const auto stats = cache_.stats();
        out->set_ticket_records(stats.ticket_records); out->set_ticket_key_bytes(stats.ticket_key_bytes);
        out->set_hits(stats.engine.hits); out->set_stale_hits(stats.engine.stale_hits); out->set_misses(stats.engine.misses);
        out->set_puts(stats.engine.puts); out->set_evictions(stats.engine.evictions); out->set_expirations(stats.engine.expirations);
        const auto rpc = metrics();
        out->set_rpc_active(rpc.active); out->set_rpc_accepted(rpc.accepted); out->set_rpc_rejected(rpc.rejected);
        out->set_rpc_cancelled(rpc.cancelled);
    });
}
} // namespace dbwaller::daemon
