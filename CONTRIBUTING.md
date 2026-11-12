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

The daemon service tests are included in Tier A/B without a network listener or
additional runtime dependencies. CI validates the experimental schema in
`verify-daemon` when building the generated wire code with Conan's matching
Protobuf compiler/runtime. The embedded `verify` job requires no system Protobuf
installation or APT step. For a standalone local schema check, use an installed
`protoc`:

```bash
protoc --proto_path=proto --descriptor_set_out=build/dbwaller-v1.pb proto/dbwaller/v1/cache.proto
```

See [Bridge service contract](docs/bridge-service.md) for targeted facade tests.
[Run the daemon](docs/daemon.md) covers the opt-in listener build and actual socket
tests. The [C++ remote SDK](docs/remote-cpp.md) is packaged separately. Daemon changes must pass the optional
build with `with_daemon=True`, Tier B, and the `daemon` CTest label. Verify both
embedded and daemon Conan package variants when changing their build/install rules.
For client changes, enable `with_remote_tests=True` alongside the daemon/tests,
run Tier B including the `remote` label, and run `conan create sdk/cpp` with a
matching C++20 compiler profile. Verify the installed example over TLS and during
an outage using `tests/system/remote/installed_example.py`. The root Conan package
does not install the remote SDK, even when its integration tests are enabled.
Socket-test executables follow the documented TLS startup policy. The
`remote_tls_policy` subprocess regression retains baseline and too-late cases;
do not remove those checks or suppress TSan reports to make a run pass.

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
