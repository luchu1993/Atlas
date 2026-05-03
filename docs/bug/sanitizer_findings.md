# Sanitizer findings (open)

Bugs surfaced by the weekly Sanitizers workflow once its sccache backend
was repaired. They predate the CI fix and are deferred — fix individually
once the CI restructure settles.

Reproduce: trigger `Sanitizers` workflow on GitHub, or locally:
```bash
cmake --preset asan   && cmake --build build/asan  --config Debug
cmake --preset ubsan  && cmake --build build/ubsan --config Debug
ctest --build-config Debug --label-regex unit --output-on-failure
```

## ASAN

### A1 · heap-use-after-free in `RangeList::Remove` destructor path
- **Source:** `src/lib/space/range_list.cc:14` (`UnlinkX`), called from
  `src/lib/space/range_trigger.cc:46` (`RangeTrigger::~RangeTrigger`).
- **Trigger:** `RangeTrigger.PeerEntersOnBothAxesFiresOneOnEnter`.
- **Symptom:** writing to a freed `RangeListNode` while unlinking on
  destruction. Likely the surrounding `Space` already freed the node
  list before the trigger destructor runs.

### A2 · stack-use-after-scope in `Task<void>` await
- **Source:** `tests/unit/test_coro_task.cpp:66` (lambda body) reached
  through `sync_wait_void`.
- **Trigger:** `Task.VoidTask`.
- **Symptom:** the awaiter or coroutine frame outlives a stack-scoped
  capture. Likely a missing `std::move` / capture-by-value somewhere
  in the void-task path.

## UBSAN

### U1 · null pointer to nonnull arg in `CompressionFilter::SendFilter`
- **Source:** `src/lib/network/compression_filter.cc:20`.
- **Trigger:** `CompressionFilterTest.EmptyPacket`.
- **Symptom:** `memcpy` (or similar) called with a null source on the
  empty-packet path. Add an early-return for empty input.

### U2 · invalid vptr in `RangeList::UnlinkX` (same site as A1)
- **Source:** `src/lib/space/range_list.cc:14`.
- **Trigger:** `RangeTrigger.PeerEntersOnBothAxesFiresOneOnEnter`.
- **Symptom:** member access on an object whose vptr was clobbered by
  the prior free (paired with A1). Fixing A1 should clear this too.

### U3 · invalid vptr on `INativeApiProvider`
- **Source:** `src/server/cellapp/cellapp_native_provider.cc:253`.
- **Trigger:** `CellAppNativeProviderTest.ProximityEventIsNoopWhenCallbackNull`.
- **Symptom:** member call on the base interface vptr after the derived
  `CellAppNativeProvider` has been destructed; the test path goes
  through a callback held past the provider's lifetime.

### U4 · invalid vptr on `Channel` in `MachinedClient::IsConnected`
- **Source:** `src/lib/server/machined_client.cc:58`.
- **Trigger:** `EntityApp.BgTaskRunsOnBackgroundThread` (the
  `Connection refused` path during baseapp startup).
- **Symptom:** `Channel*` cached on the client survives past the
  channel object itself when the connection refused path tears down.
  Need to null the cached pointer on disconnect or guard with a
  weak handle.

### U5 · gtest internal — uninitialized bool (load value 223)
- **Source:** `_deps/googletest-src/googletest/include/gtest/gtest-assertion-result.h:161`.
- **Trigger:** runs immediately after U4 in the same suite.
- **Symptom:** likely a downstream effect of U4's UB corrupting the
  stack; revisit after U4 lands.
