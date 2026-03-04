# This standalone benchmark does NOT require Google Benchmark library
# add_executable(dbwaller_bench_throughput throughput_bench.cpp)
# target_link_libraries(dbwaller_bench_throughput PRIVATE dbwaller)
find_package(benchmark CONFIG REQUIRED)

add_executable(dbwaller_bench_throughput
  throughput_bench.cpp
)

target_link_libraries(dbwaller_bench_throughput PRIVATE
  dbwaller
  benchmark::benchmark
  benchmark::benchmark_main
)
