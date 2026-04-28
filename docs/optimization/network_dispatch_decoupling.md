# Receive / Dispatch Decoupling (Plan B)

**Status:** ✅ Shipped — replaces Plan A's intra-flush yield
(`2c3ced4`) after the 500-cli baseline (`8846cc1`) showed A's
per-packet bound was too coarse to eliminate Slow-tick warnings
(still 16, max 214 ms). Plan B was prepared as a design-only
fallback; the data made the case for promoting it to the actual
fix.

**Subsystem:** `src/lib/network/reliable_udp.{h,cc}`,
`src/lib/network/network_interface.{h,cc}`,
`src/lib/server/server_app.cc` (tick driver), application handlers
that assume synchronous dispatch.

**Relationship to Plan A:** Plan A (commit `2c3ced4`) yielded *within*
FlushReceiveBuffer when the per-callback deadline was hit and
rescheduled the rest. It preserved the contract that an arriving
in-order packet fires its application handler before the receive
callback returns, just sometimes across multiple receive callbacks.
**Plan B unconditionally defers all dispatch to NetworkInterface's
DoTask drain (frequent-task cadence), regardless of load.** The data
structure (a hot-channel set) is the same; only the trigger point
moves from "inside OnDatagramReceived" to "DoTask sweep". A's
hot-channel plumbing was the scaffold B reused — the actual
promotion was ~30 lines of behavioural change (split
OnDatagramReceived into NoFlush + sync wrapper; swap which one the
production path calls; remove the in-callback drain at the tail of
OnRudpReadable).

## What we shipped

- `ReliableUdpChannel::OnDatagramReceived(span)` — synchronous
  wrapper retained for test ergonomics.  Does parse + enqueue + an
  immediate `FlushReceiveBuffer(TimePoint::max())`.
- `ReliableUdpChannel::OnDatagramReceivedNoFlush(span)` — the new
  production receive path.  Parses headers, advances ACK / SACK /
  receive-window state, enqueues into `rcv_buf_`, and notifies the
  hot-callback iff `HasReceiveBacklog()` (i.e. `rcv_buf_` now
  contains `rcv_nxt_`).  Does NOT dispatch.
- `NetworkInterface::OnRudpReadable` — calls
  `OnDatagramReceivedNoFlush`.  Drops the tail
  `DrainHotChannels(deadline)` that Plan A added; receive is now
  pure parse + enqueue.
- `NetworkInterface::DoTask` — drains hot channels with budget
  `kReadableCallbackBudget / 2` (5 ms) every dispatcher
  frequent-task pass (~5 kHz on the 500-cli capture, so effective
  drain capacity is whatever the dispatcher schedules).
- `ReliableUdpChannel::FlushReceiveBuffer(deadline)` — kept its
  Plan A contract (always make progress on at least one packet,
  yield after each delivery if deadline passed).  Now invoked
  exclusively from `DrainHotChannels` and from the legacy test
  wrapper.
- Tests: `pump_datagrams` helper unchanged (uses the sync wrapper);
  `NoFlushOnReceiveAndDeadlineYieldOnExternalFlush` exercises the
  Plan B contract directly.

All 119 tests (105 unit + 14 integration) pass.

## Original "why not B initially" — preserved for the record

The 500-client / 2-baseapp baseline (`86aceee`) showed the production
deployment shape — 250 clients per baseapp — already healthy with
`echo_rtt p99 = 106 ms`. The remaining "single baseapp at 500 clients"
goal is achievable via Plan A's surgical fix without paying B's costs:

1. **Latency floor for every packet rises.** B holds every packet for
   up to 1 tick (≈ 67 ms at 15 Hz). Every RPC reply, every position
   update, every Echo round-trip eats the tick cadence. Action-MMO
   handfeel is more sensitive to *consistent* delay than to *rare*
   tail spikes.
2. **Multi-hop RPC chains compound the cost.** Atlas's login flow is
   4 RPC hops (loginapp → DBApp → BaseAppMgr → BaseApp). Under B,
   each hop adds 0–67 ms of tick-cadence delay; worst-case chain adds
   ≈ 268 ms to `auth_latency` purely from B. At 250 cli/proc baseline
   `auth_latency p99` is 193 ms — B would push it past 400 ms.
3. **Test surface large.** Every "send → assert receiver acted on it"
   pattern across the unit and integration suites would need a tick
   pump between send and assert. Estimated several hundred test
   touches.
