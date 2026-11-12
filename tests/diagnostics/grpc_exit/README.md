# Dependency-only process-exit reproducer

This diagnostic links gRPC/OpenSSL and includes no DBWaller code. It initializes
OpenSSL before constructing and destroying ten local TLS servers/channels. This
matches the dependency initialization order of an application that uses crypto
before creating its cache client. It is not a normal CTest test or a workaround.

Use the fully instrumented Conan dependency graph described in
`docs/daemon.md` and the TLS fixture files created by the `daemon_fixtures` test:

```sh
cmake -S tests/diagnostics/grpc_exit -B build/grpc-exit \
  -DCMAKE_TOOLCHAIN_FILE=/absolute/path/to/tsan/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/grpc-exit
build/grpc-exit/grpc_exit /absolute/path/to/server.crt /absolute/path/to/server.key
```

On macOS / AppleClang 21 with fully instrumented gRPC 1.84.0 and OpenSSL 3.5.6,
the second run reported process-exit races in `init_thread_deregister` and
`OPENSSL_sk_num`, matching the new client test's report, plus
`ossl_lib_ctx_get_data`. OpenSSL's automatic cleanup overlapped gRPC worker
thread-local cleanup. This is timing-dependent; one successful run does not
disprove the report. That original run used no suppressions, sleeps, forced
thread termination, or explicit OpenSSL startup policy.

The default command preserves the original reproducer. To exercise the same
explicit early initialization policy exposed by the SDK helper, append:

```sh
build/grpc-exit/grpc_exit /absolute/path/to/server.crt /absolute/path/to/server.key --process-lifetime-tls
```

This calls `OPENSSL_init_ssl(OPENSSL_INIT_NO_ATEXIT, ...)` before the first SHA256
call or thread creation. Twenty fresh-process runs passed under the fully
instrumented TSan graph. No OpenSSL cleanup is manually invoked; process-global
allocations are reclaimed by the OS. The SDK's `remote_tls_policy` test separately
checks real exit-handler behavior deterministically and proves that calling the
helper after SHA256 is too late.

A subsequent baseline check on 2026-11-12 passed four fresh-process runs, then
reported three TSan warnings on the fifth, including `OPENSSL_sk_num`. This
confirms that the original failure remains reproducible without the startup policy.

This is an application startup mitigation for the current dependency contract,
not proof that all gRPC threads are joined or that arbitrary host initialization
orders are safe. See [TLS startup contract](../../../docs/remote-cpp.md#process-wide-tls-startup-contract).
Revalidate the full client suite when updating dependencies.
