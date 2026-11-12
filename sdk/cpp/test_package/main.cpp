#include <dbwaller/remote/client.hpp>
// The consumer never includes generated gRPC headers or the embedded library.
int main() {
    namespace r = dbwaller::remote;
    r::ClientOptions options;
    options.endpoint = "127.0.0.1:1"; options.cache_namespace = "consumer";
    options.bearer_token = std::string(64, 'x'); options.insecure_loopback = true;
    r::Client client(std::move(options));
    auto expired = client.lookup("post", false, {r::Clock::now()}).get();
    if (expired || expired.error().code != r::ErrorCode::DeadlineExceeded) return 1;
    client.shutdown();
    auto closed = client.begin_load("post").get();
    return !closed && closed.error().code == r::ErrorCode::ClientClosed ? 0 : 1;
}
