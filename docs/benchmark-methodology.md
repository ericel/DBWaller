# DBWaller Benchmark Methodology

## Purpose
This document defines a reproducible way to run and compare baseline versus sharded DBWaller benchmarks.

## Required Binaries
- dbwaller_bench_baseline
- dbwaller_bench_throughput

Build with benchmarks enabled, then run the automation script.

Build commands:
conan profile detect --force
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20 \
  -o '&:with_tests=False' -o '&:with_benchmarks=True' -o '&:build_apps=False' \
  -c tools.cmake.cmaketoolchain:extra_variables="{'GIT_EXECUTABLE':'/usr/bin/git'}"
cmake --preset conan-release-benchmarks
cmake --build --preset build-conan-release-benchmarks

The `GIT_EXECUTABLE` override is required on environments where Git.app is resolved first and returns an empty `git describe` output for `benchmark/1.9.1`.

## Runner
- Script: scripts/run_benchmarks.sh
- Default output root: results/raw

### Example
scripts/run_benchmarks.sh

### Custom run id
scripts/run_benchmarks.sh run_20260318_local_trial1

## Run Naming Convention
Use the pattern below:

run_<UTC timestamp>_<host>_<git sha>

Example:
run_20260318T154200Z_mbp_m1a2b3c

For manual naming, include these components:
- date and time in UTC
- host identifier
- git commit short SHA
- optional suffix for trial notes

## Output Contract
Each run directory contains:
- metadata.txt
- baseline.json
- sharded.json

Directory layout:
- results/raw/<run_id>/baseline.json
- results/raw/<run_id>/sharded.json
- results/raw/<run_id>/metadata.txt

## Benchmark Matrix
Both benchmark binaries are expected to run this matrix:
- shards: 1, 8, 16, 32, 64
- threads: 1, 2, 4, 8, 16, 32
- write percent: 0, 5, 20, 50
- key distribution mode:
  - 0: uniform
  - 1: hotspot (80 percent traffic on 20 percent keys)

## Reproducibility Controls
The runner captures:
- timestamp
- git SHA
- hostname
- uname
- benchmark binary paths
- benchmark filter and timing flags

To reduce run-to-run noise:
- close heavy background processes
- keep power mode consistent
- avoid changing CPU governor settings between runs

## Suggested Workflow
1. Build benchmarks.
2. Run scripts/run_benchmarks.sh.
3. Aggregate run output to CSV.
4. Commit or archive results/raw/<run_id> and results/processed/<run_id>.csv.
5. Plot charts into results/figures.

## Aggregation Script
- Script: scripts/analyze_results.py

Examples:

scripts/analyze_results.py --latest

scripts/analyze_results.py --run-id run_20260318T154200Z_mbp_m1a2b3c

Default CSV output path:
- results/processed/<run_id>.csv

## Plotting Script
- Script: scripts/plot_results.py

Examples:

scripts/plot_results.py --latest

scripts/plot_results.py --run-id run_20260318T154200Z_mbp_m1a2b3c

Default figure output path:
- results/figures/<run_id>/

Generated figures:
- throughput_vs_threads_w<write>_s<skew>.png
- latency_<p95_or_mean>_vs_threads_w<write>_s<skew>.png
- baseline_vs_sharded_grouped_t<threads>_s<shards>.png

## Notes
- The runner currently tolerates benchmark process failures and still writes partial output.
- If perf counters are not supported on your platform, JSON benchmark output will still be produced.
