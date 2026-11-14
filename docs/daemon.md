# Run the experimental daemon

`dbwallerd` is an optional C++20 gRPC server for one shared in-memory cache. The
embedded library remains the default. The daemon implements `dbwaller.v1`, TLS,
namespace authorization, bounded admission, aggregate statistics, and graceful
shutdown. A separately installable [C++ remote SDK](remote-cpp.md) and generic
example are available from source. There is no Redis/RESP compatibility,
persistence, replication, or Google Cloud installer yet. Applications keep their durable data
and perform backend fetches themselves.

For the packaged deployment, use the [container quickstart](container.md), which
includes a separate C++ example, TLS configuration, limits, and lifecycle checks.

The daemon selects OpenSSL's process-lifetime cleanup policy before configuration
validation or worker creation. This prevents global crypto cleanup racing gRPC
worker teardown at process exit. OpenSSL global allocations are reclaimed with
the process; cache entries, requests, and client-owned resources still retire
normally. Applications using the SDK must follow its explicit
[TLS startup contract](remote-cpp.md#process-wide-tls-startup-contract).

## Build and install

Supported daemon source platforms are Linux and macOS. Start with a Conan 2
profile matching the compiler you actually run; do not reuse a stale profile from
another compiler. CMake, Python 3, and an `openssl` command are needed for tests.
The optional Conan dependency is `grpc/1.84.0`; generated code uses that graph's
matching Protobuf compiler/runtime, even if other generators are on `PATH`.
The daemon recipe currently supports native builds only. Embedded-only builds
acquire neither networking dependency.

```sh
conan profile detect --name dbwaller-native
conan install . --output-folder=build/daemon --build=missing -r conancenter \
  -pr:h dbwaller-native -pr:b dbwaller-native \
  -s build_type=Release -s compiler.cppstd=20 \
  -o '&:with_daemon=True' -o '&:with_tests=True' -o '&:build_apps=False'
cmake -S . -B build/daemon/build/Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/daemon/build/Release/generators/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/daemon/build/Release -j 4
ctest --test-dir build/daemon/build/Release -L daemon --no-tests=error --output-on-failure
cmake --install build/daemon/build/Release --prefix "$PWD/build/daemon/install"
```

For an already provisioned CMake environment, set `DBWALLER_BUILD_DAEMON=ON`
and provide the gRPC/Protobuf package prefixes. The daemon requires gRPC with
`GRPC_ARG_MAX_ALLOWED_INCOMING_CONNECTIONS`; older gRPC versions fail compilation
instead of silently ignoring the configured connection limit. A system dependency
build has been exercised with gRPC 1.82.0. Use one coherent dependency graph for
release artifacts. Conan daemon and embedded package variants have different
package IDs; `with_daemon` is not removed by `package_id()`. Conan packaging copies
dependency license notices for daemon distributions. Source installs still rely
on their installed dependencies; this is not a standalone container distribution.

## Local TLS setup

The helper creates a **new** private directory, a random bearer credential, its
server-side SHA-256 digest, and a short-lived self-signed localhost certificate.
It refuses to overwrite an existing directory and never prints the token.

```sh
python3 scripts/create_daemon_config.py /tmp/dbwaller-local
build/daemon/install/bin/dbwallerd --config /tmp/dbwaller-local/daemon.json --check-config
build/daemon/install/bin/dbwallerd --config /tmp/dbwaller-local/daemon.json
```

The default endpoint is `127.0.0.1:7447`. Clients trust `server.crt`, read the token
from `client.token`, and use namespace `default`. Treat the token and private key
as secrets. The development certificate expires after seven days and is not a
deployment identity. Use certificates issued for the actual private hostname
before deployment. SIGINT or SIGTERM closes admission and drains local work.

Configuration paths resolve relative to the JSON file, not the process working
directory. Unknown fields, wrong types, negative/zero capacity settings, absent
credentials, malformed/mismatched TLS keys, and unsafe plaintext binds fail startup.
`--check-config` validates files without listening. Parser errors do not echo
credential values. Certificate expiry/hostname trust is checked by clients; the
startup key-pair check is not an external PKI validation service.

An explicit `"insecure_loopback": true` permits plaintext only on the literal
addresses `127.0.0.1` or `[::1]`; certificates must then be omitted. Hostnames such
as `localhost` and wildcard/private-network addresses are rejected in this mode.
Credentials are required in either mode. Non-loopback listening requires TLS.
Keep the daemon behind private networking/firewall rules; TLS does not make it a
publicly operated multi-tenant service.

## Credentials and namespaces

The main configuration references a `credentials_file`. Its shape is:

```json
{
  "principals": [
    {
      "name": "application-a",
      "token_sha256": "<64 hexadecimal characters: SHA-256 of the exact bearer token>",
      "namespaces": ["public-feed"],
      "write": true,
      "invalidate": true,
      "admin": false
    }
  ]
}
```

The example digest placeholder is intentionally not a working credential. Use a
cryptographically random token with at least 32 bytes of entropy. Transmit it in
`authorization: Bearer <token>` metadata over TLS, not inside request bodies.
Duplicate authorization metadata, duplicate principals, and duplicate credential
digests are rejected. Runtime digest comparisons use constant-time comparison.

Namespace membership permits reads. `write` permits BeginLoad, Publish, and
Lookup with ticket acquisition. `invalidate` permits key/tag invalidation.
`admin` permits Health and Stats; it does not implicitly grant data access to
other namespaces. Multiple principals can share a namespace, but tickets cannot
be transferred between principals. There is no automatic interpretation of
application user permissions: the application must design its namespaces and keys.

Configuration and credentials are loaded once. Rotate by updating protected files
and restarting; this drops cached data and outstanding tickets. Keep an overlap
credential under a separate principal if application rollout needs it. Credentials
are not read from VM metadata, logged, or returned by diagnostics. Dynamic rotation
and Secret Manager integration belong to later deployment work.

## Requests, deadlines, and lifecycle

Reuse a gRPC channel, set a finite deadline on every RPC, and disable automatic
retries/hedging for Publish. Requests without a deadline are rejected. All cache
work and authorization run on a bounded worker pool, not a gRPC callback thread.
Cancellation or deadline expiry observed before work starts skips the operation.
Completed or already-started mutations are not rolled back. A client-side timeout
or disconnect is not proof that no write occurred; never get a new ticket merely
to retry an old fetched result. See [Bridge service contract](bridge-service.md).

The reactor owns admission capacity through `OnDone`, including cancelled work
still queued. Queued work retires on the bounded executor. Closing admission makes
new calls Unavailable; accepted local work drains. After `shutdown_grace_ms`,
gRPC cancels remaining RPCs; executor work still retires safely before its state
is destroyed. The grace is a transport cancellation deadline, not a mechanism to
forcibly kill C++ code. The daemon executes no application/backend callbacks.

Health reports readiness, protocol/build version, and an instance identity that
changes on restart. Stats reports aggregate cache/ticket/RPC counters, including
active, accepted, rejected, and server-observed cancelled calls. Active includes
the Stats call itself. There is no cache-key/value listing. Diagnostics require
an admin identity; process liveness should not depend on cache data availability.

## Resource configuration

All limits are per process, not per principal. Optional top-level sections:

```json
{
  "cache": {
    "shards": 16,
    "value_bytes": 67108864,
    "entries": 16384,
    "max_key_bytes": 4096,
    "max_value_bytes": 1048576,
    "max_tags": 32,
    "max_tag_bytes": 256
  },
  "tickets": {"records": 4096, "key_bytes": 4194304, "lifetime_ms": 30000, "reap_per_call": 64},
  "rpc": {
    "workers": 4, "queue": 64, "active": 128,
    "connections": 64, "streams_per_connection": 16,
    "message_bytes": 2097152, "transport_memory_bytes": 67108864,
    "shutdown_grace_ms": 5000
  }
}
```

These sections supplement the listen/TLS/credentials settings; they are not a
complete runnable configuration. Ticket lifetime and cache TTL+SWR are capped at
one day. Message limits apply to receive/send; gRPC metadata is capped at 8 KiB.
TLS handshakes have a 10-second timeout and idle connections a 60-second limit.
The transport resource quota has a 16-thread cap. Admission rejects overload
instead of running excess work inline. Thread and message quotas do not represent
a total RSS cap: use OS/container memory limits and leave room for runtime buffers.

Entry and value budgets are divided across shards. With a finite entry limit,
publication enforces both budgets immediately, including zero-length values.
Stored means accepted under eviction policy; it does not pin a value. An oversized
value relative to its shard's budget may be evicted immediately. Keep value sizes,
key/tag limits, and entry counts proportionate to the machine's RAM. Embedded
`max_entries_total=0` preserves the prior behavior and approximate byte eviction.

## Tests and current scope

The `daemon` CTest label prepares ephemeral TLS fixtures, runs actual loopback
RPCs, and starts the executable in a separate process. It covers independent
clients, stale publication fencing, namespace/role enforcement, wrong ticket
owners, malformed/oversized requests, certificate trust, connection limits,
queue/active admission, cancellation, SIGTERM, cold restart, and previous-instance
tickets. The queue tests use controlled gates and server-side counters rather
than sleeps; process/socket operations have generous watchdogs.

`verify-daemon` in CI builds the optional Conan graph and runs Tier B on Linux;
the original `verify` still checks the embedded-only build and package. A local
macOS pass is not a claim that the remote Linux job has run. Sanitizer builds can
enable `DBWALLER_BUILD_DAEMON=ON` with the flags in [Compute lifecycle](compute-lifecycle.md).
System-installed gRPC/Protobuf binaries are not instrumented by those flags.

For ThreadSanitizer, instrument the dependency graph as well. In particular,
uninstrumented gRPC/Abseil scheduling can hide synchronization from TSan. Use a
separate build directory and append these options to the Conan install command:

```sh
-c 'tools.build:cflags=["-fsanitize=thread","-fno-omit-frame-pointer","-g"]' \
-c 'tools.build:cxxflags=["-fsanitize=thread","-fno-omit-frame-pointer","-g"]' \
-c 'tools.build:exelinkflags=["-fsanitize=thread"]' \
-c 'tools.build:sharedlinkflags=["-fsanitize=thread"]' \
-c 'tools.info.package_id:confs=["tools.build:cflags","tools.build:cxxflags","tools.build:exelinkflags","tools.build:sharedlinkflags"]'
```

The final option makes instrumentation part of dependency package identities,
so sanitizer artifacts do not replace release packages. This builds substantial
debug information in Conan's cache as well as the selected output directory;
allow space for both. Configure/build with the generated toolchain, then run
`TSAN_OPTIONS=halt_on_error=1 ctest --test-dir <build-directory> -L 'concurrency|daemon' --no-tests=error --output-on-failure`.

Local validation on macOS/AppleClang 21 (2026-11-11):

- Native Conan graph, gRPC 1.84.0 / Protobuf 6.30.1: Tier B **48/48**.
- Fully instrumented Conan dependency graph: TSan concurrency/daemon **42/42**,
  without suppressions. An earlier partially instrumented build using system
  gRPC 1.82.0 reported callback scheduling races; those reports did not reproduce
  with the matched, fully instrumented graph.
- ASan/UBSan: the 47-test suite passed, then both capacity regressions passed
  after adding the byte-budget case. That build used uninstrumented system
  networking libraries; it is not dependency-wide ASan coverage.
- Embedded and daemon Conan packages passed their downstream consumer checks;
  the daemon package also ran its installed executable. Workflow syntax passed
  `actionlint`. Linux CI and cloud deployment have not been run locally.

The [separately installable asynchronous C++ client](remote-cpp.md) includes current
validation results and the explicit startup mitigation for the dependency
process-exit TSan finding. Python, Node.js, GCS snapshots, containers, and the Google Cloud installer remain later
work. This daemon remains a disposable cache with application-owned backend fallback.
