#include <benchmark/benchmark.h>

static void BM_Placeholder(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(42);
    }
}
BENCHMARK(BM_Placeholder);
BENCHMARK_MAIN();
