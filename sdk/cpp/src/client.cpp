#include "dbwaller/remote/client.hpp"
#include "dbwaller/v1/cache.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <openssl/ssl.h>
#include <charconv>
#include <limits>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace dbwaller::remote {
void initialize_process_tls() {
    if (OPENSSL_init_ssl(OPENSSL_INIT_NO_ATEXIT, nullptr) != 1)
        throw std::runtime_error("TLS process initialization failed");
}
namespace detail {
struct Access {
    struct Scope { std::string cache_namespace, endpoint; };
    template<class T> static Request<T> request(std::shared_ptr<Completion<T>> result,
                                               std::weak_ptr<Cancellation> cancellation = {}) {
        return Request<T>(std::move(result), std::move(cancellation));
    }
    static Ticket ticket(std::string bytes, const Scope& options) {
        Ticket t; t.bytes_ = std::move(bytes); t.namespace_ = options.cache_namespace; t.endpoint_ = options.endpoint;
        return t;
    }
    static const std::string& bytes(const Ticket& t) { return t.bytes_; }
    static bool matches(const Ticket& t, const ClientOptions& o) {
        return t.valid() && t.namespace_ == o.cache_namespace && t.endpoint_ == o.endpoint;
    }
};
}
namespace {
Error error(ErrorCode code, bool unknown = false) {
    const char* text = "cache request failed";
    switch (code) {
        case ErrorCode::InvalidArgument: text = "invalid cache request"; break;
        case ErrorCode::Unauthenticated: text = "cache authentication failed"; break;
        case ErrorCode::PermissionDenied: text = "cache operation not authorized"; break;
        case ErrorCode::ResourceExhausted: text = "cache request capacity exhausted"; break;
        case ErrorCode::Unavailable: text = "cache unavailable"; break;
        case ErrorCode::DeadlineExceeded: text = "cache request deadline exceeded"; break;
        case ErrorCode::Cancelled: text = "cache request cancelled"; break;
        case ErrorCode::ClientClosed: text = "cache client closed"; break;
        case ErrorCode::ProtocolError: text = "invalid cache protocol response"; break;
        case ErrorCode::Internal: break;
    }
    return {code, text, unknown};
}
ErrorCode translate(grpc::StatusCode code) {
    switch (code) {
        case grpc::StatusCode::INVALID_ARGUMENT: return ErrorCode::InvalidArgument;
        case grpc::StatusCode::UNAUTHENTICATED: return ErrorCode::Unauthenticated;
        case grpc::StatusCode::PERMISSION_DENIED: return ErrorCode::PermissionDenied;
        case grpc::StatusCode::RESOURCE_EXHAUSTED: return ErrorCode::ResourceExhausted;
        case grpc::StatusCode::UNAVAILABLE: return ErrorCode::Unavailable;
        case grpc::StatusCode::DEADLINE_EXCEEDED: return ErrorCode::DeadlineExceeded;
        case grpc::StatusCode::CANCELLED: return ErrorCode::Cancelled;
        case grpc::StatusCode::UNIMPLEMENTED: return ErrorCode::ProtocolError;
        default: return ErrorCode::Internal;
    }
}
void validate(const ClientOptions& o) {
    const auto colon = o.endpoint.rfind(':');
    if (colon == std::string::npos || !colon || o.endpoint.size() > 256) throw std::invalid_argument("invalid cache endpoint");
    const auto port_text = std::string_view(o.endpoint).substr(colon + 1);
    unsigned port = 0;
    const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() || !port || port > 65535)
        throw std::invalid_argument("invalid cache endpoint");
    if (o.insecure_loopback && ((o.endpoint.substr(0, colon) != "127.0.0.1" && o.endpoint.substr(0, colon) != "[::1]") ||
                                !o.root_certificates.empty())) throw std::invalid_argument("plaintext requires literal loopback");
    if (o.cache_namespace.empty() || o.cache_namespace.size() > 256 ||
        o.bearer_token.size() < 32 || o.bearer_token.size() > 1024 ||
        o.timeout.count() <= 0 || o.timeout > std::chrono::hours(24) ||
        !o.max_in_flight || o.max_in_flight > 4096 ||
        o.max_message_bytes < 4096 || o.max_message_bytes > 16 * 1024 * 1024 || o.root_certificates.size() > 65536)
        throw std::invalid_argument("invalid cache client options");
    for (unsigned char c : o.bearer_token)
        if (c < 0x21 || c > 0x7e) throw std::invalid_argument("invalid cache credential format");
}
Milliseconds remaining(uint64_t ms, Milliseconds elapsed) {
    constexpr uint64_t maximum = 86'400'000;
    if (ms > maximum) throw std::runtime_error("invalid duration");
    return Milliseconds(ms) > elapsed ? Milliseconds(ms) - elapsed : Milliseconds(0);
}
LoadGrant decode_grant(const v1::LoadGrant& grant, const detail::Access::Scope& options, Milliseconds elapsed) {
    if (grant.ticket().empty() || grant.ticket().size() > 4096) throw std::runtime_error("invalid ticket");
    return {detail::Access::ticket(grant.ticket(), options), remaining(grant.valid_for_ms(), elapsed)};
}
template<class T> Request<T> failed(Error failure) {
    auto result = std::make_shared<detail::Completion<T>>();
    result->complete(std::move(failure));
    return detail::Access::request(result);
}
} // namespace