4. **Risk vs reward.** A removes the 200 ms cascade stalls without
   touching dispatch semantics. B removes the same stalls *and*
   restructures the model. Until profile data shows A insufficient,
   the risk is unwarranted.

## When B becomes worth doing

Open this doc when one of these signals appears in a fresh capture:

- **A is in place** and `Slow tick` warnings on baseapp persist above
  ~ 5 events per 120 s capture at the target client count.
- **OnRudpReadable max** stays > 50 ms after A — i.e. the per-callback
  yield isn't catching enough cascade depth, or the application
  handlers themselves contain individual calls > 50 ms.
- **Atlas's network model migrates to tick-cadence** for unrelated
  reasons (e.g. lockstep / replay determinism / cross-region time
  synchronization). At that point B is a natural fit, not an
  imposition.
- **Per-process target rises to 1 000+ clients per baseapp** without
  multi-threading the dispatcher. A's "yield within callback" gives
  a constant-factor headroom; B gives a structurally different
  scheduling model that may scale further.

## Design

### High-level shape

```
┌─────────────────────────────────────────────────────────────┐
│ Dispatcher thread (single-threaded)                         │
│                                                             │
│  socket readable → OnRudpReadable                           │
│  ─────────────────                                          │
│   for each datagram in OS recv queue:                       │
│      RecvFrom + parse header + ProcessUna/ProcessAck        │
│      EnqueueForDelivery(seq, payload)        ← buffer only  │
│      mark channel as "ready to dispatch"                    │
│   (no DispatchMessages, no application handler)             │
│                                                             │
│  tick boundary → ServerApp::AdvanceTime                     │
│  ────────────────                                           │
│   ProcessReadyChannels(deadline = tick budget * α)          │
│   for each ready channel (round-robin or priority):         │
│      while deadline not hit AND channel has rcv_nxt_:       │
│         deliver next in-order packet                        │
│         DispatchMessages (the application stuff)            │
│   channels not drained stay in ready set for next tick      │
└─────────────────────────────────────────────────────────────┘
```

### File-by-file changes (rough)

| File | Change | LOC |
|---|---|---|
| `reliable_udp.h` | Drop synchronous `FlushReceiveBuffer` call from `OnDatagramReceived`; expose `DrainPending(deadline)` similar to A's API | ~40 |
| `reliable_udp.cc` | Remove the `FlushReceiveBuffer(...)` call inside `OnDatagramReceived`; keep `EnqueueForDelivery`; mark hot-callback unconditionally on enqueue | ~25 |
| `network_interface.{h,cc}` | Repurpose `hot_channels_` → `ready_channels_`; expose `DrainReadyChannels(deadline)` for `ServerApp` to call per tick; remove the in-callback drain at the tail of `OnRudpReadable` | ~50 |
| `server_app.cc` | Insert `network_interface_->DrainReadyChannels(deadline)` early in `AdvanceTime`, before per-app `OnTickComplete` | ~10 |
| `tests/unit/*.cpp` | Add an explicit `dispatcher.PumpReady()` between "send" and "assert delivered" in every test that depended on synchronous dispatch | ~100–300 across suites |
| Application handlers that assume sync delivery | Audit — most are already coroutine-based via RPC registry, which is timing-tolerant; some bare callback paths may need re-shaping | unknown until enumerated |

Total: ~250–500 LOC of code + the test sweep.

### Key invariants to preserve

- **Order of in-order delivery within a channel.** The `rcv_buf_` →
  `rcv_nxt_++` → DispatchMessages sequence stays atomic per packet;
  only the moment when DispatchMessages runs moves.
- **ACK / cumulative-ACK timing.** `ProcessUna` / `ProcessAck` /
  `ScheduleDelayedAck` continue to fire from `OnDatagramReceived`, so
  the sender's congestion window isn't artificially inflated by
  delayed dispatch on the receiver side.
- **Channel condemn / disconnect.** `ready_channels_` holds raw
  pointers; `CondemnChannel` must scrub on teardown — same pattern
  Plan A already established.

### Budget allocation

The tick-time drain needs a budget. Suggested initial values:

```
const auto kTickReceiveDrainBudget = TickPeriod() * 0.5;
```

i.e. spend at most half of the inter-tick gap draining receives,
leaving the other half for actual game logic. Concrete numbers:

