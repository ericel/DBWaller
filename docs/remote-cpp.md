# C++ remote client

`sdk/cpp` is a separately installable C++20 client for the experimental
`dbwaller.v1` service. It provides `DBWallerRemote::client` and the Conan package
`dbwaller-remote`, with versions taken from the repository's `VERSION` file.
Native Linux and macOS builds are supported; Windows and cross compilation are
not yet supported. This is a source installation route, not a claim that a package
has been published to ConanCenter. The API remains experimental at version 0.x.

The public header is `<dbwaller/remote/client.hpp>`. Consumers do not include
generated Protobuf/gRPC headers, link the engine, or need a sibling application.
The implementation depends on gRPC and Protobuf. Embedded-only DBWaller builds
still acquire neither dependency. No cloud account is needed.

## Install and build an independent example

From a public source checkout, with Conan 2, CMake, and a C++20 compiler:

```sh
conan profile detect --name dbwaller-native
conan create sdk/cpp --build=missing -r conancenter \
  -pr:h dbwaller-native -pr:b dbwaller-native \
  -s build_type=Release -s compiler.cppstd=20

conan install examples/remote_cpp --output-folder=build/remote-example --build=missing \
  -pr:h dbwaller-native -pr:b dbwaller-native \
  -s build_type=Release -s compiler.cppstd=20
cmake -S examples/remote_cpp -B build/remote-example/build/Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/remote-example/build/Release/generators/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/remote-example/build/Release -j 4
build/remote-example/build/Release/dbwaller_remote_example --mode disabled
```

Use a profile that matches the compiler being invoked. The SDK pins gRPC and its
matching generators through Conan, preventing a system `protoc` from accidentally
generating code for another runtime. `conan create` runs an unrelated consumer
that includes only the public header. `shared=True` selects a shared SDK library;
the default is static. Distributions include first-party and dependency notices.

In your own Conan recipe, require `dbwaller-remote/0.1.0`, generate `CMakeDeps` and
`CMakeToolchain`, and use:

```cmake
find_package(DBWallerRemote CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE DBWallerRemote::client)
```

Standalone CMake builds from `sdk/cpp` also support `cmake --install` and export a
relocatable `DBWallerRemoteConfig.cmake`. Supply a coherent gRPC/Protobuf dependency
graph, including their code generators, when building from source. Installed
static consumers still need those dependencies; the archive is not a replacement
for a dependency manager. Headers and generated wire code are bundled separately:
only the SDK header is public, and wire implementation objects are in the archive.

Build/start `dbwallerd` and create local TLS credentials using
[Run the daemon](daemon.md). With that guide's `/tmp/dbwaller-local` configuration:

```sh
build/remote-example/build/Release/dbwaller_remote_example --mode remote \
  --endpoint 127.0.0.1:7447 --namespace default \
  --ca /tmp/dbwaller-local/server.crt --token-file /tmp/dbwaller-local/client.token
```

The example fetches a simulated backend, reuses a cached value, updates the backend
and invalidates the cache, then reads the new value. Stop the daemon and run it
again to observe backend fallback. It reads credentials from files and does not
print them. The development certificate is for local testing only.

## Requests and application executors

Call `dbwaller::remote::initialize_process_tls()` at application startup, before
**any OpenSSL use** (including credential hashing or crypto-using global objects)
and before starting workers. This selects the process-lifetime cleanup policy
required by the current gRPC/OpenSSL combination. See the startup contract below.

Reuse a `dbwaller::remote::Client` per endpoint/namespace/credential configuration.
It owns one completion-queue thread and a reusable channel; gRPC manages its own
bounded transport resources. Each method returns a copyable `Request<T>` with a
shared future, explicit `cancel()`, and optional completion notification.
There is no thread per request and no unbounded SDK admission queue.

```cpp
namespace r = dbwaller::remote;
r::initialize_process_tls(); // At process startup, before any other crypto use.
r::ClientOptions options;
options.endpoint = "cache.internal:7447";
options.cache_namespace = "public-feed";
options.bearer_token = read_protected_token_file(); // Application-supplied.
options.root_certificates = read_ca_file();
r::Client cache(std::move(options));
auto deadline = r::Clock::now() + std::chrono::milliseconds(200);
auto request = cache.lookup("post:123", true, {deadline});

// post_to_event_loop must enqueue; it must not execute the task inline or wait
// for the task. Capture an owned completion/coordinator, not stack references.
auto receipt = request.on_complete(post_to_event_loop, [coordinator](auto result) {
    coordinator->complete(std::move(result));
});
// request.cancel() stops this call without affecting other requests.
```

The posting hook may run on the SDK completion thread or the subscribing thread
when a result is already ready. It must be thread-safe, enqueue promptly, and
throw if it rejects a task. Never call client shutdown/destruction inside that
hook. The actual application callback runs on the supplied executor and may use
normal client lifecycle operations there. The SDK has no framework/event-loop
dependency; a coroutine adapter can resume its owned coordinator on this executor.
No built-in coroutine or Drogon adapter is supplied yet.

