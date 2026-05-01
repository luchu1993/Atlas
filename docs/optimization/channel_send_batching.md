# Channel-Level Send Batching

**Status:** ✅ Shipped (`7cfbb27` plumbing → `4057994` default flip →
`938ca28` / `c44cb32` whitelist → `201a80a` cellapp dedupe →
`0bd984c` OOM-only threshold → `bd5ca36` correctness fixes).
**Subsystem:** `src/lib/network/{message.h,channel.{h,cc},reliable_udp.{h,cc},network_interface.{h,cc}}`,
`src/server/baseapp/baseapp.cc`, `src/server/cellapp/cellapp.cc`.

## Design

Each `MessageDesc` carries a two-axis classification:

```cpp
enum class MessageReliability : uint8_t { kReliable, kUnreliable };
enum class MessageUrgency     : uint8_t { kBatched, kImmediate };
```

`Channel::SendMessage` routes on `urgency`:

- **`kImmediate`** — append to bundle, `Send` (or `SendUnreliable`)
  now. One `sendto` per call. Used for handshake, login, RPC replies
  the caller is `co_await`ing on, and the PvP combat-command path.
- **`kBatched`** (default for new descriptors) — append to the
  channel's per-reliability deferred bundle, register the channel
  with `NetworkInterface::dirty_send_channels_`, return without a
  syscall. `NetworkInterface::DoTask` flushes all dirty channels
  once per dispatcher tick.

Threshold rules in the batched path:

- Bundle accumulates within the tick. A flush fires early only when
  the bundle exceeds the OOM-only safety threshold
  (`RudpProfile.deferred_flush_threshold`, default 60 KB) — well
  past `kMaxUdpPayload` so RUDP fragmentation handles the rest.
- Tick-end flush is the common path: one `sendto` per
  (channel × reliability × tick), regardless of how many
  descriptors fed in.
- TCP channels ignore `urgency`; the kernel write buffer + MSS
  segmentation already coalesce.

## `kImmediate` whitelist (PR-2 audit ground truth)

Categories that stay `kImmediate`:

| Category | Why |
|---|---|
| PvP command path (`Client*Rpc`, `InternalCellRpc`) | +1-tick (~67 ms) breaks the 1v1 / 2v2 / 4v4 PvP feel target |
| Login & handshake | Coroutine `co_await`; caller blocks on the syscall result |
| Disconnect / kick | State flips; user-visible behaviour gates on prompt delivery |
| Cellapp migration barrier (`OffloadEntity`, `EntityTransferred`, `BackupCellEntity`, `CellReady`) | Lifecycle ordering; reordering corrupts the migration handshake |
| All `*Ack` | Reply messages — sender is `co_await`ing |
| Machined control plane, manager registration, space topology | Low-frequency, latency-sensitive cluster control |
| Witness control (`EnableWitness` / `DisableWitness` / `SetAoIRadius`) | Configuration flips |

Categories flipped to `kBatched`:

| Category | Descriptors | Payoff |
|---|---|---|
| Replication delta | `ReplicatedReliableDeltaFromCell`, `ReplicatedBaselineFromCell` | Direct hit on the BaseApp `Channel::Send` hotspot |
| Ghost sync | `GhostDelta`, `GhostPositionUpdate`, `GhostSnapshotRefresh` | Collapses cellapp's `O(reals × haunts)` ghost broadcast to `O(haunts × tick)` |
| RPC forwarding | `CellRpcForward`, `SelfRpcFromCell`, `BroadcastRpcFromCell` | Cellapp → BaseApp → client chain coalesces per client |
| Ghost lifecycle | `CreateGhost`, `DeleteGhost` | Coalesces with the next ghost-update bundle to the same peer |
| Entity creation | `CreateBase`, `CreateBaseFromDb`, `CreateCellEntity`, `DestroyCellEntity` | Login-wave / instance-entry fan-out |
| Client event report | `ClientEventSeqReport` | Periodic acks merge into the next outbound bundle |

DBApp descriptors (`WriteEntity`, `CheckoutEntity`, …) stay
`kImmediate` — DB durability concerns dominate any batching win.

## Caveats

- **Latency floor.** Every batched message can sit up to one tick
  (≈ 67 ms BaseApp / 100 ms CellApp) before hitting the wire. Combat
  / hit-confirm paths must stay `kImmediate`; re-grep the C# combat
  pipeline whenever new client-message descriptors are added.
- **Reliable / unreliable order inversion.** A reliable-then-
  unreliable pair from the same caller used to go out in that
  order. After batching they live in separate per-reliability
  bundles and `FlushDeferred` ships unreliable first. Reliable-vs-
  unreliable pairs in this codebase are independent (volatile
  position vs semantic state) so this is benign — confirm before
  relying on cross-axis ordering.
- **Failure visibility.** `cwnd`-blocked failures surface at flush
  time, not at `SendMessage` return. Reliable retains the existing
  retransmit machinery; unreliable drops on the floor under
  back-pressure.
- **Memory cost.** Each `ReliableUdpChannel` retains two
  `std::vector<std::byte>` capacities (reliable + unreliable
  deferred bundles) until the channel is condemned. At 500 cli /
  1-BA the BaseApp peak Working Set rose ~+108 MB
  (≈ 500 × 210 KB / channel watermark). Linear in client count;
  halving topology halves the delta. Tunable knobs:
  drop `RudpProfile.deferred_flush_threshold` from 60 KB to 16 KB
  for tighter capacity at the cost of mid-tick flushes, or change
  `Bundle::Finalize` to reset capacity post-move.
- **`packet_filter_` interaction.** Filter runs once per flush over
  the whole bundle. Existing `compression_filter` operates on the
  full payload so it's neutral. Per-message-shape filters
  (encryption with per-message nonces) need a re-audit before
  enabling.
- **Test surface.** Send-then-assert patterns require a tick pump
  between send and assert under batching. The integration suite
  currently flushes via `NetworkInterface::FlushDirtySendChannels`
  in the test harness; new tests must follow the same pattern.

## Tuning

| Symptom | Knob |
|---|---|
| Mid-tick flushes during burst traffic | raise `RudpProfile.deferred_flush_threshold` |
| Working Set climbs over time | lower threshold (caps per-channel deferred capacity) |
| Slow-tick on flush | move flush off the tick driver via the network I/O thread design (future work) |
