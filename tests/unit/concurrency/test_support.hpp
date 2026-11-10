#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace lifecycle_test {
using namespace std::chrono_literals;
inline constexpr auto watchdog = 10s;

class Signal {
public:
    void set() {
        { std::lock_guard lock(mu_); ready_ = true; }
        cv_.notify_all();
    }
    bool wait() {
        std::unique_lock lock(mu_);
        return cv_.wait_for(lock, watchdog, [&] { return ready_; });
    }
private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool ready_ = false;
};

struct Gate {
    Signal entered;
    Signal release;
    void block() {
        entered.set();
        if (!release.wait()) throw std::runtime_error("callback release watchdog expired");
    }
};

struct ReleaseOnExit {
    Signal& signal;
    ~ReleaseOnExit() { signal.set(); }
};

template <class Predicate>
bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + watchdog;
    do {
        if (predicate()) return true;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

template <class T>
bool ready(std::future<T>& future) {
    return future.wait_for(watchdog) == std::future_status::ready;
}
} // namespace lifecycle_test
