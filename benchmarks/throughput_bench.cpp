#include <benchmark/benchmark.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "dbwaller/adapters/adapter.hpp"
#include "dbwaller/adapters/request_context.hpp"
#include "dbwaller/core/gateway.hpp"
#include "dbwaller/core/sharded_engine.hpp"
#include "dbwaller/policy/rules.hpp"

// -----------------------------
// Fast adapter (authoritative source)
// -----------------------------
class FastAdapter : public dbwaller::adapters::Adapter {
public:
    std::optional<dbwaller::adapters::FetchResult> fetch_one(
        const std::string& key,
        const dbwaller::adapters::RequestContext&
    ) override {
        dbwaller::adapters::FetchResult r;
        r.value = "PAYLOAD_FOR_" + key;
        r.ttl_ms = 60'000; // not used by Gateway caching (Gateway uses policy PutOptions), but fine
        return r;
    }
};

// -----------------------------
// Shared environment for a single benchmark run (per process invocation)
// We create it once per benchmark invocation and share across threads.
// -----------------------------
struct BenchEnv {
    using Clock = std::chrono::steady_clock;

    // Config
    int shards = 64;
    int keys_count = 100'000;
    int total_ops = 1'000'000;

    // DBWaller
    dbwaller::core::ShardedEngine engine;
    dbwaller::core::Gateway waller;
    FastAdapter adapter;

    // Policy
    dbwaller::policy::PolicyRuleSet rules;
    dbwaller::adapters::RequestContext ctx;

    // Keys (prebuilt strings, so we don't measure string building too much)
    std::vector<std::string> ids;

    // Barriers for synchronized timing
    std::barrier<> start_barrier;
    std::barrier<> end_barrier;

    // Timing for manual time
    std::atomic<int64_t> start_ns{0};
    std::atomic<int64_t> end_ns{0};

    BenchEnv(int threads, int shards_, int keys_count_, int total_ops_)
        : shards(shards_),
          keys_count(keys_count_),
          total_ops(total_ops_),
          engine(make_engine_cfg_(shards_)),
          waller(engine),
          start_barrier(threads),
          end_barrier(threads) {
        // Policy: Public, long TTL so hot reads stay hits
        dbwaller::policy::Rule r;
        r.scope = dbwaller::policy::CacheScope::Public;
        r.ttl_ms = 60'000; // 60s
        r.swr_ms = 0;
        r.stale_ttl_ms = 0;
        r.serve_stale_on_error = false;
        rules.set_default(r);

        // Context
        ctx.tenant = "bench";
        ctx.viewer_id = "anon";
        ctx.locale = "en";
        dbwaller::adapters::set_claims_fingerprint(ctx, {"user"}, {"read"});

        // Prebuild ids [0..keys_count-1]
        ids.reserve(static_cast<size_t>(keys_count));
        for (int i = 0; i < keys_count; ++i) {
            ids.push_back(std::to_string(i));
        }
    }

    static dbwaller::core::ShardedEngine::Config make_engine_cfg_(int shards) {
        dbwaller::core::ShardedEngine::Config cfg;
        cfg.num_shards = shards;
        cfg.max_bytes_total = 512ull * 1024ull * 1024ull; // 512MB
        cfg.enable_compute_pool = true;
        cfg.compute_threads = std::max(2, static_cast<int>(std::thread::hardware_concurrency() / 2));
        cfg.compute_max_queue = 1024;
        cfg.compute_timeout_ms = 1500;
        cfg.backpressure = dbwaller::core::ShardedEngine::BackpressureMode::RunInline;
        cfg.sweep_interval_ms = 200;
        return cfg;
    }

    // Helper to populate the cache (miss->fetch->put) for all keys
    void populate_all_keys() {
        for (int i = 0; i < keys_count; ++i) {
            (void)waller.get_or_fetch_object_ruled(rules, "bench", ids[i], "get", ctx, adapter);
        }
    }
};

// Global env pointer used for “one env per benchmark invocation”
static std::atomic<BenchEnv*> g_env{nullptr};

