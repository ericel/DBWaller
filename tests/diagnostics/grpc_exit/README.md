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
disprove the report. No suppressions, sleeps, forced thread termination, or
process-global OpenSSL cleanup changes are applied by DBWaller.

This isolates the observed report to the dependencies; it does not prove the SDK
has no other races. Revalidate the full client suite when updating dependencies.
