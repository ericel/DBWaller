# Changelog

All notable changes to this project will be documented in this file.

The format is based on Keep a Changelog, and DBWaller follows Semantic Versioning.

## [Unreleased]

### Fixed

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

- Deterministic concurrency/lifecycle regressions with CTest watchdogs and a
  documented shutdown and callback-ownership contract.
- GitHub Actions workflows for pull request, release-tag, and nightly verification.
- Conan package install and export metadata plus a downstream `test_package/`.
- A documented CI and release roadmap for versioning and configuration control.

### Changed

- Project versioning now flows from the tracked `VERSION` file.
- Local and CI build instructions are aligned around Conan-managed dependencies and tracked CMake presets.