// Spin until env exists (short and only during setup barrier)
static BenchEnv* wait_env() {
    BenchEnv* env = nullptr;
    while ((env = g_env.load(std::memory_order_acquire)) == nullptr) {
        std::this_thread::yield();
    }
    return env;
}

// -----------------------------
// BENCH 1: Populate throughput (miss-heavy warmup)
// Measures: time to do KEYS_COUNT miss->fetch->put operations across threads.
// Args: [shards, keys_count]
// -----------------------------
static void BM_DBWaller_Populate(benchmark::State& state) {
    const int shards = static_cast<int>(state.range(0));
    const int keys_count = static_cast<int>(state.range(1));
    const int threads = static_cast<int>(state.threads());

    // Create env once (thread 0), share across all threads.
    if (state.thread_index() == 0) {
        auto* env = new BenchEnv(threads, shards, keys_count, /*total_ops*/ keys_count);
        g_env.store(env, std::memory_order_release);
    }

    BenchEnv* env = wait_env();

    // We do exactly one timed “phase” per iteration (manual time).
    for (auto _ : state) {
        // Partition keys across threads
        const int per = (keys_count + threads - 1) / threads;
        const int begin = static_cast<int>(state.thread_index()) * per;
        const int end = std::min(keys_count, begin + per);

        // Sync start
        env->start_barrier.arrive_and_wait();

        if (state.thread_index() == 0) {
            const auto t0 = BenchEnv::Clock::now().time_since_epoch();
            env->start_ns.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t0).count(),
                std::memory_order_release
            );
        }

        // Work
        for (int i = begin; i < end; ++i) {
            auto res = env->waller.get_or_fetch_object_ruled(
                env->rules, "bench", env->ids[i], "get", env->ctx, env->adapter
            );
            benchmark::DoNotOptimize(res);
        }

        // Sync end
        env->end_barrier.arrive_and_wait();

        if (state.thread_index() == 0) {
            const auto t1 = BenchEnv::Clock::now().time_since_epoch();
            env->end_ns.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1).count(),
                std::memory_order_release
            );

            const int64_t start_ns = env->start_ns.load(std::memory_order_acquire);
            const int64_t end_ns = env->end_ns.load(std::memory_order_acquire);
            const double sec = static_cast<double>(end_ns - start_ns) / 1e9;

            // Tell Google Benchmark the exact timing for this iteration
            state.SetIterationTime(sec);

            // Useful counters
            state.counters["keys"] = keys_count;
            state.counters["shards"] = shards;
            state.counters["threads"] = threads;
            state.counters["populate_ops_per_sec"] =
                benchmark::Counter(keys_count, benchmark::Counter::kIsRate);
        }
    }

    // Cleanup once (thread 0)
    if (state.thread_index() == 0) {
        auto* env_to_delete = g_env.exchange(nullptr, std::memory_order_acq_rel);
        delete env_to_delete;
    }
}