class Client::Impl {
    struct CallBase : detail::Cancellation {
        grpc::ClientContext context;
        grpc::Status status;
        Clock::time_point start, deadline;
        bool mutation;
        std::mutex mutex;
        bool finished = false;
        std::optional<ErrorCode> stopped;
        CallBase(Clock::time_point s, Clock::time_point d, bool m) : start(s), deadline(d), mutation(m) {}
        void stop(ErrorCode why) noexcept {
            std::lock_guard lock(mutex);
            if (finished || stopped) return;
            stopped = Clock::now() >= deadline ? ErrorCode::DeadlineExceeded : why;
            context.TryCancel();
        }
        void cancel() noexcept override { stop(ErrorCode::Cancelled); }
        virtual void finish(bool ok) = 0;
    };
    template<class T, class Wire> struct Call final : CallBase {
        Wire response;
        std::unique_ptr<grpc::ClientAsyncResponseReader<Wire>> reader;
        std::shared_ptr<detail::Completion<T>> result = std::make_shared<detail::Completion<T>>();
        std::function<T(const Wire&, Clock::time_point)> decode;
        Call(Clock::time_point s, Clock::time_point d, bool m,
             std::function<T(const Wire&, Clock::time_point)> decoder)
            : CallBase(s, d, m), decode(std::move(decoder)) {}
        void finish(bool ok) override {
            auto outcome = [&]() -> Result<T> {
                std::lock_guard lock(this->mutex);
                this->finished = true;
                if (this->stopped) return error(*this->stopped, this->mutation);
                if (Clock::now() >= this->deadline) return error(ErrorCode::DeadlineExceeded, this->mutation);
                if (!ok) return error(ErrorCode::Unavailable, this->mutation);
                if (!this->status.ok()) return error(translate(this->status.error_code()), this->mutation);
                try {
                    auto value = decode(response, this->start);
                    if (Clock::now() >= this->deadline) return error(ErrorCode::DeadlineExceeded, this->mutation);
                    return value;
                } catch (...) { return error(ErrorCode::ProtocolError, this->mutation); }
            }();
            result->complete(std::move(outcome));
        }
    };
public:
    ClientOptions options;
    std::unique_ptr<v1::Cache::Stub> stub;
    explicit Impl(ClientOptions o) : options(std::move(o)) {
        validate(options);
        grpc::ChannelArguments args;
        args.SetInt(GRPC_ARG_ENABLE_RETRIES, 0);
        args.SetMaxReceiveMessageSize(options.max_message_bytes);
        args.SetMaxSendMessageSize(options.max_message_bytes);
        grpc::ResourceQuota quota; quota.Resize(32 * 1024 * 1024); quota.SetMaxThreads(4);
        args.SetResourceQuota(quota);
        std::shared_ptr<grpc::ChannelCredentials> credentials;
        if (options.insecure_loopback) credentials = grpc::InsecureChannelCredentials();
        else { grpc::SslCredentialsOptions tls; tls.pem_root_certs = options.root_certificates; credentials = grpc::SslCredentials(tls); }
        stub = v1::Cache::NewStub(grpc::CreateCustomChannel(options.endpoint, credentials, args));
        try { worker_ = std::thread([this] { drain(); }); }
        catch (...) { queue_.Shutdown(); void* tag; bool ok; while (queue_.Next(&tag, &ok)) {} throw; }
        worker_id_ = worker_.get_id();
    }
    ~Impl() { shutdown(); }
    void shutdown() {
        if (std::this_thread::get_id() == worker_id_)
            throw std::logic_error("completion executor must enqueue without running client lifecycle operations inline");
        std::lock_guard joining(shutdown_mutex_);
        if (!worker_.joinable()) return;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            for (const auto& [_, call] : pending_) call->stop(ErrorCode::ClientClosed);
            queue_.Shutdown();
        }
        worker_.join();
    }
    size_t in_flight() const { std::lock_guard lock(mutex_); return pending_.size(); }

    template<class T, class Wire, class Input, class Start, class Decode>
    Request<T> submit(Clock::time_point start, Input input, CallOptions call_options, bool mutation, Start start_rpc, Decode decode) {
        const auto deadline = call_options.deadline.value_or(start + options.timeout);
        if (deadline <= Clock::now()) return failed<T>(error(ErrorCode::DeadlineExceeded));
        if (deadline - start > std::chrono::hours(24) || input.ByteSizeLong() > static_cast<size_t>(options.max_message_bytes))
            return failed<T>(error(ErrorCode::InvalidArgument));
        auto call = std::make_shared<Call<T, Wire>>(start, deadline, mutation, std::move(decode));
        call->context.set_deadline(std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            std::chrono::system_clock::now() + (deadline - Clock::now())));
        call->context.AddMetadata("authorization", "Bearer " + options.bearer_token);
        auto request = detail::Access::request(call->result, call);
        {
            std::lock_guard lock(mutex_);
            if (closed_) return failed<T>(error(ErrorCode::ClientClosed));
            if (Clock::now() >= deadline) return failed<T>(error(ErrorCode::DeadlineExceeded));
            if (pending_.size() >= options.max_in_flight) return failed<T>(error(ErrorCode::ResourceExhausted));
            // Register before starting; shutdown cannot close the CQ between Start and Finish.
            pending_.emplace(call.get(), call);
            try {
                call->reader = start_rpc(&call->context, input, &queue_);
                call->reader->Finish(&call->response, &call->status, call.get());
            } catch (...) {
                pending_.erase(call.get());
                call->context.TryCancel();
                return failed<T>(error(ErrorCode::Internal, mutation));
            }
        }
        return request;
    }
