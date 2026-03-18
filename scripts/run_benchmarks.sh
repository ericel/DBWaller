#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

RUN_TS="$(date -u +%Y%m%dT%H%M%SZ)"
GIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo nogit)"
HOST_NAME="$(hostname -s 2>/dev/null || hostname || echo unknown-host)"
RUN_ID="${1:-run_${RUN_TS}_${HOST_NAME}_${GIT_SHA}}"

RAW_DIR="$ROOT_DIR/results/raw/$RUN_ID"
mkdir -p "$RAW_DIR"

BENCH_FILTER="${BENCH_FILTER:-.*}"
MIN_TIME="${BENCH_MIN_TIME:-0.05}"
PERF_COUNTERS="${BENCH_PERF_COUNTERS:-}"

find_bin() {
  local name="$1"
  local candidates=(
    "$ROOT_DIR/build/build/Release/benchmarks/$name"
    "$ROOT_DIR/build/Release/benchmarks/$name"
    "$ROOT_DIR/build/benchmarks/$name"
    "$ROOT_DIR/build/$name"
  )

  local c
  for c in "${candidates[@]}"; do
    if [[ -x "$c" ]]; then
      echo "$c"
      return 0
    fi
  done

  return 1
}

BASELINE_BIN="${BASELINE_BIN:-}"
THROUGHPUT_BIN="${THROUGHPUT_BIN:-}"

if [[ -z "$BASELINE_BIN" ]]; then
  BASELINE_BIN="$(find_bin dbwaller_bench_baseline || true)"
fi
if [[ -z "$THROUGHPUT_BIN" ]]; then
  THROUGHPUT_BIN="$(find_bin dbwaller_bench_throughput || true)"
fi

if [[ -z "$BASELINE_BIN" || -z "$THROUGHPUT_BIN" ]]; then
  echo "Could not locate benchmark binaries."
  echo "Expected executables: dbwaller_bench_baseline and dbwaller_bench_throughput"
  echo "Install/build benchmark prerequisites first:"
  echo "  conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20"
  echo "    -o '&:with_tests=False' -o '&:with_benchmarks=True' -o '&:build_apps=False'"
  echo "    -c tools.cmake.cmaketoolchain:extra_variables=\"{'GIT_EXECUTABLE':'/usr/bin/git'}\""
  echo "Build them with benchmark-enabled presets, for example:"
  echo "  cmake --preset release-benchmarks"
  echo "  cmake --build --preset build-release-benchmarks"
  echo "or (Conan):"
  echo "  cmake --preset conan-release-benchmarks"
  echo "  cmake --build --preset build-conan-release-benchmarks"
  echo "Set BASELINE_BIN and THROUGHPUT_BIN env vars to explicit paths if needed."
  exit 1
fi

cat > "$RAW_DIR/metadata.txt" <<EOF
run_id=$RUN_ID
timestamp_utc=$RUN_TS
hostname=$HOST_NAME
git_sha=$GIT_SHA
uname=$(uname -a)
baseline_bin=$BASELINE_BIN
throughput_bin=$THROUGHPUT_BIN
bench_filter=$BENCH_FILTER
bench_min_time=$MIN_TIME
bench_perf_counters=$PERF_COUNTERS
EOF

COMMON_ARGS=(
  "--benchmark_filter=$BENCH_FILTER"
  "--benchmark_min_time=$MIN_TIME"
  "--benchmark_repetitions=5"
  "--benchmark_report_aggregates_only=true"
  "--benchmark_enable_random_interleaving=true"
  "--benchmark_counters_tabular=true"
  "--benchmark_format=console"
)

if [[ -n "$PERF_COUNTERS" ]]; then
  COMMON_ARGS+=("--benchmark_perf_counters=$PERF_COUNTERS")
fi

echo "[1/2] Running baseline benchmark..."
"$BASELINE_BIN" \
  "${COMMON_ARGS[@]}" \
  "--benchmark_out=$RAW_DIR/baseline.json" \
  "--benchmark_out_format=json" \
  || true

echo "[2/2] Running sharded throughput benchmark..."
"$THROUGHPUT_BIN" \
  "${COMMON_ARGS[@]}" \
  "--benchmark_out=$RAW_DIR/sharded.json" \
  "--benchmark_out_format=json" \
  || true

echo "Run complete."
echo "Results: $RAW_DIR"
