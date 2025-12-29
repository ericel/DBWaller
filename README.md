# DBWaller

DBWaller is a **C++20 data-plane gateway** (“data firewall”) that sits between application backends (web/mobile) and authoritative services/databases to enforce **context-aware access rules** and accelerate hot paths with **encrypted in-memory caching**, **request coalescing (single-flight)**, **batching**, and **fan-out orchestration**.

It is designed for cost-aware, cloud-native deployments (e.g., ephemeral instances) while preserving correctness by treating the cache as **non-authoritative** and always allowing fallback to the source of truth.

## Why DBWaller?

Most modern apps eventually depend on external cache infrastructure (Redis/Memcached) to reduce latency and backend load. DBWaller explores a different route: a reusable, high-performance **data-plane layer** that can run:

- **Embedded** inside your service (max locality, lowest overhead)
- **As a daemon** (Redis-like boundary for multi-client use)
- **(Planned)** as a shared-memory multi-process variant (isolation + locality)

## Core Capabilities

- **Policy-aware caching** with explicit *vary-by* rules (viewer/claims/locale/field masks)
- **Encrypted cache values** (AEAD-ready design; key providers pluggable)
- **Tag-based invalidation** (`post:{id}`, `user:{id}`, `timeline:{viewer}`, etc.)
- **TTL + eviction** with shard-based memory accounting
- **Single-flight coalescing** to prevent thundering herds on hot keys
- **Batch hydration hooks** for fan-out and aggregate workloads
- **Observability hooks** (stats, logging, metrics export planned)

## Project Structure

- `include/dbwaller/` — public headers (core, concurrency, policy, crypto, adapters)
- `src/` — implementations
- `apps/embedded_demo` — minimal embedded example
- `apps/dbwaller_daemon` — standalone service (planned)
- `apps/benchmark_harness` — load + latency evaluation harness (planned)
- `tests/` and `benchmarks/` — correctness + performance suites (planned)
- `docs/` — architecture, evaluation plan, threat model

## Build (Skeleton)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DDBWALLER_BUILD_TESTS=OFF -DDBWALLER_BUILD_BENCHMARKS=OFF
cmake --build build -j
./build/apps/embedded_demo/dbwaller_embedded_demo
