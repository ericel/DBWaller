# Compute lifecycle

The compute path keeps the existing public signatures, defaults, single-flight
behavior, and `RunInline` / `FailFast` options.

## Admission and backpressure

`ThreadPool::try_submit()` accepts a task only while admission is open and the
bounded queue has space. It returns `false` for a full queue or shutdown;
`try_submit_future()` returns `nullopt`, and `submit_or_throw()` throws
`std::runtime_error` in either case. Rejected callbacks are not executed by the
pool. These APIs take ownership of their callable arguments, including on rejection.

The engine retains its own callback until pool acceptance. Under `RunInline`, a
full queue executes that retained callback once on the submitting thread. Under
`FailFast`, the engine skips the callback and resolves the flight as a failure;
miss callers return an empty string and stale cache entries remain available under
their existing policy. This also wakes callers that joined a rejected refresh.
Callback-copy or scheduling failures retire the flight so a later call can retry.

With `enable_compute_pool = false`, compute runs inline with either backpressure
setting: there is no queue to saturate. Refresh is asynchronous when accepted by
the pool, but can block in inline or pool-disabled execution.

## Waiting and ownership

`compute_timeout_ms` limits a caller's wait on the flight's condition variable.
Zero means no wait timeout. Timing out does not cancel queued/active computation,
erase its flight, or by itself prevent a later successful cache publication.
Invalidation does prevent publication and separates old and new flights; see
[Cache invalidation](cache-invalidation.md) for `LoadInvalidatedError`, refresh
failure handling, and the external load-ticket API. Other callers in the same
generation can still share a computation. Inline execution is synchronous and cannot be
interrupted by this wait timeout.

Gateway miss and refresh callbacks own the raw key, copied request context,
policy/tags, and a shared adapter reference. They do not reference the gateway
request's stack or its `OpMeta` output after return. The caller may release its
adapter owner and request arguments after a timeout; the callback retains the
data it needs. Adapters must handle concurrent calls when the application permits
them and retain any external resources they themselves reference.

Direct engine callers are responsible for the lifetime of references captured by
their own callbacks. Synchronous `NoStore` bypass calls are owned by their calling
request, rather than the engine's compute pool.

## Shutdown

Thread-pool shutdown closes admission under the queue lock, requests cooperative
stop on workers, drains all previously accepted tasks, and joins the workers.
Queued tasks can therefore start after shutdown begins and see a requested stop
token. A queued future settles with its callback's value or exception before
shutdown returns, without waiting for the pool object to be destroyed. Exceptions
from fire-and-forget callbacks are contained so the worker can finish the queue;
such tasks have no result/error channel. `size()` is safe during shutdown and
returns zero after workers have joined.

Engine shutdown closes compute admission, wakes and joins the sweeper, drains the
compute pool, and waits for admitted API calls, including inline callbacks and
shared waiters. Work admitted before closure may finish and publish until shutdown
returns. Afterwards no accepted engine callback remains active. New
`get_or_compute()` calls return an empty string, and `trigger_refresh()` does
nothing. Explicit cache reads, writes, invalidation, and statistics remain usable
while the engine object is alive.

Concurrent and repeated shutdown calls are supported; each waits for completion.
No thread is detached and no adapter callback is forcibly terminated. A callback
that never returns can keep shutdown blocked. Calling shutdown from that same
pool's task or engine's compute callback (including callback-owned cleanup) is
outside the supported contract because it would wait for itself.

The owner must stop new external calls and join callers, including concurrent
shutdown callers, before destroying the engine or pool. Shutdown is a compute
completion boundary, not a replacement for normal object-lifetime synchronization.

## Regression checks

After configuring and building the tests as described in `CONTRIBUTING.md`:

```sh
ctest --test-dir build/build/Release -L concurrency --no-tests=error --output-on-failure --repeat until-fail:2
ctest --preset tier-a-pr --no-tests=error
ctest --preset tier-b-main --no-tests=error
```

The concurrency cases use controlled callbacks, promises, condition variables,
public counters, and generous watchdogs. The rejected-refresh case pauses a user
callable's destructor so another request can join before rejection cleanup. The
copy-failure case uses a throwing user callable. Neither requires private engine
hooks. The gateway lifetime case keeps the request frame separate for ASan's
stack-use-after-return detection.

For AddressSanitizer and UndefinedBehaviorSanitizer, use a separate build:

```sh
cmake -S . -B build/lifecycle-asan -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/build/Release/generators/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DDBWALLER_BUILD_TESTS=ON -DDBWALLER_BUILD_APPS=OFF \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -fsanitize-address-use-after-scope' \
  '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -g -DNDEBUG' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build/lifecycle-asan -j 4
ASAN_OPTIONS=detect_stack_use_after_return=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/lifecycle-asan -L concurrency --no-tests=error --output-on-failure
```

ThreadSanitizer uses a separate `build/lifecycle-tsan` build, with
`-fsanitize=thread -fno-omit-frame-pointer` as the C++ flags and
`-fsanitize=thread` as the executable linker flags. Do not combine TSan and ASan.
Public-header changes also require the `conan create` consumer check described in
`CONTRIBUTING.md`.
