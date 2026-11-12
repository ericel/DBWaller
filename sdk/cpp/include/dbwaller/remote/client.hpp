#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>
#include <utility>

namespace dbwaller::remote {
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

// Application-owned process policy: call at startup before ANY OpenSSL use or
// worker creation. Prevents automatic global crypto cleanup racing gRPC workers
// at process exit. Repeated calls are safe, but cannot undo earlier OpenSSL init.
// Global crypto allocations live until OS process reclamation; do not manually
// call OPENSSL_cleanup() or unload OpenSSL while gRPC can still use it.
// Call explicitly: Client does not call this helper before gRPC initialization.
// Throws std::runtime_error if OpenSSL initialization fails.
void initialize_process_tls();

enum class ErrorCode {
    InvalidArgument, Unauthenticated, PermissionDenied, ResourceExhausted,
    Unavailable, DeadlineExceeded, Cancelled, ClientClosed, ProtocolError, Internal
};
struct Error {
    ErrorCode code;
    std::string message; // Generic diagnostics; never includes credentials or caller data.
    bool outcome_unknown = false; // An attempted mutation may have reached the server.
};
template<class T> class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(std::move(error)) {}
    bool has_value() const noexcept { return std::holds_alternative<T>(data_); }
    explicit operator bool() const noexcept { return has_value(); }
    const T& value() const { return std::get<T>(data_); }
    const Error& error() const { return std::get<Error>(data_); }
private:
    std::variant<T, Error> data_;
};

namespace detail {
struct Cancellation { virtual ~Cancellation() = default; virtual void cancel() noexcept = 0; };
template<class T> struct Completion {
    std::promise<Result<T>> promise;
    std::shared_future<Result<T>> future = promise.get_future().share();
    std::mutex mutex;
    bool ready = false, subscribed = false;
    std::function<void()> notification;
    void complete(Result<T> result) {
        std::function<void()> notify;
        {
            std::lock_guard lock(mutex);
            promise.set_value(std::move(result));
            ready = true;
            notify = std::move(notification);
        }
        if (notify) notify();
    }
    void subscribe(std::function<void()> notify) {
        {
            std::lock_guard lock(mutex);
            if (subscribed) throw std::logic_error("request already has a completion notification");
            subscribed = true;
            if (!ready) { notification = std::move(notify); return; }
        }
        notify();
    }
};
struct Access;
} // namespace detail

// Must enqueue promptly on an application-owned executor, never run the task
// inline or wait for its execution. Throw to report a rejected enqueue.
using Executor = std::function<void(std::function<void()>)>;

template<class T> class Request {
public:
    std::shared_future<Result<T>> future() const { return state_->future; }
    Result<T> get() const { return state_->future.get(); } // Blocking convenience; not for event loops.
    void cancel() const noexcept { if (auto call = cancellation_.lock()) call->cancel(); }

    // One notification per operation, shared by copies of this handle. Its
    // receipt reports executor/callback exceptions independently of the RPC result.
    // Tasks already queued on the application's executor may run after shutdown.
    std::future<void> on_complete(Executor post, std::function<void(Result<T>)> callback) const {
        if (!post || !callback) throw std::invalid_argument("executor and callback required");
        struct Delivery { std::atomic<bool> claimed{false}; std::promise<void> receipt; };
        auto delivery = std::make_shared<Delivery>();
        auto receipt = delivery->receipt.get_future();
        state_->subscribe([state = state_, post = std::move(post), callback = std::move(callback), delivery] {
            try {
                post([state, callback, delivery] {
                    if (delivery->claimed.exchange(true)) return;
                    try { callback(state->future.get()); delivery->receipt.set_value(); }
                    catch (...) { delivery->receipt.set_exception(std::current_exception()); }
                });
            } catch (...) {
                if (!delivery->claimed.exchange(true)) delivery->receipt.set_exception(std::current_exception());
            }
        });
        return receipt;
    }
private:
    friend struct detail::Access;
    Request(std::shared_ptr<detail::Completion<T>> state, std::weak_ptr<detail::Cancellation> cancellation)
        : state_(std::move(state)), cancellation_(std::move(cancellation)) {}
    std::shared_ptr<detail::Completion<T>> state_;
    std::weak_ptr<detail::Cancellation> cancellation_;
};

class Ticket {
public:
    Ticket() = default;
    bool valid() const noexcept { return !bytes_.empty(); }
private:
    friend struct detail::Access;
    std::string bytes_, namespace_, endpoint_;
};
struct LoadGrant { Ticket ticket; Milliseconds valid_for{0}; };
enum class LookupKind { Miss, Fresh, Stale };
struct LookupResult {
    LookupKind kind = LookupKind::Miss;
    std::string value; // Empty values are valid hits; use kind.
    Milliseconds fresh_for{0}, servable_for{0}; // Conservative hints, never an invalidation lease.
    std::optional<LoadGrant> load;
};
struct PublishOptions { Milliseconds ttl{0}, swr{0}; std::vector<std::string> tags; };
enum class PublishStatus { Stored, Invalidated, Expired, Consumed, UnknownTicket };
struct Unit {};
struct Health { bool ready; std::string protocol_version, build_version, instance_id; };
struct Stats {
    uint64_t ticket_records, ticket_key_bytes, hits, stale_hits, misses, puts, evictions, expirations;
    uint64_t rpc_active, rpc_accepted, rpc_rejected, rpc_cancelled;
};
struct CallOptions {
    // Absolute monotonic deadline, captured before submitting. Reuse it to bound
    // several cache operations within an application's overall request budget.
    std::optional<Clock::time_point> deadline;
};
struct ClientOptions {
    std::string endpoint, cache_namespace, bearer_token;
    std::string root_certificates; // PEM; empty uses the system trust roots.
    bool insecure_loopback = false; // Explicit development mode; literal loopback only.
    Milliseconds timeout{1000};
    size_t max_in_flight = 64;
    int max_message_bytes = 2 * 1024 * 1024;
};

class Client {
public:
    explicit Client(ClientOptions options);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    Request<LookupResult> lookup(std::string key, bool acquire_ticket = false, CallOptions options = {});
    Request<LoadGrant> begin_load(std::string key, CallOptions options = {});
    Request<PublishStatus> publish(Ticket ticket, std::string value, PublishOptions publish, CallOptions options = {});
    Request<Unit> invalidate_key(std::string key, CallOptions options = {});
    Request<Unit> invalidate_tag(std::string tag, CallOptions options = {});
    Request<Health> health(CallOptions options = {});
    Request<Stats> stats(CallOptions options = {});

    // Thread-safe, repeated/concurrent shutdown cancels and drains network work.
    // Stop external callers before destruction. Executor posting hooks must not
    // invoke shutdown/destruction (they must enqueue, not execute application code).
    void shutdown();
    size_t in_flight() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dbwaller::remote
