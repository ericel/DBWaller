#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace dbwaller::concurrency
{

    class ThreadPool
    {
    public:
        using Task = std::function<void(std::stop_token)>;

        explicit ThreadPool(std::size_t num_threads);
        ~ThreadPool();

        ThreadPool(const ThreadPool &) = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;

        // Enqueue work. Task receives a stop_token so it can cooperate with shutdown.
        void submit(Task task);

        std::size_t size() const { return workers_.size(); }

    private:
        void worker_loop(std::stop_token st);

        mutable std::mutex mu_;
        std::condition_variable_any cv_;
        std::deque<Task> q_;
        std::vector<std::jthread> workers_;
    };

} // namespace dbwaller::concurrency