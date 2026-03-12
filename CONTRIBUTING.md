# Contributing to DBWaller

Thanks for your interest in contributing.

## Project Status

DBWaller is an experimental systems project exploring:
- in-process encrypted caching
- request coalescing (single-flight)
- batching and fan-out acceleration
- execution boundaries (embedded vs daemon vs shared memory)

Expect breaking changes as APIs stabilize.

## Local Setup

```bash
conan profile detect --force
conan install . --output-folder=build --build=missing -s build_type=Release -s compiler.cppstd=20 \
  -o '&:with_tests=True' -o '&:with_benchmarks=False' -o '&:build_apps=False'
cmake --preset conan-release-tests
cmake --build --preset build-conan-release-tests
ctest --preset tier-a-pr
```

## Package Validation

If you touch public headers, dependency wiring, install rules, or packaging, run:

```bash
conan create . --build=missing -s build_type=Release -s compiler.cppstd=20 \
  -o '&:with_tests=False' -o '&:with_benchmarks=False' -o '&:build_apps=False'
```

## Change Control

- Keep generated build output out of git. `build/`, `CMakeCache.txt`, and `CMakeUserPresets.json` stay untracked.
- `VERSION` is authoritative for release numbering across CMake and Conan.
- Update `CHANGELOG.md` for user-visible, package-facing, or operational changes.
- Document workflow or configuration changes in `README.md`, `CONTRIBUTING.md`, or `docs/ci-release-roadmap.md`.

## Pull Requests

- Target `main` through a pull request. Direct pushes should be reserved for repository administration.
- Expect GitHub Actions Tier A validation plus `conan create` package verification before merge.
- If a PR intentionally prepares a release, note the planned tag explicitly and make sure it matches `VERSION`.
