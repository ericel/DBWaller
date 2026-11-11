# Changelog

All notable changes to this project will be documented in this file.

The format is based on Keep a Changelog, and DBWaller follows Semantic Versioning.

## [Unreleased]

### Fixed

- Fence cache publication from misses and refreshes across key/tag invalidation,
  including cold keys and absent tag indexes. Keep new single-flights independent
  of invalidated work and prevent old refresh failures from altering replacement
  entries. Invalidated waiters receive `LoadInvalidatedError`.
- Fix release workflow validation by checking optional Artifactory credentials
  inside the upload step instead of referencing secrets in its condition.
- Preserve miss and refresh callbacks when a saturated compute queue falls back
  to inline execution, including callbacks with large captured values.
- Keep gateway miss inputs, policy/tags, and the shared adapter alive when queued
  computation outlives the waiting request's timeout.
- Close thread-pool admission during shutdown and drain accepted tasks so queued
  futures settle before shutdown returns. Serialize concurrent shutdown calls and
  synchronize worker-count reads with cleanup. Keep workers alive after a
  fire-and-forget callback throws.
- Fence engine shutdown against inline and pool-disabled computation as well as
  pooled tasks; reject new compute work once shutdown begins.
- Settle all shared waiters on rejected refreshes and callback-copy failures, and
  retire failed flights so later calls can retry.
- Wake the sweeper when stopping instead of waiting for its entire sleep interval.

### Added

- Optional `dbwallerd` gRPC daemon with TLS, namespace/operation authorization,
  bounded RPC admission and transport resources, graceful signal shutdown,
  aggregate diagnostics, and local TLS configuration tooling. Add actual socket
  and separate-process tests, plus opt-in Conan daemon packaging and Linux CI.
- Optional embedded entry-count limits, used by the daemon to bound tiny/empty
  entries and enforce per-shard value budgets at publication. Zero preserves
  the embedded default behavior.
- Experimental `dbwaller.v1` protocol and transport-independent daemon service
  with namespace-scoped binary keys/tags, bounded instance/owner-bound single-use
  tickets, monotonic expiry, conditional publication, and cold restart semantics.
  Embedded builds acquire no networking dependency. Add shared conformance
  scenarios and service tests. Public remote SDK and persistence remain planned.
- Public `get_snapshot()` and `get_snapshot_and_begin_load()` helpers: relative
  freshness metadata and a snapshot/ticket pair atomic with respect to invalidation.
- Public `begin_load()` / `put_if_current()` API with opaque, engine-bound
  `LoadTicket` and `PublishStatus` for external asynchronous fetches. Document the
  conservative engine-wide generation and unconditional `put()` semantics.
- Deterministic concurrency/lifecycle regressions with CTest watchdogs and a
  documented shutdown and callback-ownership contract.
- GitHub Actions workflows for pull request, release-tag, and nightly verification.
- Conan package install and export metadata plus a downstream `test_package/`.
- A documented CI and release roadmap for versioning and configuration control.

### Changed

- Project versioning now flows from the tracked `VERSION` file.
- Local and CI build instructions are aligned around Conan-managed dependencies and tracked CMake presets.