// -----------------------------
// BENCH 2: Hot read throughput (hit-heavy)
// Measures: time for TOTAL_OPS reads across threads, from a pre-populated cache.
// Args: [shards, keys_count, total_ops]
// -----------------------------
static void BM_DBWaller_HotReads(benchmark::State& state) {
    const int shards = static_cast<int>(state.range(0));
    const int keys_count = static_cast<int>(state.range(1));
    const int total_ops = static_cast<int>(state.range(2));
    const int threads = static_cast<int>(state.threads());

    if (state.thread_index() == 0) {
        auto* env = new BenchEnv(threads, shards, keys_count, total_ops);

        // Pre-populate cache BEFORE timing
        env->populate_all_keys();

        g_env.store(env, std::memory_order_release);
    }

    BenchEnv* env = wait_env();

    for (auto _ : state) {
        const int ops_per_thread = (total_ops + threads - 1) / threads;
        const int my_ops = ops_per_thread;

        // Fast per-thread RNG (minimal overhead)
        uint64_t x = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(state.thread_index()) + 1);
        auto next_u32 = [&]() -> uint32_t {
            // xorshift64*
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            return static_cast<uint32_t>((x * 2685821657736338717ull) >> 32);
        };

        env->start_barrier.arrive_and_wait();

        if (state.thread_index() == 0) {
            const auto t0 = BenchEnv::Clock::now().time_since_epoch();
            env->start_ns.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t0).count(),
                std::memory_order_release
            );
        }

        for (int i = 0; i < my_ops; ++i) {
            const int idx = static_cast<int>(next_u32() % static_cast<uint32_t>(keys_count));
            auto res = env->waller.get_or_fetch_object_ruled(
                env->rules, "bench", env->ids[idx], "get", env->ctx, env->adapter
            );
            benchmark::DoNotOptimize(res);
        }

        env->end_barrier.arrive_and_wait();

        if (state.thread_index() == 0) {
            const auto t1 = BenchEnv::Clock::now().time_since_epoch();
            env->end_ns.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1).count(),
                std::memory_order_release
            );

            const int64_t start_ns = env->start_ns.load(std::memory_order_acquire);
            const int64_t end_ns = env->end_ns.load(std::memory_order_acquire);
            const double sec = static_cast<double>(end_ns - start_ns) / 1e9;

            state.SetIterationTime(sec);

            state.counters["keys"] = keys_count;
            state.counters["ops"] = total_ops;
            state.counters["shards"] = shards;
            state.counters["threads"] = threads;
            state.counters["hotread_ops_per_sec"] =
                benchmark::Counter(total_ops, benchmark::Counter::kIsRate);
        }
    }

    if (state.thread_index() == 0) {
        auto* env_to_delete = g_env.exchange(nullptr, std::memory_order_acq_rel);
        delete env_to_delete;
    }
}

// -----------------------------
// Registration
// -----------------------------
BENCHMARK(BM_DBWaller_Populate)
    ->Args({64, 100000})              // shards, keys_count
    ->Threads(8)
    ->UseManualTime()
    ->Iterations(1)
    ->Repetitions(3);

BENCHMARK(BM_DBWaller_HotReads)
    ->Args({64, 100000, 1000000})     // shards, keys_count, total_ops
    ->Threads(8)
    ->UseManualTime()
    ->Iterations(1)
    ->Repetitions(3);

// No BENCHMARK_MAIN() here if you link benchmark::benchmark_main in CMake


// #include <atomic>
// #include <chrono>
// #include <iomanip>
// #include <iostream>
// #include <mutex>
// #include <random>
// #include <string>
// #include <thread>
// #include <vector>
// #include <barrier>

// #include "dbwaller/adapters/adapter.hpp"
// #include "dbwaller/adapters/request_context.hpp"
// #include "dbwaller/core/gateway.hpp"
// #include "dbwaller/core/sharded_engine.hpp"
// #include "dbwaller/policy/rules.hpp"

// // -----------------------------
// // Fast adapter (authoritative source)
// // -----------------------------
// class FastAdapter : public dbwaller::adapters::Adapter {
// public:
//     std::optional<dbwaller::adapters::FetchResult> fetch_one(
//         const std::string& key,
//         const dbwaller::adapters::RequestContext&
//     ) override {
//         dbwaller::adapters::FetchResult r;
//         r.value = "PAYLOAD_FOR_" + key;
//         r.ttl_ms = 60'000;
//         return r;
//     }
// };

// struct BenchEnv {
//     int shards;
//     int keys_count;
    
//     dbwaller::core::ShardedEngine engine;
//     dbwaller::core::Gateway waller;
//     FastAdapter adapter;
//     dbwaller::policy::PolicyRuleSet rules;
//     dbwaller::adapters::RequestContext ctx;
//     std::vector<std::string> ids;

//     BenchEnv(int shards_, int keys_count_)
//         : shards(shards_),
//           keys_count(keys_count_),
//           engine(make_cfg(shards_)),
//           waller(engine) {
        
//         dbwaller::policy::Rule r;
//         r.scope = dbwaller::policy::CacheScope::Public;
//         r.ttl_ms = 60'000;
//         rules.set_default(r);

//         ctx.tenant = "bench";
//         ctx.viewer_id = "anon";
//         dbwaller::adapters::set_claims_fingerprint(ctx, {"user"}, {"read"});

//         ids.reserve(keys_count);
//         for(int i=0; i<keys_count; ++i) ids.push_back(std::to_string(i));
//     }

