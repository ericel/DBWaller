#include "dbwaller/concurrency/thread_pool.hpp"

#include <stdexcept>

namespace dbwaller::concurrency {

ThreadPool::ThreadPool(std::size_t num_threads, std::size_t max_queue)
    : max_queue_(max_queue) {
    if (num_threads == 0) num_threads = 1;
    workers_.reserve(num_threads);

    for (std::size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this](std::stop_token st) { worker_loop(st); });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    // Only one caller may touch the jthreads while joining. Admission and queue
    // access use a separate mutex, which is never held while executing user code.
    std::lock_guard shutdown_lock(shutdown_mu_);
    {
        std::lock_guard lock(mu_);
        closing_ = true;
    }
    for (auto& worker : workers_) {
        worker.request_stop();
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    // Synchronize size() with worker cleanup as well as concurrent shutdowns.
    std::lock_guard lock(mu_);
    workers_.clear();
}

bool ThreadPool::try_submit(Task task) {
    {
        std::lock_guard<std::mutex> lk(mu_);

        if (closing_ || (max_queue_ > 0 && q_.size() >= max_queue_)) {
            return false;
        }

        q_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::submit_or_throw(Task task) {
    if (!try_submit(std::move(task))) {
        throw std::runtime_error("ThreadPool queue is full or shutdown has started");
    }
}

void ThreadPool::worker_loop(std::stop_token st) {
    for (;;) {
        Task task;

        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, st, [&] { return closing_ || !q_.empty(); });

            if (q_.empty()) {
                if (closing_ || st.stop_requested()) return;
                continue;
            }

            task = std::move(q_.front());
            q_.pop_front();
        }

        // Accepted tasks drain even with stop requested. Packaged tasks publish
        // exceptions through their futures; a fire-and-forget exception must not
        // terminate the worker and strand the rest of the queue.
        try { task(st); } catch (...) {}
    }
}

} // namespace dbwaller::concurrency

// #include "dbwaller/concurrency/thread_pool.hpp"

// #include <stdexcept>

// namespace dbwaller::concurrency {

// ThreadPool::ThreadPool(std::size_t num_threads, std::size_t max_queue)
//     : max_queue_(max_queue) {
//     if (num_threads == 0) num_threads = 1;
//     workers_.reserve(num_threads);

//     for (std::size_t i = 0; i < num_threads; ++i) {
//         workers_.emplace_back([this](std::stop_token st) { worker_loop(st); });
//     }
// }

// ThreadPool::~ThreadPool() {
//     // std::jthread requests stop automatically when destroyed.
//     // Wake all workers so they can observe stop and exit promptly.
//     cv_.notify_all();
// }

// bool ThreadPool::try_submit(Task task) {
//     {
//         std::lock_guard<std::mutex> lk(mu_);

//         // Backpressure: reject if queue is full (if bounded)
//         if (max_queue_ > 0 && q_.size() >= max_queue_) {
//             return false;
//         }

//         q_.push_back(std::move(task));
//     }
//     cv_.notify_one();
//     return true;
// }

// void ThreadPool::submit_or_throw(Task task) {
//     if (!try_submit(std::move(task))) {
//         throw std::runtime_error("ThreadPool queue is full (backpressure)");
//     }
// }

// void ThreadPool::worker_loop(std::stop_token st) {
//     while (!st.stop_requested()) {
//         Task task;

//         {
//             std::unique_lock<std::mutex> lk(mu_);
//             cv_.wait(lk, st, [&] { return st.stop_requested() || !q_.empty(); });

//             if (st.stop_requested()) return;
//             if (q_.empty()) continue;

//             task = std::move(q_.front());
//             q_.pop_front();
//         }

//         // Execute outside lock
//         task(st);
//     }
// }

// } // namespace dbwaller::concurrency
