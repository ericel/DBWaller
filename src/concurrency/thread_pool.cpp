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
    // std::jthread requests stop automatically when destroyed.
    // Wake all workers so they can observe stop and exit promptly.
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Signal stop (though jthreads do this via stop_token, 
        // notifying CV is critical to wake sleeping workers)
    }
    cv_.notify_all();

    // Explicitly clear workers if needed, or let jthread destructors handle join
    // Since jthreads join on destruction, we just need to ensure they wake up.
    // However, if you want explicit early shutdown before destruction:
    for (auto& worker : workers_) {
        worker.request_stop();
    }
    cv_.notify_all();
    
    // Note: Since you are using std::jthread, they will join automatically 
    // when the vector is cleared or the pool is destroyed.
    // But to force a sync point now:
    workers_.clear(); 
}

bool ThreadPool::try_submit(Task task) {
    {
        std::lock_guard<std::mutex> lk(mu_);

        // Backpressure: reject if queue is full (if bounded)
        if (max_queue_ > 0 && q_.size() >= max_queue_) {
            return false;
        }

        q_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::submit_or_throw(Task task) {
    if (!try_submit(std::move(task))) {
        throw std::runtime_error("ThreadPool queue is full (backpressure)");
    }
}

void ThreadPool::worker_loop(std::stop_token st) {
    while (!st.stop_requested()) {
        Task task;

        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, st, [&] { return st.stop_requested() || !q_.empty(); });

            if (st.stop_requested()) return;
            if (q_.empty()) continue;

            task = std::move(q_.front());
            q_.pop_front();
        }

        // Execute outside lock
        task(st);
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