One notification is allowed per operation, including across copies of a handle.
Its returned `future<void>` reports posting/callback exceptions independently of
the RPC result. Observe that receipt if notification delivery matters. A caller
executor that accepts and then discards a task violates its delivery contract;
the SDK cannot drive or drain that external queue.

`get()` and `future().get()` are blocking conveniences for main/background threads.
Do not use them on an event loop. Dropping a request handle does not cancel it;
use `cancel()` for that. Requests and queued application notifications can outlive
the client. Capture application state with the necessary lifetime.

Concurrent calls, cancellation, and repeated/concurrent `shutdown()` are supported.
Shutdown closes admission, cancels outstanding network work, drains completions,
and joins the SDK thread; all accepted RPC futures settle before it returns.
Application executor tasks are separate and may run afterward. Stop external
callers before destroying the client. Destruction invokes shutdown; threads are
not detached.

### Process-wide TLS startup contract

The helper calls OpenSSL's supported `OPENSSL_init_ssl(OPENSSL_INIT_NO_ATEXIT, ...)`
before another caller can register automatic global cleanup. gRPC 1.84.0 uses the
same policy, but its initialization is too late if an application has already used
OpenSSL. Its internal EventEngine threads may still be retiring after ordinary
gRPC shutdown; OpenSSL global cleanup can race their thread-local destructors.
See [gRPC's upstream change](https://github.com/grpc/grpc/pull/42608) and
[OpenSSL initialization documentation](https://docs.openssl.org/3.5/man3/OPENSSL_init_crypto/).

This is an explicit application startup decision. Client construction does not
call this helper; relying on gRPC's later TLS initialization is too late when other
code has already used OpenSSL. The supplied daemon, example, and socket-test
executables initialize the policy before credential hashing or worker creation.
The embedded library's initialization and dependencies are unchanged.

Repeated helper calls are safe. A late call **cannot undo** an already registered
OpenSSL exit handler, and a successful return does not establish that the call
was early enough. If an earlier global constructor uses crypto, arrange startup
before it or use an OpenSSL build with its documented `no-atexit` configuration.
Applications that cannot control this policy remain affected by the upstream
shutdown limitation; the helper is not a universal repair for every host process.

OpenSSL's global allocations remain until the operating system reclaims the
process; per-request/client objects still retire normally. Do not call
`OPENSSL_cleanup()` manually or unload OpenSSL/gRPC while their workers may remain.
Dynamic unloading of this transport stack is not supported. No sleeps, detached
SDK threads, or sanitizer suppressions implement this mitigation.

## Deadlines, limits, and errors

Every RPC has a finite deadline. The default is 1 second from method entry;
`CallOptions` accepts an absolute `steady_clock` deadline to share an application
budget. The maximum is 24 hours. Admission, connection/TLS establishment, transfer,
and response decoding count toward it. Results observed at or after the deadline
are rejected. Delivery time on the application's executor is outside the RPC
budget. Earlier explicit cancellation preserves its cancellation outcome.

The default admission limit is 64 active network calls (configurable 1–4096).
Excess calls fail immediately with `ResourceExhausted`; there is no hidden retry
queue. Default message limit is 2 MiB (configurable 4 KiB–16 MiB); the daemon may
enforce smaller key/value/ticket limits. These bounds do not cap application-owned
result handles, queued callbacks, or total process memory.

Expected operation failures are `Result<T>` errors: invalid input, authentication,
authorization, capacity, availability, deadline, cancellation, closed client,
malformed/incompatible protocol, or internal failure. Invalid constructor options
throw `std::invalid_argument`; accessing the wrong Result alternative throws.
Error messages are generic and do not echo server diagnostics or caller data.
Unknown response enums, invalid durations, and unsupported Health protocol versions
are rejected. Empty byte strings remain valid hits: inspect `LookupKind`.

TLS validates the server identity and certificate trust chain. An empty CA string
uses the transport's default trust roots. Plaintext requires an explicit
`insecure_loopback` option and a literal loopback address, for development only.
Bearer credentials and namespaces follow the [daemon authorization contract](daemon.md#credentials-and-namespaces).
Application user authorization must happen **before a cache lookup** as well as
before backend fetching; the daemon does not know an application's users/claims.

## Conditional publication and fallback policy

Lookup may acquire a ticket atomically with its snapshot, or use `begin_load()`
before starting a backend fetch. Tickets are opaque, endpoint/namespace-bound
client types; the server also enforces owner, instance, generation, single-use,
and expiry constraints. They are distinct from native `core::LoadTicket` values.

Publish returns `Stored`, `Invalidated`, `Expired`, `Consumed`, or `UnknownTicket`.
Discard a rejected fetched value. If application policy permits, acquire a new
ticket **and fetch again** within a bounded budget. Never give old fetched bytes
a new ticket. Relative freshness/ticket durations subtract the full elapsed call
time conservatively, and remain hints rather than invalidation leases.

No RPC is automatically retried. A failed/cancelled attempted publication or
invalidation may have reached the server: `Error::outcome_unknown` is conservative,
not evidence that it did execute. A timeout cannot roll back a mutation. Losing an
invalidation response requires application reconciliation before trusting cache
hits; fallback on the current request alone does not repair other readers.

`examples/remote_cpp/cache_aside.hpp` is an optional **example policy**, not a stable
SDK interface. It demonstrates disabled, embedded, and remote stores with distinct
ticket types, caller-owned backend fetching, explicit stale-read policy, an overall
budget, shorter per-cache budgets, and at most two backend fetches. Its blocking
helpers belong on main/backend workers. A real backend must honor its supplied
deadline; arbitrary application callbacks cannot be forcibly stopped.

Remote mode falls back to the backend on unavailable reads/ticket acquisition.
If publication is rejected it fetches again with a new ticket; if publication's
outcome is unknown it performs one fresh backend read without caching. It never
silently installs an independent local cache. Disabled mode always reads the
backend. Embedded mode uses the installed engine and its invalidation fencing.
To build that optional example mode, add `dbwaller/0.1.0` to the example's Conan
requirements and configure `-DDBWALLER_EXAMPLE_EMBEDDED=ON`. Remote-only consumers
need neither the engine nor the daemon library.

The service starts empty after restart. Tickets do not survive it. There is no
distributed single-flight, backend freshness guarantee, persistence, replication,
or automatic failover. Python/Node.js clients and the Google Cloud installer are
later stages of the [bridge plan](daemon-bridge-plan.md).

## Validation

Add `-o '&:with_remote_tests=True'` to the daemon's Conan install with
`with_daemon=True` and `with_tests=True`, then build and run Tier B or
`ctest --test-dir <build> -L remote --no-tests=error --output-on-failure`.
The `remote` tests use local TLS fixtures and controlled server callbacks for
deadlines, cancellation, capacity, shutdown, dispatch, protocol failures, and
cache modes. Separate client processes exercise the public header over real TLS.

After installing/building the standalone example, its release smoke test is:

```sh
python3 tests/system/remote/installed_example.py \
  --daemon build/daemon/build/Release/apps/dbwaller_daemon/dbwallerd \
  --example build/remote-example/build/Release/dbwaller_remote_example
```

It creates temporary loopback credentials and tests the installed consumer against
a daemon and then an outage, with process watchdogs. CI includes package creation,
the installed example, and Tier B. No live cloud service or real credentials are
used. Sanitizer builds should instrument the dependency graph for meaningful TSan
results; see the daemon testing notes.

Local validation on macOS / AppleClang 21 (2026-11-12):

- Native Conan graph after the TLS startup mitigation: Tier B **51/51**, including
  all Tier A tests and three remote CTest entries. The client executable covers
  **12 test cases**; `remote_tls_policy` separately verifies real process-exit
  handlers for early, baseline, and late initialization.
- ASan/UBSan concurrency, daemon, and remote selection: **45/45**. Networking libraries
  in this build are uninstrumented system libraries, so this is SDK/application
  coverage, not dependency-wide ASan coverage.
- Static and shared SDK Conan packages passed their downstream consumer checks;
  existing embedded and daemon package variants passed too. The installed example
  passed TLS/invalidation/outage checks via Conan and via relocated native CMake
  exports. The remote-only package does not link the embedded engine.
  These checks were rerun after the mitigation using the existing native dependency
  graph (including c-ares 1.25.0 and zlib 1.3.1). Initial unlocked package checks
  selected newer cached dependencies and were stopped during dependency compilation;
  they did not produce package-validation results.
- Before the TLS startup mitigation, fully instrumented TSan: **42/43** entries passed. The client's
  **480 assertions / 12 cases passed**, then process exit reported OpenSSL cleanup
  racing gRPC worker teardown. A [gRPC/OpenSSL-only reproducer](../tests/diagnostics/grpc_exit/README.md)
  reproduced the same report without DBWaller. This original result was **not clean**.
  After applying the explicit startup policy, the fully instrumented concurrency,
  daemon, and remote selection passed **45/45** with no suppressions.
  The reproducer retains that original mode and now also accepts an explicit early
  initialization mode. Twenty fresh processes in that mode passed under TSan,
  without suppressions. This mitigates the observed race for applications following
  the startup contract above; hosts that initialize OpenSSL earlier remain affected.
- Workflow syntax passed `actionlint`. The new Linux workflow has not been run
  from this local change, and no cloud deployment or registry publication occurred.
