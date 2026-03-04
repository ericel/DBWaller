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

    // Try to enqueue a task. Returns false if queue is full (backpressure).
    bool try_submit(Task task);

    // Convenience: throws std::runtime_error if queue is full.
    void submit_or_throw(Task task);

    // Enqueue a task that returns a value. Returns nullopt if queue is full.
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

    std::size_t size() const { return workers_.size(); }
    std::size_t max_queue() const { return max_queue_; }

    // Graceful shutdown
    void shutdown();

private:
    void worker_loop(std::stop_token st);

    mutable std::mutex mu_;
    std::condition_variable_any cv_;
    std::deque<Task> q_;

    std::vector<std::jthread> workers_;
    std::size_t max_queue_ = 0;
};

} // namespace dbwaller::concurrency
