# DBWaller Testing Plan

## Purpose

This document defines a practical, staged testing strategy for DBWaller across:

- Unit testing
- Functional testing
- Regression testing
- System testing

The goal is to move from today’s integration-focused validation to a repeatable, CI-gated quality model with clear coverage targets and release criteria.

## Current Baseline (as of 2026-03-04)

### What exists

- Integration executables in `tests/integration/`:
  - `test_claims_isolation.cpp`
  - `test_invalidation.cpp`
  - `test_thundering_herd.cpp`
  - `test_eviction.cpp`
  - `test_swr_bench.cpp`
- A placeholder Catch2 unit test file: `tests/core_engine_tests.cpp`
- CTest wiring for integration binaries via `tests/integration/CMakeLists.txt`
- Optional benchmarks via Google Benchmark (`benchmarks/`)

### Gaps to close

- Unit coverage is effectively missing.
- Functional and system tests are not formally separated.
- No dedicated regression suite/data artifacts.
- No written test taxonomy, quality gates, or CI cadence documented in-repo.

## Testing Strategy Overview

Use a 4-layer pyramid:

1. **Unit tests (fast, deterministic, high volume)**
2. **Functional tests (feature behavior across module boundaries)**
3. **Regression tests (bug replay + invariant lock-in)**
4. **System tests (end-to-end binaries, concurrency and resource stress)**

### Execution tiers

- **Tier A (per PR, required):** unit + selected functional + compile checks
- **Tier B (per merge to main):** full functional + regression + system smoke
- **Tier C (nightly):** heavy system/concurrency/memory/stress + benchmark trend checks

## Target Test Layout

Adopt this structure under `tests/`:

```text
tests/
  unit/
    core/
    policy/
    security/
    concurrency/
    observability/
  functional/
    gateway/
    policy_rules/
    invalidation/
    swr/
  regression/
    issues/
    datasets/
  system/
    embedded/
    daemon/
    stress/
  integration/   # keep existing files, gradually migrate/rename by intent
```

## Layer 1: Unit Testing Plan

### Scope

Unit tests should target pure logic and small classes/functions from:

- `src/core/sharded_engine.cpp`
- `src/policy/config_loader.cpp`
- `src/security/crypto.cpp`
- `src/security/claims.cpp`
- `src/concurrency/thread_pool.cpp`
- `src/observability/observer.cpp`

### Priority unit suites

1. **Policy key derivation and rule application**
   - cache key shape stability (namespace/op/id/viewer/claims)
   - vary-by behavior and fallback behavior
   - malformed/partial context behavior
2. **TTL/SWR time semantics**
   - fresh vs stale transitions
   - stale-on-error behavior transitions
3. **Claims and security helpers**
   - fingerprint determinism
   - distinct claims isolation
4. **Engine accounting primitives**
   - hit/miss/put/eviction counters
   - shard distribution consistency
5. **Thread pool basic scheduling and shutdown behavior**

### Coverage goal

- Initial target: **60% line coverage on core library sources**
- Next target: **75% line coverage** for `core/policy/security` modules

## Layer 2: Functional Testing Plan

Functional tests validate behavior across `Gateway + Engine + Adapter` interactions with realistic policies.

### Functional scenarios (must-have)

1. **Access isolation:** viewer/claims produce separate cache identities
2. **Tag invalidation:** direct tag and group tag invalidation
3. **Single-flight:** concurrent herd collapses to one backend fetch
4. **SWR behavior:** stale served quickly while background refresh runs
5. **Error fallback:** stale-on-error and eventual consistency after recovery
6. **Eviction behavior:** bounded memory under sustained write pressure

### Existing test mapping

- Current `tests/integration/*.cpp` already covers these behaviors and should be reclassified over time:
  - `test_claims_isolation` -> functional/security
  - `test_invalidation` -> functional/invalidation
  - `test_thundering_herd` -> functional/concurrency
  - `test_swr_bench` -> functional/swr
  - `test_eviction` -> functional/resource

## Layer 3: Regression Testing Plan

Regression tests lock in previously fixed bugs and high-risk invariants.

### Regression policy

- Every production bug or critical test escape gets:
  1. an issue ID,
  2. a minimal reproducer test in `tests/regression/issues/`,
  3. expected behavior assertions,
  4. a short note in changelog/release notes.

### Recommended regression classes

- cache key collision edge cases
- stale data exposure across claims/viewers
- invalidation misses for tag/group paths
- deadlock/starvation risks under herd pressure
- accounting/counter overflow or inconsistent snapshots

### Naming convention

- `reg_<issue_or_ticket>_<short_behavior>.cpp`
- Example: `reg_142_claims_fingerprint_collision.cpp`

## Layer 4: System Testing Plan

