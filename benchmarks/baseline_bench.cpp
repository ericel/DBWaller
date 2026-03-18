#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class GlobalMutexCache {
public:
    explicit GlobalMutexCache(std::vector<std::string> ids) : ids_(std::move(ids)) {}

    void preload() {
        std::unique_lock<std::shared_mutex> lock(mu_);
        for (const auto& id : ids_) {
            data_[id] = make_value(id);
        }
    }

    std::string get(const std::string& id) {
        {
            std::shared_lock<std::shared_mutex> lock(mu_);
            auto it = data_.find(id);
            if (it != data_.end()) {
                return it->second;
            }
        }

        std::unique_lock<std::shared_mutex> lock(mu_);
        auto [it, inserted] = data_.emplace(id, make_value(id));
        if (!inserted && it->second.empty()) {
            it->second = make_value(id);
        }
        return it->second;
    }

    void put(const std::string& id) {
        std::unique_lock<std::shared_mutex> lock(mu_);
        data_[id] = make_value(id);
    }

private:
    static std::string make_value(const std::string& id) {
        return "PAYLOAD_FOR_" + id;
    }

    std::shared_mutex mu_;
    std::unordered_map<std::string, std::string> data_;
    std::vector<std::string> ids_;
};

struct BaselineBenchEnv {
    using Clock = std::chrono::steady_clock;

    int keys_count;
    int total_ops;
    int write_percent;
    int key_skew;

    std::vector<std::string> ids;
    GlobalMutexCache cache;

    std::barrier<> start_barrier;
    std::barrier<> end_barrier;

    std::atomic<int64_t> start_ns{0};
    std::atomic<int64_t> end_ns{0};
    std::atomic<int64_t> read_ops{0};
    std::atomic<int64_t> write_ops{0};

    BaselineBenchEnv(int threads, int keys_count_, int total_ops_, int write_percent_, int key_skew_)
        : keys_count(keys_count_),
          total_ops(total_ops_),
          write_percent(write_percent_),
          key_skew(key_skew_),
          ids(build_ids(keys_count_)),
          cache(ids),
          start_barrier(threads),
          end_barrier(threads) {
        cache.preload();
    }

    static std::vector<std::string> build_ids(int keys_count) {
        std::vector<std::string> out;
        out.reserve(static_cast<size_t>(keys_count));
        for (int i = 0; i < keys_count; ++i) {
            out.push_back(std::to_string(i));
        }
        return out;
    }
};

static std::atomic<BaselineBenchEnv*> g_env{nullptr};

static BaselineBenchEnv* wait_env() {
    BaselineBenchEnv* env = nullptr;
    while ((env = g_env.load(std::memory_order_acquire)) == nullptr) {
        std::this_thread::yield();
    }
    return env;
}

static int pick_key_index(uint32_t rnd, int keys_count, int key_skew) {
    if (keys_count <= 1) {
        return 0;
    }
    if (key_skew == 0) {
        return static_cast<int>(rnd % static_cast<uint32_t>(keys_count));
    }

    const bool pick_hot = (rnd % 100) < 80;
    const int hot_keys = std::max(1, keys_count / 5);
    if (pick_hot) {
        return static_cast<int>(rnd % static_cast<uint32_t>(hot_keys));
    }
    const int cold_keys = std::max(1, keys_count - hot_keys);
    return hot_keys + static_cast<int>(rnd % static_cast<uint32_t>(cold_keys));
}

