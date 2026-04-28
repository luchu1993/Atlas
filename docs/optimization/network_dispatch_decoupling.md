# Receive / Dispatch Decoupling (Plan B)

**Status:** 📋 design-only · briefly implemented (`49703d8`) and
**reverted** (`a78d540`) after the 500-cli single-baseapp validation
showed B was a net regression for Atlas's mixed coroutine + RPC chain
workload. See **Empirical revert reasoning** below before proposing
B again.

**Forward path:** the 500-cli single-baseapp goal will be pursued
via **A2** — extending Plan A's deadline-yield from packet-level
into `DispatchMessages` itself (handler-level yield + resume).  A2
preserves Plan A's "synchronous when fast, yield when slow"
semantic, which avoids the multi-hop RPC-chain regression that
killed B in practice.

**Subsystem:** `src/lib/network/reliable_udp.{h,cc}`,
`src/lib/network/network_interface.{h,cc}`,
`src/lib/server/server_app.cc` (tick driver), application handlers
that assume synchronous dispatch.

**Relationship to Plan A:** Plan A (commit `2c3ced4`) yields *within*
FlushReceiveBuffer when the per-callback deadline is hit and reschedules
the rest. It preserves the contract that an arriving in-order packet
fires its application handler before the receive callback returns, just
sometimes across multiple receive callbacks. **Plan B unconditionally
defers all dispatch to a tick-time drain, regardless of load.** The
data structure (a hot-channel list / per-channel ready queue) is the
same; only the trigger point differs. **Building A leaves the scaffold
B needs**, so going from A→B is incremental, not a rewrite.

## Empirical revert reasoning (post-`49703d8`)

B was implemented and run against the 500-cli single-baseapp
shape (capture `49703d8_20260429-003653`).  Headline numbers vs
Plan A (`8846cc1`) on the same workload:

| Signal | Plan A | Plan B | Δ |
|---|---|---|---|
| `OnRudpReadable` max | 166 ms | **11.4 ms** | ✅ ~14× |
| `OnRudpReadable` mean | 2.74 ms | **0.41 ms** | ✅ ~7× |
| `Slow tick` (baseapp) | 16 (max 214 ms) | 3 (max 168 ms) | ✅ -81 % |
| **`DoTask` max** | trivial | **112.5 ms** | ❌ new hotspot |
| `Channel::Dispatch` max | 166 ms | 111.7 ms | -33 % |
| **`cell_ready`** | 3 395 | **1 204** | ❌ -65 % |
| `timeout_fail` | 0 | **133** | ❌ login chain regression |
| `auth_latency` p99 | 5.22 s | **6.28 s** | ❌ +20 % |
| `bytes_rx_per_sec` | 1.97 MB/s | **0.58 MB/s** | ❌ -71 % (downstream of timeouts) |
| `aoi_prop_update` | 3 499 k | **1 008 k** | ❌ -71 % (ditto) |

**Root cause B did not eliminate:** a single MTU-sized reliable
packet from cellapp can bundle 10+ entity property handlers.
`Channel::DispatchMessages` runs all of them in one synchronous
pass.  B moved that 100+ ms pass from `OnRudpReadable` (where Plan A
saw it) into `DoTask` (where B sees it now).  The dispatcher thread
still stalls; only the zone label changed.

**The cost B paid that broke things:** every reliable packet now
has +0–67 ms of tick-cadence delay before its handler runs.
Atlas's login flow chains 4 + RPC hops (loginapp → DBApp →
BaseAppMgr → BaseApp → cellapp setup → cell ready).  At 1 tick per
hop the chain accumulates 200–400 ms of pure wait per login,
which under 500-cli concurrent ramp pushes a third of clients past
their 20 s connect timeout.  `cell_ready: 3395 → 1204` was the
operator-visible failure mode.

**Conclusion:** B is theoretically clean but mismatched to
Atlas's RPC-heavy login / setup paths.  The right surgery is
finer-grained yielding (A2), not architectural decoupling.  Keep
this design doc for the case where Atlas migrates wholesale to
tick-cadence semantics — at that point B becomes coherent with the
broader engine model and the RPC-chain cost is paid by the design,
not by the receive path.

## Why we did not implement B initially

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