System tests validate deployable binaries and runtime characteristics.

### Targets

- `apps/embedded_demo`
- `apps/dbwaller_daemon` (as features become production-ready)
- `apps/benchmark_harness` (for controlled load profiles)

### System scenarios

1. **Embedded mode smoke test**
   - startup, policy load, fetch/hit path, clean shutdown
2. **Long-running stability test (30-120 minutes nightly)**
   - sustained mixed read/write/invalidation load
   - no crash, acceptable memory growth, stable throughput
3. **Concurrency stress test**
   - high parallel requests to hot and cold key sets
4. **Failure-injection system test**
   - backend latency spikes, intermittent failures, stale serving behavior
5. **Resource boundary test**
   - strict memory cap behavior + eviction under pressure

## Tooling and Build Integration

## Test frameworks

- Keep using **Catch2** for unit/functional/regression assertions.
- Keep **CTest** as the test orchestrator.
- Keep **Google Benchmark** for performance baselines (not pass/fail correctness).

## CMake expectations

Add explicit subdirectories in `tests/CMakeLists.txt`:

- `add_subdirectory(unit)`
- `add_subdirectory(functional)`
- `add_subdirectory(regression)`
- `add_subdirectory(system)`

Preserve existing `integration/` while migrating tests by intent.

## Local build and test commands

Use this baseline flow from the repository root:

```bash
conan profile detect --force
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20 \
  -o '&:with_tests=True' -o '&:with_benchmarks=False' -o '&:build_apps=False'
cmake --preset conan-release-tests
cmake --build --preset build-conan-release-tests
ctest --preset tier-a-pr
```

These tiers are mapped into GitHub Actions as:

- `ci.yml` -> Tier A
- `release.yml` -> Tier B
- `nightly.yml` -> Tier C

## Recommended labels

Register tests with CTest labels:

- `unit`
- `functional`
- `regression`
- `system`
- `slow`
- `nightly`

This enables selective runs:

- PR checks: `ctest -L "unit|functional"`
- nightly checks: `ctest -L "system|nightly|slow"`

## Quality Gates

## PR gate (required)

- Build succeeds with tests enabled.
- All unit tests pass.
- Functional smoke tests pass.
- No flaky failures in 2 repeated runs for changed test targets.

## Main branch gate (required)

- Full unit + functional + regression pass.
- System smoke passes.

## Nightly gate (required)

- Extended system and stress suites pass.
- Benchmark deltas reported (alert only; not hard fail initially).

## Rollout Plan (Phased)

## Phase 0 - Stabilize baseline (1 week)

- Keep existing integration tests green.
- Convert `tests/core_engine_tests.cpp` from placeholder to real unit suite.
- Introduce CTest labels for current tests.

## Phase 1 - Unit foundation (1-2 weeks)

- Add unit test targets for policy/security/core helpers.
- Achieve first 60% core coverage target.

## Phase 2 - Functional and regression expansion (2 weeks)

- Reclassify existing integration tests into functional buckets.
- Add first regression issue tests and regression naming policy.

## Phase 3 - System and resilience hardening (2+ weeks)

- Add long-running system/stress suites.
- Add failure injection and resource-bound tests.
- Introduce nightly benchmark trend report.

## Test Case Design Rules

- Deterministic by default: bounded timing windows and clear pass criteria.
- No external network dependency in unit/functional/regression suites.
- Explicit assertions on outcomes and critical counters.
- Avoid giant monolithic tests; prefer focused, scenario-driven cases.
- Any concurrency test must include bounded timeout and clear failure messages.

## Metrics and Reporting

Track and publish (at least per merge):

- pass/fail by test layer
- flaky test count
- mean test duration by layer
- code coverage trend (line/function)
- benchmark trend (p50/p95 latency, throughput)

## Ownership and Workflow

- Code owner for each module owns corresponding unit tests.
- Feature PRs must include or update tests in at least one layer.
- Bugfix PRs must include a regression test when applicable.

## Immediate Next Actions

1. Create `tests/unit`, `tests/functional`, `tests/regression`, `tests/system` directories.
2. Move/duplicate existing integration scenarios into `functional` with CTest labels.
3. Replace placeholder unit test with real suites for `policy` and `security` first.
4. Add CI jobs for Tier A (PR) and Tier B (main).
5. Add nightly Tier C job for stress/system + benchmarks.

## Initial Preset Commands

- Configure tests build:
   - `cmake --preset release-tests`
- Build tests:
   - `cmake --build --preset build-release-tests -j`
- Run Tier A (PR):
   - `ctest --preset tier-a-pr`
- Run Tier B (main):
   - `ctest --preset tier-b-main`
- Run Tier C (nightly):
   - `ctest --preset tier-c-nightly`