static void BM_GlobalMutex_Mixed(benchmark::State& state) {
    const int shards = static_cast<int>(state.range(0)); // retained for matrix parity with sharded bench
    const int keys_count = static_cast<int>(state.range(1));
    const int total_ops = static_cast<int>(state.range(2));
    const int write_percent = static_cast<int>(state.range(3));
    const int key_skew = static_cast<int>(state.range(4));
    const int threads = static_cast<int>(state.threads());

    if (state.thread_index() == 0) {
        auto* env = new BaselineBenchEnv(threads, keys_count, total_ops, write_percent, key_skew);
        g_env.store(env, std::memory_order_release);
    }

    BaselineBenchEnv* env = wait_env();

    for (auto _ : state) {
        const int my_ops = (total_ops + threads - 1) / threads;
        int64_t local_reads = 0;
        int64_t local_writes = 0;

        uint64_t x = 0xD2B74407B1CE6E93ull ^ (static_cast<uint64_t>(state.thread_index()) + 1);
        auto next_u32 = [&]() -> uint32_t {
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            return static_cast<uint32_t>((x * 2685821657736338717ull) >> 32);
        };

        env->start_barrier.arrive_and_wait();

        if (state.thread_index() == 0) {
            const auto t0 = BaselineBenchEnv::Clock::now().time_since_epoch();
            env->read_ops.store(0, std::memory_order_release);
            env->write_ops.store(0, std::memory_order_release);
            env->start_ns.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t0).count(),
                std::memory_order_release
            );
        }

        for (int i = 0; i < my_ops; ++i) {
            const uint32_t rnd = next_u32();
            const int idx = pick_key_index(rnd, keys_count, key_skew);
            const bool is_write = (rnd % 100) < static_cast<uint32_t>(write_percent);

            if (is_write) {
                env->cache.put(env->ids[idx]);
                ++local_writes;
            } else {
                auto value = env->cache.get(env->ids[idx]);
                benchmark::DoNotOptimize(value);
                ++local_reads;
            }
        }

        env->read_ops.fetch_add(local_reads, std::memory_order_relaxed);
        env->write_ops.fetch_add(local_writes, std::memory_order_relaxed);

        env->end_barrier.arrive_and_wait();

        if (state.thread_index() == 0) {
            const auto t1 = BaselineBenchEnv::Clock::now().time_since_epoch();
            env->end_ns.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1).count(),
                std::memory_order_release
            );

            const int64_t start_ns = env->start_ns.load(std::memory_order_acquire);
            const int64_t end_ns = env->end_ns.load(std::memory_order_acquire);
            const double sec = static_cast<double>(end_ns - start_ns) / 1e9;

            state.SetIterationTime(sec);
            state.counters["impl"] = 0;
            state.counters["keys"] = keys_count;
            state.counters["ops"] = total_ops;
            state.counters["read_percent"] = 100 - write_percent;
            state.counters["write_percent"] = write_percent;
            state.counters["skew_mode"] = key_skew;
            state.counters["shards"] = shards;
            state.counters["threads"] = threads;
            state.counters["reads"] = static_cast<double>(env->read_ops.load(std::memory_order_acquire));
            state.counters["writes"] = static_cast<double>(env->write_ops.load(std::memory_order_acquire));
            state.counters["baseline_ops_per_sec"] = benchmark::Counter(total_ops, benchmark::Counter::kIsRate);
        }
    }

    if (state.thread_index() == 0) {
        auto* env_to_delete = g_env.exchange(nullptr, std::memory_order_acq_rel);
        delete env_to_delete;
    }
}

static void ApplyCommonMatrix(benchmark::internal::Benchmark* b) {
    constexpr int kKeys = 100'000;
    constexpr int kOps = 1'000'000;
    const int shards[] = {1, 8, 16, 32, 64};
    const int write_pct[] = {0, 5, 20, 50};
    const int skew_mode[] = {0, 1};

    for (int s : shards) {
        for (int w : write_pct) {
            for (int skew : skew_mode) {
                b->Args({s, kKeys, kOps, w, skew});
            }
        }
    }
}

BENCHMARK(BM_GlobalMutex_Mixed)
    ->Apply(ApplyCommonMatrix)
    ->ThreadRange(1, 32)
    ->UseManualTime()
    ->Iterations(1)
    ->Repetitions(5);