| App | TickPeriod | Drain budget |
|---|---|---|
| BaseApp / LoginApp / DBApp | 67 ms (15 Hz) | 33 ms |
| CellApp | 100 ms (10 Hz) | 50 ms |

If the drain consistently hits its budget, the channel ready queue
grows; in practice this means the system is at the structural CPU
ceiling and either multi-threading or another baseapp is needed.

### Fairness across channels

A naive `for (ch : ready_channels_) drain(ch)` starves later channels
when an earlier one's backlog fills the budget. Two acceptable
schemes:

- **Per-channel slice cap**: each channel gets `budget / |ready|`
  worth of time per tick. Fair, simple, but small backlogs that
  could finish in one slice instead carry forward.
- **Round-robin with running cursor**: maintain a `next_to_drain`
  cursor; each tick starts from the cursor and advances it. Channels
  that yielded last tick come up first this tick. No per-channel
  arithmetic, just FIFO.

Round-robin is cheaper and matches the existing witness-update
priority queue idiom.

### Migration path from A

Plan A leaves us with:
- `ReliableUdpChannel::FlushReceiveBuffer(deadline)` — already
  deadline-aware
- `ReliableUdpChannel::SetHotCallback` — registration plumbing
- `NetworkInterface::hot_channels_` — set of channels needing more
  flushing
- `NetworkInterface::DrainHotChannels(deadline)` — bounded drain

Migration to B is roughly:
1. In `OnDatagramReceived`, **always** notify the hot-callback after
   `EnqueueForDelivery`, regardless of whether
   `FlushReceiveBuffer(now)` would have made progress. (One-line
   change — invert the branch.)
2. **Remove** the `FlushReceiveBuffer(deadline)` call from inside
   `OnDatagramReceived` so receive never dispatches.
3. **Remove** the `DrainHotChannels` call at the tail of
   `OnRudpReadable` (no in-callback drain).
4. **Add** a `DrainReadyChannels` invocation at the head of every
   `ServerApp::AdvanceTime`.
5. Audit + update tests.

The first three steps are < 30 LOC. The fourth step is one new call.
The fifth step is the bulk of the work.

## Risks specific to B

- **Coroutine-based RPC reply timing changes.** A reply that arrives
  on the network "this tick" but is dispatched "next tick" delays a
  coroutine resumption by one tick. For chains > 2 hops this
  compounds. Mitigation: identify hot-path RPC chains and consider
  same-tick delivery exemption (would re-introduce the cascade
  problem — usually not worth it).
- **`Channel::SendMessage` from inside a handler now happens on the
  tick thread, not the receive callback.** The transmit queue
  interactions (cwnd, retransmit timer, delayed ACK) have implicit
  timing relationships with receive that get reshuffled. Need to
  confirm none of those depend on synchronous receive→send ordering.
- **Latency budget surprises in tests with synthetic timestamps.**
  Tests that use `Clock::now()` to gate behavior may behave
  differently when dispatch is deferred. Likely a few flakes to fix.

## Validation plan when implementing B

1. Re-run 500-cli single-baseapp baseline (same params as `b1782e5`)
   and compare:
   - `OnRudpReadable` max should be < 5 ms (only RecvFrom +
     EnqueueForDelivery work).
   - `Slow tick` count should be 0.
   - `auth_latency` p99 — accept up to 2× degradation as the cost.
   - `echo_rtt` p99 should drop dramatically (no cascade stalls).
2. Re-run 100-cli baseline (sparse load) and check:
   - `auth_latency` p50 — make sure light-load RPC is not visibly
     worse. If p50 climbs > 100 %, B is paying too much in the common
     case and may not be worth keeping.
3. Re-run multi-cellapp / multi-baseapp combinations to confirm the
   tick-cadence model doesn't regress the cross-process RPC chains
   that drive Offload, EntityTransferred, etc.

## Cookbook for the future implementer

- Start by adding `NetworkInterface::DrainReadyChannels(TimePoint
  deadline)` as a public method that does what A's `DrainHotChannels`
  already does.
- Wire it from `ServerApp::AdvanceTime` first, while keeping A's
  in-callback drain in place. Verify both drains coexist without
  deadlocking — the in-callback drain just becomes a fast-path that
  rarely has anything to do.
- Once that's stable, remove the `FlushReceiveBuffer` call inside
  `OnDatagramReceived` and the in-callback drain. Tests will start
  failing; fix them by adding tick pumps where needed.
- Last step: rebaseline against the 500-cli capture and compare to
  the post-A numbers.
