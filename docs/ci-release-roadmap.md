# DBWaller CI And Release Roadmap

This document defines the tracked path for continuous integration, versioning, and release control as DBWaller becomes a publishable Conan library.

## Goals

- Keep one authoritative version number across CMake, Conan, tags, and release notes.
- Make every pull request prove the library still builds, tests, and packages cleanly.
- Make release publishing deliberate, reproducible, and driven by tracked configuration.

## Version Source Of Truth

- `VERSION` is authoritative for the project version.
- `CMakeLists.txt` reads `VERSION` and exposes it as `PROJECT_VERSION`.
- `conanfile.py` reads `VERSION` in `set_version()`.
- Release tags must be `v<version>`. Example: `v0.1.0`.
- `CHANGELOG.md` records user-visible, package-facing, and operational changes.

## GitHub Workflow Layout

- `.github/workflows/ci.yml`
  - Runs on pull requests and pushes to `main`.
  - Installs Conan dependencies.
  - Builds with the tracked Conan-aware CMake presets.
  - Runs Tier A checks via `ctest --preset tier-a-pr`.
  - Runs `conan create .` to verify the install/export boundary and `test_package/`.
- `.github/workflows/release.yml`
  - Runs on `v*.*.*` tags.
  - Verifies the pushed tag matches `VERSION`.
  - Runs Tier B validation with tests enabled before any release approval is requested.
  - Waits on the `release` GitHub environment before publishing.
  - Re-creates the Conan package in the gated publish job.
  - Optionally uploads to Artifactory when the Artifactory variables and secrets are present.
  - Creates a GitHub release after the gated publish job completes.
- `.github/workflows/nightly.yml`
  - Runs on a schedule and by manual dispatch.
  - Executes Tier C system-oriented validation.
  - Publishes test artifacts for inspection.

## Required GitHub Repository Settings

- Protect `main`.
- Require pull requests before merge.
- Require the `verify` status check before merge.
- Block force pushes to `main`.
- Add a `release` environment with required reviewer `ericel` and `prevent_self_review=false`.
- Add a tag ruleset for `v*` that blocks tag updates and deletions.

## Optional Artifactory Upload Configuration

Only set these if you want `release.yml` to publish built packages to Artifactory. If they are absent, the workflow still validates the tag, builds the package, and creates the GitHub release, but it skips remote upload.

- Repository variables:
  - `ARTIFACTORY_REMOTE_NAME`
  - `ARTIFACTORY_REMOTE_URL`
- Repository secrets:
  - `ARTIFACTORY_USERNAME`
  - `ARTIFACTORY_PASSWORD`

## Release Environment Gate

- The `publish` job in `.github/workflows/release.yml` uses the `release` environment.
- Validation completes before GitHub asks for approval.
- `ericel` is configured as the required reviewer.
- Self-review is allowed so the gate stays usable in a solo-maintainer repository.

## Local Maintainer Flow

```bash
conan profile detect --force
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20 \
  -o '&:with_tests=True' -o '&:with_benchmarks=False' -o '&:build_apps=False'
cmake --preset conan-release-tests
cmake --build --preset build-conan-release-tests
ctest --preset tier-a-pr
conan create . --build=missing -s build_type=Release -s compiler.cppstd=20 \
  -o '&:with_tests=False' -o '&:with_benchmarks=False' -o '&:build_apps=False'
```

To cut a release:

1. Update `CHANGELOG.md`.
2. Commit any release-ready changes with the desired `VERSION`.
3. Push the release tag `v<version>`.
4. Approve the `release` environment when GitHub prompts after validation succeeds.
5. Let `release.yml` publish the package and create the GitHub release.

## Staged Path Forward

### Phase 1

- Keep CI on `ubuntu-latest` until package creation is stable.
- Treat `conan create .` as the package boundary gate.
- Publish static library binaries first.

### Phase 2

- Extend `ci.yml` to add `macos-latest` and then `windows-latest`.
- Add benchmark trend capture to nightly once performance baselines matter for regression control.
- Introduce branch protection for release branches if DBWaller adopts long-lived stabilization branches.
