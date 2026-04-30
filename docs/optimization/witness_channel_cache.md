# Witness Outbound Channel Cache

**Status:** ✅ Shipped (`0072` SendFn refactor, `0073` channel cache + disconnect callback, 2026-04-30).
**Priority:** P1 — surfaced as the dominant Pump cost at 500 clients.
**Subsystem:** `src/server/cellapp/witness.{h,cc}` (SendFn signature, OutboundChannel hint),
`src/server/cellapp/cellapp.cc::AttachWitness` (channel resolution + lambda capture),
`src/server/cellapp/cellapp.{cc,h}::OnOutboundChannelDeath` (synchronous invalidation hook).

## Why this matters

### Diagnosis (500-client capture, `e45866b`)

`Witness::Event::Send` was the dominant zone inside `Witness::Update::Pump` —
6.86 s out of 10.37 s Pump total = **70 %** of Pump CPU. Per-call cost
**805 ns** at 500 cli, **335 ns** at 200 cli (×2.4 retreated as scale grew).

The wrapper lambda installed by `AttachWitness` looked like:

```cpp
[this](EntityID observer_base_id, std::span<const std::byte> env) {
  auto* observer = FindEntityByBaseId(observer_base_id);   // hash lookup
  if (!observer) return;
  auto ch = Network().ConnectRudpNocwnd(observer->BaseAddr());  // hash lookup + dynamic_cast
  if (!ch) return;
  baseapp::ReplicatedReliableDeltaFromCellSpan msg{observer_base_id, env};
  (void)(*ch)->SendMessage(msg);
}
```

Two hash-map lookups (`entity_population_`, `channels_`) plus a `dynamic_cast`
ran every send. Both maps grow with active client count, so per-call cost
itself drifted upward as the population grew — the source of the ×2.4 retreat.

A side observation: `observer_base_id` was always equal to `owner_.Id()` at every
call site inside `Witness`, so the EntityID parameter on `SendFn` was dead weight
that just round-tripped to find the same observer.

## What landed

### 1. Drop the redundant EntityID parameter (`0072`, refactor)

`SendFn` becomes `function<void(span<byte>)>`. Cellapp lambda captures
`observer_id` directly. Pure refactor; no behavior change.

### 2. Resolve `Channel*` once, capture in lambda (`0073`)

```cpp
auto ch_result = Network().ConnectRudpNocwnd(entity.BaseAddr());
if (!ch_result) return;  // log-warn and bail
ReliableUdpChannel* const ch = *ch_result;
const EntityID observer_id = entity.Id();

entity.EnableWitness(
    aoi_radius,
    [ch, observer_id](std::span<const std::byte> env) {
      if (ch->IsCondemned()) return;
      baseapp::ReplicatedReliableDeltaFromCellSpan msg{observer_id, env};
      (void)ch->SendMessage(msg);
    },
    [ch, observer_id](std::span<const std::byte> env) {
      if (ch->IsCondemned()) return;
      baseapp::ReplicatedDeltaFromCellSpan msg{observer_id, env};
      (void)ch->SendMessage(msg);
    },
    hysteresis);
```

The hot path is now a single condemned-state load + bundle append. Two hash
lookups + dynamic_cast eliminated.

### 3. Synchronous invalidation on channel disconnect (`0073`)

The captured raw `Channel*` survives a normal disconnect because the channel
moves into `condemned_` for 5 s before destruction (`network_interface.cc::CondemnChannel`).
That's enough time for the orderly `DisableWitness` message from baseapp to
clean up the witness, and during the gap `IsCondemned()` short-circuits the
lambda safely.

The pathological case is the `kMaxCondemnedChannels = 4096` cap force-destroying
oldest condemned channels under cascade disconnect. To close that window:

- `Witness` exposes an `OutboundChannel()` hint set by `AttachWitness`.
- CellApp registers `Network().SetDisconnectCallback(OnOutboundChannelDeath)`.
- The handler scans `entity_population_` and disables witnesses whose hint
  matches the dying channel — runs **synchronously** before
  `disconnect_callback_` returns, so the channel pointer is still valid when
  we drop the witnesses (and therefore the lambdas).

