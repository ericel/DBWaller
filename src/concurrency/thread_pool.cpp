#include "dbwaller/concurrency/thread_pool.hpp"

#include <stop_token>
#include <thread> 

namespace dbwaller::concurrency
{

    ThreadPool::ThreadPool(std::size_t num_threads)
    {
        workers_.reserve(num_threads);

        for (std::size_t i = 0; i < num_threads; ++i)
        {
            workers_.emplace_back([this](std::stop_token st)
                                  { worker_loop(st); });
        }
    }

    ThreadPool::~ThreadPool()
    {
        // std::jthread requests stop automatically when destroyed.
        // Wake all workers to exit promptly.
        cv_.notify_all();
    }

    void ThreadPool::submit(Task task)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    void ThreadPool::worker_loop(std::stop_token st)
    {
        while (!st.stop_requested())
        {
            Task task;

            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, st, [&]
                         { return st.stop_requested() || !q_.empty(); });

                if (st.stop_requested())
                    return;
                if (q_.empty())
                    continue;

                task = std::move(q_.front());
                q_.pop_front();
            }

            // Execute outside lock
            task(st);
        }
    }

} // namespace dbwaller::concurrency