//     static dbwaller::core::ShardedEngine::Config make_cfg(int shards) {
//         dbwaller::core::ShardedEngine::Config cfg;
//         cfg.num_shards = shards;
//         cfg.max_bytes_total = 512ull * 1024ull * 1024ull;
//         cfg.enable_compute_pool = true;
//         cfg.compute_threads = 4;
//         cfg.backpressure = dbwaller::core::ShardedEngine::BackpressureMode::RunInline;
//         return cfg;
//     }
// };

// void print_result(std::string name, int threads, int count, double duration_sec) {
//     double rps = count / duration_sec;
//     double lat_us = (duration_sec * 1e6) / count;
//     std::cout << std::left << std::setw(20) << name 
//               << "| Threads: " << std::setw(2) << threads
//               << "| Ops: " << std::setw(8) << count
//               << "| Time: " << std::fixed << std::setprecision(3) << duration_sec << "s"
//               << "| RPS: " << std::setprecision(0) << rps
//               << "| Lat: " << std::setprecision(2) << lat_us << " us\n";
// }

// int main() {
//     const int SHARDS = 64;
//     const int KEYS = 100'000;
//     const int THREADS = 8;
//     const int OPS = 1'000'000;

//     std::cout << "--- DBWALLER THROUGHPUT BENCHMARK ---\n";
//     std::cout << "Shards: " << SHARDS << ", Keys: " << KEYS << "\n\n";

//     BenchEnv env(SHARDS, KEYS);

//     // -------------------------------------------------
//     // BENCH 1: POPULATE (Write Throughput)
//     // -------------------------------------------------
//     {
//         std::vector<std::thread> workers;
//         std::barrier sync_point(THREADS);
//         std::atomic<int64_t> start_ns{0}, end_ns{0};

//         for(int t=0; t<THREADS; ++t) {
//             workers.emplace_back([&, t]() {
//                 int start_idx = (KEYS / THREADS) * t;
//                 int end_idx = std::min(KEYS, start_idx + (KEYS / THREADS));

//                 sync_point.arrive_and_wait();
//                 if (t == 0) start_ns = std::chrono::steady_clock::now().time_since_epoch().count();

//                 for(int i=start_idx; i<end_idx; ++i) {
//                     env.waller.get_or_fetch_object_ruled(env.rules, "bench", env.ids[i], "get", env.ctx, env.adapter);
//                 }

//                 sync_point.arrive_and_wait();
//                 if (t == 0) end_ns = std::chrono::steady_clock::now().time_since_epoch().count();
//             });
//         }
//         for(auto& w : workers) w.join();
//         print_result("Populate (Writes)", THREADS, KEYS, (end_ns - start_ns) / 1e9);
//     }

//     // -------------------------------------------------
//     // BENCH 2: HOT READS (Read Throughput)
//     // -------------------------------------------------
//     {
//         std::vector<std::thread> workers;
//         std::barrier sync_point(THREADS);
//         std::atomic<int64_t> start_ns{0}, end_ns{0};

//         for(int t=0; t<THREADS; ++t) {
//             workers.emplace_back([&, t]() {
//                 int ops_per_thread = OPS / THREADS;
                
//                 // Fast PRNG
//                 std::mt19937 rng(t);
//                 std::uniform_int_distribution<int> dist(0, KEYS - 1);

//                 sync_point.arrive_and_wait();
//                 if (t == 0) start_ns = std::chrono::steady_clock::now().time_since_epoch().count();

//                 for(int i=0; i<ops_per_thread; ++i) {
//                     int idx = dist(rng);
//                     auto res = env.waller.get_or_fetch_object_ruled(env.rules, "bench", env.ids[idx], "get", env.ctx, env.adapter);
//                     if (res->empty()) std::terminate(); // Prevent optimization
//                 }

//                 sync_point.arrive_and_wait();
//                 if (t == 0) end_ns = std::chrono::steady_clock::now().time_since_epoch().count();
//             });
//         }
//         for(auto& w : workers) w.join();
//         print_result("Hot Reads (Hits)", THREADS, OPS, (end_ns - start_ns) / 1e9);
//     }

//     return 0;
// }