Cost on disconnect: O(N) scan of `entity_population_`. At 500 cli ≈ 25 µs;
even cellapp-peer / machined disconnects (no matching witnesses) are cheap.

## Profiling Evidence

500-client baseline, 120 s, `e45866b` → `3ab62ca`:

| Zone | Old mean | New mean | Δ | Old total | New total | Δ |
|---|---|---|---|---|---|---|
| `Witness::Event::Send` (per call) | 805 ns | **322 ns** | **-60 %** | 6 856 ms | 2 538 ms | -63 % |
| `Witness::Update::Pump` | 9.5 µs | 5.2 µs | -46 % | 10 369 ms | 5 612 ms | -46 % |
| `CellApp::TickWitnesses` | 11.0 ms | 7.6 ms | -31 % | 21 394 ms | 14 754 ms | -31 % |
| `Tick` (cellapp) | 12.8 ms | 9.2 ms | -28 % | 25 026 ms | 18 027 ms | -28 % |
| `Tick` max (cellapp) | 41.7 ms | **28.2 ms** | -32 % | — | — | — |
| `Witness::Update::PriorityHeap` | 5.5 µs | 3.3 µs | -41 % | — | — | — |

The 500-cli `Event::Send` mean (322 ns) is now **slightly below** the 200-cli
mean from before this change (335 ns) — the scale-induced per-call retreat is
gone.

`PriorityHeap` -41 % is collateral: the heap-build loop iterates the same
`aoi_map_` entries that the hot Event::Send path was thrashing through; once
the lookup-pressure on neighboring lines disappears, the heap-build code reads
warmer cache.

### Stress-run health (500 cli, 120 s)

| Signal | Old (`e45866b`) | New (`3ab62ca`) | Δ |
|---|---|---|---|
| `echo_loss` | 9 640 / 89 317 (10.8 %) | **158 / 103 042 (0.15 %)** | -98 % |
| `echo_rtt` p99 | 441 ms | **61 ms** | -86 % |
| `auth_latency` p99 | 804 ms | **208 ms** | -74 % |
| `timeout_fail` | 53 | **0** | clean |
| `unexpected_disc` | 64 | **0** | clean |
| `login_fail` | 644 | **0** | clean |
| `cell_ready / entity_transferred` | 2 543 / 2 910 (87 %) | **3 183 / 3 183 (100 %)** | clean |
| `online_at_end` | 237 | **482** | +103 % |

500-cli runs the way 200-cli ran before: zero failure-class signals.

## Caveats / known boundaries

- **Lifetime contract.** The lambda's captured `Channel*` is valid as long as
  either (a) the channel hasn't been condemned, or (b) the witness is still
  alive. Both cellapp-side teardown paths (`OnDisableWitness` from baseapp;
  `OnOutboundChannelDeath` from disconnect callback) tear the witness down
  before the channel is destroyed. The `IsCondemned()` guard is the in-lambda
  safety net for the brief window between condemn and witness teardown.
- **Single-slot disconnect callback.** `NetworkInterface::SetDisconnectCallback`
  is single-slot; cellapp now claims it. If a future feature also wants to hook
  channel disconnect, it must compose with `OnOutboundChannelDeath` — likely by
  promoting the callback to a fan-out signal.
- **Test churn.** ~25 sites (test lambdas) had to drop the EntityID parameter.
  Pure mechanical change; preserved by keeping the `SendFn` alias and just
  changing the signature.

## What did **not** change

- Bundle / RUDP serialization layer is untouched. `Bundle::AddMessage` →
  `StartMessage` + `Serialize` + `EndMessage` still runs per call. Stage 2
  (span-message fast-path) is the next lever inside `SendMessage` if needed
  — but at 322 ns / Event::Send, single-call cost is no longer the dominant
  Pump term.
- `aoi_map_` size and `WitnessMaxPeersPerTick` cap are unchanged. AoI
  hard-cap and multi-cellapp sharding remain the structural levers for
  breaking the O(N²) total work scaling — see README's strategic conclusion.
