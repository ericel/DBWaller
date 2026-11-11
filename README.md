# DBWaller

DBWaller is a **C++20 data-plane gateway** (“data firewall”) that sits between application backends (web/mobile) and authoritative services/databases to enforce **context-aware access rules** and accelerate hot paths with **encrypted in-memory caching**, **request coalescing (single-flight)**, **batching**, and **fan-out orchestration**.

It is designed for cost-aware, cloud-native deployments (e.g., ephemeral instances) while preserving correctness by treating the cache as **non-authoritative** and always allowing fallback to the source of truth.

## Why DBWaller?

Most modern apps eventually depend on external cache infrastructure (Redis/Memcached) to reduce latency and backend load. DBWaller explores a different route: a reusable, high-performance **data-plane layer** that can run:

- **Embedded** inside your service (max locality, lowest overhead)
- **As an optional daemon** (experimental gRPC boundary for multi-client use)
- **(Planned)** as a shared-memory multi-process variant (isolation + locality)

## Core Capabilities

- **Policy-aware caching** with explicit *vary-by* rules (viewer/claims/locale/field masks)
- **Encrypted cache values** (AEAD-ready design; key providers pluggable)
- **Tag-based invalidation** (`post:{id}`, `user:{id}`, `timeline:{viewer}`, etc.)
- **TTL + eviction** with shard-based memory accounting
- **Single-flight coalescing** to prevent thundering herds on hot keys
- **Batch hydration hooks** for fan-out and aggregate workloads
- **Observability hooks** (stats, logging, metrics export planned)

For compute queue backpressure, callback ownership, request timeouts, and graceful
shutdown, see [Compute lifecycle](docs/compute-lifecycle.md).
For invalidation during an in-flight fetch and the public load-ticket API for
external asynchronous fetches, see [Cache invalidation](docs/cache-invalidation.md).
For the proposed standalone service, installable public C++ SDK, Google Cloud
deployment kit, and later Python and Node.js clients, see the
[Daemon bridge implementation plan](docs/daemon-bridge-plan.md).
The experimental `dbwallerd` implements the versioned gRPC protocol with TLS,
namespace authorization, bounded admission, and graceful shutdown. See
[Run the daemon](docs/daemon.md) and [Bridge service contract](docs/bridge-service.md).
The installable public C++ remote SDK remains the next stage.

## Project Structure

- `include/dbwaller/` — public headers (core, concurrency, policy, crypto, adapters)
- `src/` — implementations
- `cmake/` — package configuration templates
- `apps/embedded_demo` — minimal embedded example
- `apps/dbwaller_daemon` — optional gRPC daemon and service foundation
- `proto/dbwaller/v1` — experimental protocol and shared conformance scenarios
- `apps/benchmark_harness` — load + latency evaluation harness (planned)
- `tests/` and `benchmarks/` — correctness + performance suites
- `test_package/` — Conan consumer verification
- `docs/` — architecture, testing, CI, and release controls

## Build And Test

DBWaller uses Conan to provision third-party dependencies for local development and CI.

```bash
conan profile detect --force
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20 \
  -o '&:with_tests=True' -o '&:with_benchmarks=False' -o '&:build_apps=False'
cmake --preset conan-release-tests
cmake --build --preset build-conan-release-tests
ctest --preset tier-a-pr
```

To build the demo binary without tests:

```bash
conan profile detect --force
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20 \
  -o '&:with_tests=False' -o '&:with_benchmarks=False' -o '&:build_apps=True'
cmake --preset conan-release
cmake --build --preset build-conan-release
./build/build/Release/apps/embedded_demo/dbwaller_embedded_demo
```

## Benchmark Automation

Build dependencies and benchmark targets, then run:

```bash
conan profile detect --force
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20 \
  -o '&:with_tests=False' -o '&:with_benchmarks=True' -o '&:build_apps=False' \
  -c tools.cmake.cmaketoolchain:extra_variables="{'GIT_EXECUTABLE':'/usr/bin/git'}"
cmake --preset conan-release-benchmarks
cmake --build --preset build-conan-release-benchmarks
```

Then run:

```bash
scripts/run_benchmarks.sh
```

Note: the `GIT_EXECUTABLE` override avoids a known macOS environment issue where Git.app returns an empty `git describe` result during `benchmark/1.9.1` configure.

Outputs are written to `results/raw/<run_id>/` with:

- `baseline.json`
- `sharded.json`
- `metadata.txt`

Aggregate to CSV:

```bash
scripts/analyze_results.py --latest
```

This writes `results/processed/<run_id>.csv`.

Generate figures:

```bash
scripts/plot_results.py --latest
```

This writes PNG charts under `results/figures/<run_id>/`.

See `docs/benchmark-methodology.md` for naming conventions and experiment controls.

## Create A Conan Package

```bash
conan create . --build=missing -s build_type=Release -s compiler.cppstd=20 \
  -o '&:with_tests=False' -o '&:with_benchmarks=False' -o '&:build_apps=False'
```

This produces a static library package and runs `test_package/` to verify downstream consumption via `find_package(DBWaller CONFIG REQUIRED)`.

## Consume From CMake

```cmake
find_package(DBWaller CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE DBWaller::dbwaller)
```

## Versioning And Release Control

- `VERSION` is the single source of truth for the project version.
- Release tags must match `v<version>`, for example `v0.1.0`.
- User-visible and package-facing changes belong in `CHANGELOG.md`.
- GitHub workflow and branch-protection guidance lives in `docs/ci-release-roadmap.md`.