private:
    void drain() {
        void* tag; bool ok;
        while (queue_.Next(&tag, &ok)) {
            std::shared_ptr<CallBase> call;
            {
                std::lock_guard lock(mutex_);
                auto found = pending_.find(tag);
                if (found == pending_.end()) continue;
                call = std::move(found->second); pending_.erase(found);
            }
            call->finish(ok);
        }
    }
    grpc::CompletionQueue queue_;
    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::unordered_map<void*, std::shared_ptr<CallBase>> pending_;
    bool closed_ = false;
    std::thread worker_;
    std::thread::id worker_id_;
};

Client::Client(ClientOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Client::~Client() = default;
void Client::shutdown() { impl_->shutdown(); }
size_t Client::in_flight() const { return impl_->in_flight(); }

Request<LookupResult> Client::lookup(std::string key, bool acquire_ticket, CallOptions options) {
    const auto entered = Clock::now();
    if (key.empty()) return failed<LookupResult>(error(ErrorCode::InvalidArgument));
    v1::LookupRequest req; req.set_cache_namespace(impl_->options.cache_namespace); req.set_key(std::move(key)); req.set_acquire_ticket(acquire_ticket);
    return impl_->submit<LookupResult, v1::LookupResponse>(entered, std::move(req), options, false,
        [this](auto* ctx, const auto& input, auto* queue) { return impl_->stub->AsyncLookup(ctx, input, queue); },
        [config = detail::Access::Scope{impl_->options.cache_namespace, impl_->options.endpoint}, acquire_ticket](const v1::LookupResponse& r, auto start) {
            LookupResult out;
            const auto elapsed = std::chrono::ceil<Milliseconds>(Clock::now() - start);
            switch (r.kind()) {
                case v1::LOOKUP_KIND_MISS: out.kind = LookupKind::Miss; break;
                case v1::LOOKUP_KIND_FRESH: out.kind = LookupKind::Fresh; break;
                case v1::LOOKUP_KIND_STALE: out.kind = LookupKind::Stale; break;
                default: throw std::runtime_error("invalid kind");
            }
            if (r.fresh_remaining_ms() > r.servable_remaining_ms() || r.has_load() != acquire_ticket ||
                (out.kind == LookupKind::Miss && (!r.value().empty() || r.fresh_remaining_ms() || r.servable_remaining_ms())) ||
                (out.kind == LookupKind::Stale && r.fresh_remaining_ms())) throw std::runtime_error("invalid snapshot");
            out.value = r.value(); out.fresh_for = remaining(r.fresh_remaining_ms(), elapsed);
            out.servable_for = remaining(r.servable_remaining_ms(), elapsed);
            if (r.has_load()) out.load = decode_grant(r.load(), config, elapsed);
            return out;
        });
}
Request<LoadGrant> Client::begin_load(std::string key, CallOptions options) {
    const auto entered = Clock::now();
    if (key.empty()) return failed<LoadGrant>(error(ErrorCode::InvalidArgument));
    v1::BeginLoadRequest req; req.set_cache_namespace(impl_->options.cache_namespace); req.set_key(std::move(key));
    return impl_->submit<LoadGrant, v1::LoadGrant>(entered, std::move(req), options, false,
        [this](auto* ctx, const auto& input, auto* queue) { return impl_->stub->AsyncBeginLoad(ctx, input, queue); },
        [config = detail::Access::Scope{impl_->options.cache_namespace, impl_->options.endpoint}](const auto& r, auto start) {
            return decode_grant(r, config, std::chrono::ceil<Milliseconds>(Clock::now() - start));
        });
}
Request<PublishStatus> Client::publish(Ticket ticket, std::string value, PublishOptions publish, CallOptions options) {
    const auto entered = Clock::now();
    if (!detail::Access::matches(ticket, impl_->options) || publish.ttl.count() < 0 || publish.swr.count() < 0 ||
        publish.ttl > std::chrono::hours(24) || publish.swr > std::chrono::hours(24) ||
        publish.ttl + publish.swr > std::chrono::hours(24)) return failed<PublishStatus>(error(ErrorCode::InvalidArgument));
    v1::PublishRequest req; req.set_cache_namespace(impl_->options.cache_namespace);
    req.set_ticket(detail::Access::bytes(ticket)); req.set_value(std::move(value));
    req.mutable_options()->set_ttl_ms(publish.ttl.count()); req.mutable_options()->set_swr_ms(publish.swr.count());
    for (auto& tag : publish.tags) req.mutable_options()->add_tags(std::move(tag));
    return impl_->submit<PublishStatus, v1::PublishResponse>(entered, std::move(req), options, true,
        [this](auto* ctx, const auto& input, auto* queue) { return impl_->stub->AsyncPublish(ctx, input, queue); },
        [](const auto& r, auto) {
            switch (r.outcome()) {
                case v1::PUBLICATION_STORED: return PublishStatus::Stored;
                case v1::PUBLICATION_INVALIDATED: return PublishStatus::Invalidated;
                case v1::PUBLICATION_EXPIRED: return PublishStatus::Expired;
                case v1::PUBLICATION_CONSUMED: return PublishStatus::Consumed;
                case v1::PUBLICATION_UNKNOWN_TICKET: return PublishStatus::UnknownTicket;
                default: throw std::runtime_error("invalid publication");
            }
        });
}
Request<Unit> Client::invalidate_key(std::string key, CallOptions options) {
    const auto entered = Clock::now();
    if (key.empty()) return failed<Unit>(error(ErrorCode::InvalidArgument));
    v1::InvalidateKeyRequest req; req.set_cache_namespace(impl_->options.cache_namespace); req.set_key(std::move(key));
    return impl_->submit<Unit, v1::InvalidationResponse>(entered, std::move(req), options, true,
        [this](auto* ctx, const auto& input, auto* queue) { return impl_->stub->AsyncInvalidateKey(ctx, input, queue); },
        [](const auto&, auto) { return Unit{}; });
}
Request<Unit> Client::invalidate_tag(std::string tag, CallOptions options) {
    const auto entered = Clock::now();
    if (tag.empty()) return failed<Unit>(error(ErrorCode::InvalidArgument));
    v1::InvalidateTagRequest req; req.set_cache_namespace(impl_->options.cache_namespace); req.set_tag(std::move(tag));
    return impl_->submit<Unit, v1::InvalidationResponse>(entered, std::move(req), options, true,
        [this](auto* ctx, const auto& input, auto* queue) { return impl_->stub->AsyncInvalidateTag(ctx, input, queue); },
        [](const auto&, auto) { return Unit{}; });
}
Request<Health> Client::health(CallOptions options) {
    return impl_->submit<Health, v1::HealthResponse>(Clock::now(), v1::HealthRequest{}, options, false,
        [this](auto* ctx, const auto& input, auto* queue) { return impl_->stub->AsyncHealth(ctx, input, queue); },
        [](const auto& r, auto) {
            if (r.protocol_version() != "dbwaller.v1" || r.instance_id().empty()) throw std::runtime_error("incompatible protocol");
            return Health{r.ready(), r.protocol_version(), r.build_version(), r.instance_id()};
        });
}
Request<Stats> Client::stats(CallOptions options) {
    return impl_->submit<Stats, v1::StatsResponse>(Clock::now(), v1::StatsRequest{}, options, false,
        [this](auto* ctx, const auto& input, auto* queue) { return impl_->stub->AsyncStats(ctx, input, queue); },
        [](const auto& r, auto) { return Stats{r.ticket_records(), r.ticket_key_bytes(), r.hits(), r.stale_hits(), r.misses(), r.puts(),
            r.evictions(), r.expirations(), r.rpc_active(), r.rpc_accepted(), r.rpc_rejected(), r.rpc_cancelled()}; });
}
} // namespace dbwaller::remote
