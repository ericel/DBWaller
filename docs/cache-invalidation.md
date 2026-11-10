# Cache invalidation and in-flight loads

Within one `ShardedEngine` instance, `invalidate_key()` and `invalidate_tag()`
prevent loads admitted before the invalidation from subsequently publishing
cached values. Loads admitted afterwards may publish normally. This applies even
if the key is absent or no entry has registered the invalidated tag yet.

## External asynchronous fetches

Applications that fetch outside DBWaller must capture a ticket **before** fetching
and use conditional publication afterwards. The legacy `get -> fetch -> put`
sequence cannot detect invalidation during the fetch.

```cpp
#include <dbwaller/core/sharded_engine.hpp>

// On a cache miss, before starting an asynchronous backend request:
auto ticket = engine.begin_load(key);

// Fetch without any engine locks held; carry ticket into the completion handler.
auto [value, options] = fetch_from_backend();

auto status = engine.put_if_current(ticket, std::move(value), options);
if (status == dbwaller::core::PublishStatus::Invalidated) {
    // Discard the fetched result. Report invalidation or retry with a NEW ticket
    // and a NEW fetch under an application-defined, bounded retry policy.
}
```

`LoadTicket` owns its key and is copyable/movable. Its generation is opaque and
bound to its originating engine. A ticket from another engine, a previous engine
lifetime, or a moved-from ticket returns `PublishStatus::Invalidated`. Tickets do
not extend the engine's lifetime: the application must keep the engine alive
through all calls. Tickets remain usable across explicit compute shutdown, just
like other direct cache operations.

`PublishStatus::Stored` means the publication was accepted under the normal
TTL/SWR, tag-indexing, and eviction rules. It does not pin the entry in memory.
Tickets are not consumed by publication; copies remain valid until invalidation.
Use a new ticket for each new fetch. Two loads in the same generation have no
additional ordering guarantee.

## Misses, refreshes, and waiters

`get_or_compute()` and `trigger_refresh()` capture their tickets when installing a
flight, before scheduling or running the callback. Post-invalidation callers
cannot join a flight from an older generation. They can start a new flight for
the same key while the old callback is still running.

When an invalidated computation finishes, its result is discarded and its waiting
`get_or_compute()` callers receive `dbwaller::core::LoadInvalidatedError`, derived
from `std::runtime_error`. This also applies to callers sharing an invalidated
refresh and to callbacks that fail after invalidation. Gateway miss requests
propagate this exception. Callers may handle it with a bounded retry or an explicit
application error; DBWaller does not automatically retry backend calls.

Invalidation does not forcibly stop a callback or immediately wake its waiters.
The existing wait timeout can still expire first. A background refresh has no
direct result channel; its invalidated result is discarded. An old completion
retires only its own flight and cannot remove a newer one. Refresh failures update
stale-on-error metadata only if both the generation and the original cache entry
are still current, including when a deliberate write replaced that entry.

## Conservative generation and deliberate writes

The first implementation uses one generation for the whole engine. Invalidating
any key or tag also rejects pending loads for unrelated keys. Already cached
unrelated entries remain intact. This is necessary because compute callbacks can
return dependency tags only after fetching; per-tag generations would require
dependencies to be declared before fetching.

The generation uses a shared identity, replaced on every invalidation. Retained
tickets keep old identities alive, avoiding counter wraparound or accidental
reactivation after metadata cleanup or engine-address reuse. A publication mutex
serializes generation checks and the complete cache insertion with invalidation.
Lock order is publication mutex, then shard or flight-map locks, then individual
flight locks. No publication or cache lock spans a backend callback, queue
submission, or a wait for computation.

`put()` remains an unconditional deliberate write. It does not advance the
generation or supersede pending loads. To fence existing loads before a deliberate
write, invalidate first and then write. Fetch-derived writes, including external
asynchronous integrations such as Wahalao-web, must migrate to `begin_load()` and
`put_if_current()`; merely updating the library does not protect unconditional
application calls. No external application is changed by this library fix.

This contract does not revoke responses already read or computations already
settled, guarantee freshness from the backend, or propagate invalidation between
engine instances or Cloud Run instances. Those are application and distributed
system responsibilities.

## Regression verification

The concurrency suite includes controlled old/new fetches, absent keys and tags,
refresh success/failure, queued admission, shared waiters, flight ownership, and
external tickets. It uses gates, promises, and generous watchdogs without sleeps.
A barrier-based publication/invalidation test accepts either serialization order
and requires the same final cache state. Run the concurrency repeat, Tier A/B,
sanitizers, and Conan consumer checks documented in
[Compute lifecycle](compute-lifecycle.md) and [Contributing](../CONTRIBUTING.md).
