#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <iostream>
#include <thread>
#include <chrono>

#include "dbwaller/adapters/adapter.hpp"

/**
 * FakeAdapter:
 * - In-memory authoritative source for demos/tests.
 * - Thread-safe.
 * - Supports artificial latency (fetch_delay_ms) for concurrency testing.
 * - Supports failure injection.
 */
class FakeAdapter final : public dbwaller::adapters::Adapter {
public:
    // Knobs for testing
    bool verbose = true;
    std::atomic<uint64_t> fetch_delay_ms{0}; // Artificial latency to force overlapping requests

    // Backwards compatibility for embedded_demo
    bool fail_next_timeline_fetch = false; 

    // Helper for demos/tests to update the authoritative store.
    void set_value(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lk(mu_);
        store_[key] = value;
    }

    // Helper to get raw value
    std::optional<std::string> get_value(const std::string& key) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = store_.find(key);
        if (it == store_.end()) return std::nullopt;
        return it->second;
    }

    void reset_fetch_count() {
        fetch_count_.store(0, std::memory_order_relaxed);
    }

    uint64_t fetch_count() const {
        return fetch_count_.load(std::memory_order_relaxed);
    }

    // Schedule a one-time failure for a specific key (Generic test support)
    void fail_next_for(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu_);
        next_failures_.insert(key);
    }

    std::optional<dbwaller::adapters::FetchResult> fetch_one(
        const std::string& key,
        const dbwaller::adapters::RequestContext& /*ctx*/
    ) override {
        // 1. Artificial Delay (Critical for Coalescing Tests)
        // If we don't sleep, the refresh finishes before other threads can join the queue.
        uint64_t delay = fetch_delay_ms.load(std::memory_order_relaxed);
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }

        // 2. Failure Injection
        bool should_fail = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            
            // Check generic failures
            auto it = next_failures_.find(key);
            if (it != next_failures_.end()) {
                next_failures_.erase(it);
                should_fail = true;
            }
            
            // Check legacy timeline failure (for compatibility)
            if (!should_fail && fail_next_timeline_fetch && is_timeline_key_(key)) {
                fail_next_timeline_fetch = false;
                should_fail = true;
            }
        }

        if (should_fail) {
            if (verbose) std::cout << "[FakeAdapter] fetch_one(" << key << ") -> <forced error>\n";
            return std::nullopt; // Simulate error (Adapter returns empty optional on failure)
        }

        // 3. Data Fetch
        std::string value;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = store_.find(key);
            if (it == store_.end()) {
                if (verbose) std::cout << "[FakeAdapter] fetch_one(" << key << ") -> <miss>\n";
                return std::nullopt;
            }
            value = it->second;
        }

        const auto n = fetch_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (verbose) {
            std::cout << "[FakeAdapter] fetch_one(" << key << ") count=" << n << " -> " << value << "\n";
        }

        dbwaller::adapters::FetchResult r;
        r.value = std::move(value);
        return r;
    }

private:
    static bool is_timeline_key_(const std::string& key) {
        return key.rfind("timeline:", 0) == 0;
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> store_;
    std::unordered_set<std::string> next_failures_;
    std::atomic<uint64_t> fetch_count_{0};
};