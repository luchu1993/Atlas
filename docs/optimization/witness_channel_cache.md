# Witness Outbound Channel Cache

**Status:** ✅ Shipped (`3ab62ca`, 2026-04-30).
**Subsystem:** `src/server/cellapp/witness.{h,cc}` (SendFn signature,
`OutboundChannel` hint), `src/server/cellapp/cellapp.cc::AttachWitness`
(channel resolution + lambda capture),
`src/server/cellapp/cellapp.{cc,h}::OnOutboundChannelDeath`
(synchronous invalidation hook).

## Design

`AttachWitness` resolves the observer's RUDP channel once and captures
the raw `Channel*` plus the observer's `EntityID` directly into the
`SendFn` lambdas. The hot path becomes a single `IsCondemned()` load
+ bundle append:

```cpp
auto ch_result = Network().ConnectRudpNocwnd(entity.BaseAddr());
if (!ch_result) return;
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

`SendFn` is `function<void(span<byte>)>` — the redundant
`observer_base_id` parameter that round-tripped to look up the same
observer was dropped at the same time.

## Lifetime contract

The captured raw `Channel*` is valid as long as either:

1. The channel hasn't been condemned (a normal disconnect moves the
   channel into `condemned_` for ~5 s before destruction; the
   in-lambda `IsCondemned()` guard short-circuits during that
   window), **or**
2. The witness is still alive.

Both teardown paths drop the witness before the channel destroys:

- **Orderly disconnect** — baseapp sends `DisableWitness`, cellapp
  tears down via `OnDisableWitness`.
- **Force-destroy under cascade** — the
  `kMaxCondemnedChannels = 4096` cap can force-destroy oldest
  condemned channels. `Witness::OutboundChannel()` exposes the
  captured pointer as a hint; cellapp registers
  `Network().SetDisconnectCallback(OnOutboundChannelDeath)` which
  scans `entity_population_` and disables matching witnesses
  **synchronously** before `disconnect_callback_` returns. Channel
  pointer stays valid through the witness drop; lambdas die first.

Disconnect-handler cost: O(N) over `entity_population_`; ≈ 25 µs at
500 cli, cheap on cellapp-peer / machined disconnects with no
matching witnesses.

## Caveats

- `NetworkInterface::SetDisconnectCallback` is **single-slot**.
  Cellapp now claims it. A future feature that also needs a
  disconnect hook must compose with `OnOutboundChannelDeath` —
  promote the callback to a fan-out signal.
- `aoi_map_` size and `WitnessMaxPeersPerTick` are unchanged. AoI
  hard-cap and multi-cellapp sharding remain the structural levers
  for breaking O(N²) total work scaling.
- Bundle / RUDP serialization is untouched. `Bundle::AddMessage`
  still runs `StartMessage` + `Serialize` + `EndMessage` per call.
  At current per-call cost this is no longer the dominant Pump term
  — revisit only if a future capture says otherwise.
