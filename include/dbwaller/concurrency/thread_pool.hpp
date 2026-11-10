#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace dbwaller::concurrency {

/**
 * ThreadPool (C++20)
 * - Uses std::jthread + std::stop_token for graceful shutdown
 * - Optional max queue size (backpressure)
 * - Can enqueue fire-and-forget tasks or tasks returning futures
 */
class ThreadPool {
public:
    using Task = std::function<void(std::stop_token)>;

    // max_queue = 0 means unbounded.
    explicit ThreadPool(std::size_t num_threads, std::size_t max_queue = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Returns false if the queue is full or shutdown has closed admission.
    bool try_submit(Task task);

    // Throws std::runtime_error if the queue is full or admission is closed.
    void submit_or_throw(Task task);

    // Returns nullopt if the queue is full or admission is closed.
    template <typename Fn>
    auto try_submit_future(Fn&& fn)
        -> std::optional<std::future<std::invoke_result_t<Fn>>> {
        using R = std::invoke_result_t<Fn>;

        // Wrap the callable in a packaged_task
        auto pt = std::make_shared<std::packaged_task<R()>>(std::forward<Fn>(fn));
        std::future<R> fut = pt->get_future();

        // Our worker signature expects (stop_token). Ignore it inside.
        Task t = [pt](std::stop_token) mutable {
            (*pt)();
        };

        if (!try_submit(std::move(t))) {
            return std::nullopt;
        }
        return fut;
    }

    std::size_t size() const { std::lock_guard lock(mu_); return workers_.size(); }
    std::size_t max_queue() const { return max_queue_; }

    // Close admission, request cooperative stop, drain accepted work, and join.
    // Concurrent/repeated calls wait for the same completion. Callbacks must
    // eventually return; calling shutdown from this pool's task is unsupported.
    void shutdown();

private:
    void worker_loop(std::stop_token st);

    mutable std::mutex mu_;
    std::condition_variable_any cv_;
    std::deque<Task> q_;

    std::mutex shutdown_mu_;
    bool closing_ = false; // guarded by mu_
    std::vector<std::jthread> workers_;
    std::size_t max_queue_ = 0;
};

} // namespace dbwaller::concurrency
