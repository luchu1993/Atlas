# Channel-Level Send Batching (Implicit Bundle Coalescing)

**Status:** 📋 design-only · not implemented · scoped follow-up to the
500-client baseline (`b1782e5`, 2026-04-28). Open this doc when the
BaseApp `Channel::Send` line in the README's 500-client table
(`6.30 M calls / 31.8 % CPU`) becomes the next bottleneck after
network-dispatch decoupling lands, or when a fresh capture shows
`sendto` syscall count dominating BaseApp CPU.

**Subsystem:** `src/lib/network/channel.{h,cc}`,
`src/lib/network/reliable_udp.{h,cc}`,
`src/lib/network/network_interface.{h,cc}`,
`src/lib/network/message.h` (descriptor flag),
`src/server/baseapp/baseapp.cc` (caller migration),
`src/server/cellapp/cellapp.cc` (caller migration / dedupe with
`pending_witness_channels_`).

**Relationship to existing infrastructure.** Two coalescing layers
already exist and continue to work — this proposal makes the
*third* layer (caller-driven `BufferMessageDeferred` opt-in) the
default and removes the silent foot-gun where new code defaults to
the immediate path:

| Layer | Where | Coalesces | Currently used by |
|---|---|---|---|
| TCP write buffer | `tcp_channel.cc:180-253` | Multiple `DoSend` into one `socket_.Send` per writable callback | All TCP channels (automatic) |
| Tick-end flush set | `cellapp.cc::pending_witness_channels_` + `FlushDeferred` | Multiple `BufferMessageDeferred` into one `sendto` per (channel, reliability) per tick | Witness reliable / unreliable delta on cellapp |
| **Proposed: implicit channel batching** | `Channel::SendMessage` default path | Same coalescing as the tick-end set, but driven by the channel itself, not the caller | All RUDP channels (proposed default) |

## Why this matters

### Diagnosis (from a 2026-04-29 audit)

A grep over `src/` for `BufferMessageDeferred` callers found **only
one user** — `cellapp.cc::TickWitnesses`. Every other RUDP send path
goes through naked `Channel::SendMessage` and pays one `sendto` per
message:

| Caller | File:line | Frequency | Channel direction |
|---|---|---|---|
| `BaseApp::OnReplicatedReliableDeltaFromCell` | `baseapp.cc:984` | Per Path-#2 reliable delta from cellapp (HP / state / inventory) | Client (RUDP) |
| `BaseApp::RelayRpcToClient` | `baseapp.cc:936`, `:943` | Per CellApp→Client RPC | Client (RUDP) |
| `BaseApp::OnReplicatedBaselineFromCell` | `baseapp.cc:1027` | Per baseline snapshot | Client (RUDP) |
| `CellApp` Ghost broadcast loop | `cellapp.cc:1523`, `:1538`, `:1551` | Per Real entity per haunt per replication pump | Inter-cellapp peer (RUDP) |
| `CellApp` GhostSetNextReal / DeleteGhost / OffloadEntity | `cellapp.cc:1486`, `:1495`, `:1583` | Per offload op | Inter-cellapp peer (RUDP) |

The 500-client BaseApp baseline measures **6.30 M `Channel::Send`
calls / 31.8 % CPU** over a 137 s capture — most of that is
serialization + `sendto` overhead on these naked-send paths. The
cellapp witness path already coalesces (see the README "Per-tick
witness send batching" line, `0dced2f`, 1.80 M → 124 k calls), so
the existing infrastructure works; it is just that callers have to
*remember* to use `BufferMessageDeferred` and most don't.

### Why opt-in batching is the wrong default

`BufferMessageDeferred` works but has two problems as the public
API:

1. **Easy to miss.** New code paths default to the immediate
   `SendMessage` and stay there until someone runs a profile and
   notices. The cellapp ghost broadcast (`cellapp.cc:1523`) is
   right next to the witness path that *does* batch — even with
   the pattern under their nose, the author wrote naked sends.
2. **Forces caller-side flush plumbing.** Every batching site has
   to maintain its own `pending_*_channels_` set and remember to
   call `FlushDeferred` at the right boundary. `cellapp.cc` does
   this for witnesses; nothing else does. Three more call sites
   means three more sets and three more flush points.

## Proposal

### Two-axis message attribute

Add a second axis to `MessageDesc` next to `reliability`:

```cpp
// network/message.h
enum class MessageUrgency : uint8_t {
  kBatched,    // default — append to channel bundle, flush at tick or threshold
  kImmediate,  // legacy semantics — flush as soon as SendMessage returns
};

struct MessageDesc {
  MessageID id;
  std::string_view name;
  MessageLengthStyle length_style;
  int32_t fixed_length;
  MessageReliability reliability{MessageReliability::kReliable};
  MessageUrgency urgency{MessageUrgency::kBatched};   // NEW

  [[nodiscard]] constexpr auto IsImmediate() const -> bool {
    return urgency == MessageUrgency::kImmediate;
  }
};
```

`reliability` (reliable vs unreliable) and `urgency` (batched vs
immediate) are independent — every combination is meaningful:

| Reliability × Urgency | Use case |
|---|---|
| Reliable + Batched | Default for state replication, RPC results, inventory updates |
| Reliable + Immediate | Handshake, login, channel teardown, anything where caller awaits the syscall result |
| Unreliable + Batched | Volatile position deltas — already covered by the witness path today |
| Unreliable + Immediate | One-off telemetry pings where ordering inside a tick matters |

The default for new messages stays `kBatched`; `kImmediate` is a
deliberate opt-out per descriptor.

### Channel-level behaviour change

`Channel::SendMessage` becomes:

```cpp
auto Channel::SendMessage(MessageID id, std::span<const std::byte> data) -> Result<void> {
  MessageDesc desc{id, "", MessageLengthStyle::kVariable, -1};
  if (auto* found = interface_table_.Find(id)) desc = *found;

  if (desc.IsImmediate()) {
    // Existing path — append to bundle_, flush now.
    bundle_.StartMessage(desc);
    bundle_.Writer().WriteBytes(data);
    bundle_.EndMessage();
    return desc.IsUnreliable() ? SendUnreliable() : Send();
  }

  // Batched path — append to the per-(channel, reliability) deferred
  // bundle, register channel as dirty with NetworkInterface, return.
  // No syscall here.
  return BufferMessageDeferredInternal(desc, data);
}
```

`BufferMessageDeferredInternal` (renamed from the public
`BufferMessageDeferred`) does:

1. Append to `deferred_reliable_bundle_` or
   `deferred_unreliable_bundle_` based on `desc.reliability`.
2. If the resulting bundle size **≥ flush threshold** (see below),
   call `FlushDeferred` immediately for that reliability and
   continue.
3. Otherwise, register the channel in
   `NetworkInterface::dirty_channels_` and return.

Two flush triggers:

- **Size-driven** — bundle reaches the size threshold mid-tick.
  Avoids unbounded growth and prevents the next message from
  triggering RUDP fragmentation.
- **Time-driven** — `NetworkInterface::DoTask` (already runs each
  dispatcher tick) iterates `dirty_channels_`, calls
  `FlushDeferred` on each, clears the set.

### Flush threshold

The threshold is **`kMaxUdpPayload - epsilon`**, not the IP MTU
directly. Rationale:

- `kMaxUdpPayload = kMtu - kMaxHeaderSize` already lives at
  `reliable_udp.h:37` (today: `1500 - 21 = 1479`).
- A bundle larger than `kMaxUdpPayload` triggers `SendFragmented`
  (`reliable_udp.cc:113`), which splits across multiple datagrams
  *and* multiple unacked-slots — exactly the cost we are trying to
  avoid.
- We need a small headroom (`epsilon ≈ 80 bytes`) so the *next*
  appended message — whose size is unknown at flush-decision time —
  doesn't push the bundle past the limit and force a fragmentation
  on the very next call.

So: flush when `bundle.size() + epsilon ≥ kMaxUdpPayload`. Concrete
initial value: **flush at 1400 bytes**, leaving ~80 bytes of
headroom. Tunable.

For TCP channels the threshold is moot — the TCP write buffer
already coalesces and the kernel will MSS-segment whatever we
hand it. Recommend keeping the same threshold so `Channel` doesn't
need to know what subclass it is, but the threshold simply never
fires in practice (TCP messages are typically much smaller than
1400 B in this codebase).

### Tick-driven flush integration

`NetworkInterface` already has:

- `DoTask()` — runs each dispatcher tick
- `hot_channels_` set — repurpose pattern for receive-side hot
  channels in network_dispatch_decoupling

Add a sibling for the send side:

```cpp
// network_interface.h
private:
  std::unordered_set<Channel*> dirty_send_channels_;
public:
  void MarkChannelDirty(Channel& ch) { dirty_send_channels_.insert(&ch); }
  void FlushDirtySendChannels();
```

`DoTask` calls `FlushDirtySendChannels()` once per tick. The
function iterates the set, calls `FlushDeferred` on each channel,
clears the set. Channels that became condemned mid-tick are
scrubbed the same way `hot_channels_` already handles condemnation.

## File-by-file changes (rough)

| File | Change | LOC |
|---|---|---|
| `message.h` | Add `MessageUrgency` enum + `urgency` field + `IsImmediate()` helper | ~15 |
| `channel.{h,cc}` | Reroute `SendMessage` based on `urgency`; expose batching from base class (today RUDP-only) — TCP can no-op or fall through to existing buffer | ~40 |
| `reliable_udp.{h,cc}` | Make `BufferMessageDeferred` private, expose new internal entry point that takes `MessageDesc`; add size-threshold check; call `network_interface_->MarkChannelDirty(*this)` | ~30 |
| `network_interface.{h,cc}` | Add `dirty_send_channels_` set, `MarkChannelDirty`, `FlushDirtySendChannels`; wire into `DoTask` | ~50 |
| Generated `MessageDesc` instances (entitydef codegen) | Default `urgency = kBatched`; mark handshake / login / disconnect as `kImmediate` | ~20 (codegen template) |
| Hand-written `MessageDesc` (server framework messages: `OffloadEntity`, `GhostSetNextReal`, etc.) | Audit each; mark deliberate immediates | ~30 |
| `cellapp.cc` (witness path) | Drop the now-redundant `pending_witness_channels_` set — channel auto-registers itself with the interface | ~30 (deletion) |
| `cellapp.cc` (ghost broadcast loops) | No code change required — they automatically batch via the new default | 0 |
| `baseapp.cc` (relay paths) | No code change required | 0 |
| `tests/` | Audit any test that relies on "send → assert delivered synchronously" — must add a tick pump between the call and the assert | ~50–150 |

Total: ~250–400 LOC of code + targeted test sweep + descriptor
audit.

## Hot-path migration impact (no code changes required)

These call sites become tick-batched **automatically** once the
default flips:

| Call site | Today | After |
|---|---|---|
| `baseapp.cc:984` reliable delta to client | 1 sendto / message | 1 sendto / (client × tick) |
| `baseapp.cc:936/943` RPC relay to client | 1 sendto / RPC | Coalesced with deltas to same client |
| `baseapp.cc:1027` baseline to client | 1 sendto / snapshot | Coalesced with deltas |
| `cellapp.cc:1523/1538/1551` ghost broadcast | 1 sendto × N haunts / entity | 1 sendto / (haunt × tick), regardless of entity count |
| `cellapp.cc:1486/1495/1583` offload ops | 1 sendto / op | Coalesced with same-peer ghost broadcasts |

The "regardless of entity count" line is the structural win for
the cellapp ghost path — current cost scales `O(reals × haunts)`
syscalls per replication pump, post-change scales `O(haunts)`.

## Open decisions

1. **Per-message override vs descriptor-only.** Should there be
   a runtime override (e.g. `Channel::SendMessageImmediate(...)`)
   for callers that *usually* batch a particular message but
   occasionally need to flush right now (e.g. last message before
   a coroutine `co_await ResultArrived`)? Recommendation: yes,
   add the per-call override, keep the descriptor as the default.
2. **Threshold value.** 1400 B is conservative. Could go higher
   (~1450) if we trust no message body > 30 B follows in the
   same tick, but 80 B headroom is cheap insurance against
   surprise fragmentation. **Decision deferred until we have a
   bundle-size histogram from a fresh profile.**
3. **TCP path.** Adding a tick-aligned flush on top of the
   existing TCP write buffer is redundant — TCP already coalesces
   at the syscall layer and at the kernel layer. Recommendation:
   `Channel::SendMessage` on TCP channels behaves *exactly as
   today* (immediate `Send`), regardless of `urgency`. The
   `urgency` field is a hint that only RUDP honours.
4. **Synchronous test ergonomics.** Tests that send-then-assert
   either need a `dispatcher.Pump()` between, or the test harness
   needs to mark the test channel `kImmediate`-by-default. Pick
   one — recommend the latter for ergonomics.

## Risks

- **Latency floor on every batched message.** Up to one tick
  (≈ 67 ms at 15 Hz BaseApp / 100 ms at 10 Hz CellApp) of added
  delay between `SendMessage` returning and the bytes hitting the
  wire. For PvP-handfeel-critical paths (combat-action commands,
  hit-confirms), this is the reason `kImmediate` exists. The
  descriptor audit during implementation must enumerate which
  messages those are. The Atlas PvP scope is 1v1 / 2v2 / 4v4
  small-group, latency-critical; combat commands ride client →
  baseapp → cellapp, every hop must be `kImmediate` for the
  command path.
- **Bundle ordering across reliability axes.** Today, a caller
  that sends a reliable-then-unreliable pair sees them go out in
  order on the wire (two separate sendtos). Post-change, the
  reliable goes into one deferred bundle and the unreliable into
  another, flushed in a fixed order at tick end (`FlushDeferred`
  sends unreliable first, then reliable — see
  `reliable_udp.cc:84-92`). Order between the two axes inverts.
  Real-world impact: probably none, because reliable/unreliable
  pairs in this codebase are independent (volatile position vs
  semantic state); but worth confirming during the descriptor
  audit.
- **Coroutine RPC reply timing.** A reply that the receiver hands
  to `SendMessage` synchronously now lands at the next tick
  boundary instead of immediately. RPC chains > 2 hops compound
  the delay. Cross-reference with `network_dispatch_decoupling.md`
  — that doc analyses the same compounding effect on the receive
  side; the send side is a mirror image and the same numerical
  budget applies.
- **`packet_filter_` interaction.** The current send path runs
  the packet filter at `Channel::Send`-time on the finalised
  bundle. With batching, the filter runs once per flush over a
  bigger bundle. Per-message filters that assume one
  message-per-call (compression that targets a specific message
  shape, encryption with per-message nonces) need a re-audit.
  The current `compression_filter` operates on the whole
  `Send`-time payload, so it should be neutral. Encryption is
  not yet wired in — note this constraint for whoever adds it.
- **Visibility of failure.** Today a `SendMessage` failure (e.g.
  RUDP `kWouldBlock` because send window is full) propagates
  synchronously to the caller. With deferred batching, the failure
  surfaces at flush-time, and the caller has already returned.
  Recommendation: `BufferMessageDeferredInternal` only fails for
  conditions known synchronously (channel condemned, single
  message larger than `kMaxUdpPayload` would have to fragment
  immediately). cwnd-blocked failures get logged at flush time
  and dropped on the floor for unreliable; reliable retains its
  existing retransmit machinery.

## Validation plan when implementing

1. **Microbenchmark.** Direct measurement of the change is the
   `sendto` call rate on BaseApp under the existing 500-client
   stress run. Expect: BaseApp `Channel::Send` calls drop from
   6.30 M → ~30 k (one per (channel × tick) instead of per
   message). CPU drop in the 15–25 % range based on the current
   31.8 % `Channel::Send` cost being a mix of serialization +
   syscall, with the syscall portion eliminated.
2. **Latency degradation guard rails.** Re-run the 500-client
   baseline and compare:
   - `echo_rtt p50 / p95 / p99` — accept up to +1 tick worth of
     degradation (≈ +67 ms p50). If p50 climbs by more than
     100 ms, something is wrong.
   - `auth_latency p99` — should be largely unchanged, login
     RPCs are marked `kImmediate` per the descriptor audit.
3. **CellApp ghost path.** Single-cellapp deployments don't
   exercise this. Re-run a multi-cellapp stress (2 or 4 cellapps
   sharing a Space, entity migration enabled) and confirm:
   - Ghost position update lag stays under one tick worst-case.
   - Offload latency is unchanged (offload ops are `kImmediate`
     by descriptor audit).
4. **Reliability under loss.** Re-run script_client_smoke.md
   scenario 3 (transport-level drop window). Confirm:
   - Retransmit machinery still fires on ACK timeout.
   - Bundles that are dropped retransmit as a whole — no partial
     redelivery.
5. **Test sweep.** Run the full unit + integration suite. Expect
   to fix some number of "send-then-assert" patterns. Document
   each fix in the implementing PR.

## Migration path

Recommend in two PRs to keep review manageable:

**PR 1 — descriptor + plumbing only, no behavioural change.**
- Add `MessageUrgency` enum and field, default everything to
  `kImmediate` so existing behaviour is unchanged.
- Add `dirty_send_channels_` plumbing on `NetworkInterface`,
  unused in this PR.
- Add bundle-size threshold check + auto-flush in
  `BufferMessageDeferred`, keeps existing batching call sites
  identical but bounds bundle growth.
- All tests pass with no changes.

**PR 2 — flip the default + caller audit + test sweep.**
- Flip default `urgency = kBatched` for new messages.
- Audit hand-written `MessageDesc`s, mark immediate-required
  messages.
- Audit codegen output for entitydef messages.
- Run validation plan, fix tests.
- Drop the now-redundant cellapp `pending_witness_channels_`
  bookkeeping.

## Cookbook for the future implementer

- Start with PR 1 above. It is mechanical and reviewable in
  isolation.
- For the descriptor audit, the search command is:
  `Grep`-for `MessageReliability::kReliable` across `src/`,
  cross-reference with `Grep`-for `Descriptor()` callers — that
  yields every site that constructs a descriptor. Expect ~30
  hand-written ones plus the codegen template.
- The `kImmediate` whitelist should be short. Plausible
  candidates: `LoginRequest`, `LoginReply`, `ChannelClosed`,
  `OffloadEntity`, `EntityCreatedAck`, `Heartbeat`, anything
  with `Ack` in its name. Anything with `Update`, `Delta`,
  `Snapshot`, `Broadcast` in its name should stay `kBatched`.
- After PR 2 lands, the `pending_witness_channels_` set in
  cellapp becomes redundant infrastructure — remove it, the
  channel registers itself with the interface now. This is the
  cleanest evidence that the migration succeeded.

## Immediate whitelist — PR-2 audit ground truth

Snapshot taken after PR-1 (`7cfbb27`, MessageUrgency descriptor +
Channel-base plumbing) landed.  Every descriptor below currently
defaults to `kImmediate`; PR-2 keeps the table's left two columns
unchanged and flips only the third column to `kBatched`.

Total: **82 descriptors** (80 from `src/server/*/`*_messages.h` +
`src/lib/network/machined_types.h`, plus 2 from
`src/lib/server/common_messages.h`).  Inventory derived by
`grep -h 'kDesc{' src/server/*/*.h src/lib/server/*.h
src/lib/network/*.h | grep -oE 'msg_id::[A-Za-z]+::k[A-Za-z]+' | sort -u`.

### Stay `kImmediate` (~ 35 descriptors)

| Category | Descriptors | Why immediate |
|---|---|---|
| PvP command path | `BaseApp::kClientBaseRpc`, `BaseApp::kClientCellRpc`, `CellApp::kClientCellRpcForward`, `CellApp::kInternalCellRpc` | Combat / skill commands; +1 tick (~67 ms) breaks the 1v1 / 2v2 / 4v4 PvP feel target |
| Login & handshake | `Login::kLoginRequest`, `Login::kLoginResult`, `Login::kAuthLogin`, `Login::kPrepareLogin`, `Login::kPrepareLoginResult`, `Login::kAllocateBaseAppResult`, `Login::kCancelPrepareLogin`, `BaseApp::kAuthenticate`, `BaseApp::kAuthenticateResult`, `BaseApp::kAcceptClient` | Coroutine `co_await`; caller blocks on the syscall result |
| Disconnect / kick | `BaseApp::kForceLogoff`, `BaseApp::kForceLogoffAck`, `BaseApp::kCellAppDeath` | State-flip messages; user-visible behaviour gates on prompt delivery |
| Cellapp migration barrier | `CellApp::kOffloadEntity`, `CellApp::kOffloadEntityAck`, `BaseApp::kEntityTransferred`, `BaseApp::kBackupCellEntity`, `BaseApp::kCellEntityDestroyed`, `BaseApp::kCellReady` | Entity lifecycle ordering; reordering corrupts the migration handshake |
| All `*Ack` | `DBApp::kWriteEntityAck`, `kCheckoutEntityAck`, `kDeleteEntityAck`, `kLookupEntityAck`, `kAbortCheckoutAck`, `kGetEntityIdsAck`, `kPutEntityIdsAck`, `Machined::kHeartbeatAck`, `kRegisterAck`, `kListenerAck` | Reply messages; sender is in a `co_await` waiting for them |
| Machined control plane | `Machined::kRegister`, `kDeregister`, `kHeartbeat`, `kQuery`, `kQueryResponse`, `kBirthNotification`, `kDeathNotification`, `kListenerRegister`, `kWatcherRequest`, `kWatcherResponse`, `kWatcherForward`, `kWatcherReply` | Service-discovery + watcher RPC; low frequency, latency-sensitive |
| Heartbeat / shutdown | `Common::kHeartbeat`, `Common::kShutdownRequest` | Tiny, time-sensitive, low frequency |
| Manager registration | `BaseAppMgr::kRegisterBaseApp`, `kBaseAppReady`, `CellAppMgr::kRegisterCellApp` | Cluster-bringup barrier |
| Witness control | `CellApp::kEnableWitness`, `kDisableWitness`, `kSetAoIRadius` | Configuration flips, low frequency |
| Space topology | `CellApp::kCreateSpace`, `kDestroySpace`, `CellAppMgr::kAddCellToSpace`, `kUpdateGeometry`, `kShouldOffload` | Cluster-topology messages |
| GlobalBase registry | `BaseAppMgr::kRegisterGlobalBase`, `kDeregisterGlobalBase`, `kGlobalBaseNotification` | Naming-service updates |

### Flip to `kBatched` (~ 20 descriptors)

| Category | Descriptors | Expected payoff |
|---|---|---|
| **Replication (high-freq, primary target)** | `BaseApp::kReplicatedReliableDeltaFromCell`, `BaseApp::kReplicatedBaselineFromCell` | Direct hit on the documented 6.30 M / 31.8 % BaseApp `Channel::Send` hotspot |
| Ghost sync (high-freq) | `CellApp::kGhostDelta`, `CellApp::kGhostPositionUpdate`, `CellApp::kGhostSnapshotRefresh` | Collapses cellapp's `O(reals × haunts)` ghost broadcast to `O(haunts × tick)` once multi-cellapp is exercised |
| RPC forwarding (medium-freq) | `BaseApp::kCellRpcForward`, `BaseApp::kSelfRpcFromCell`, `BaseApp::kBroadcastRpcFromCell` | Cellapp → BaseApp → client chain; multiple to the same client coalesce per tick |
| Ghost lifecycle | `CellApp::kCreateGhost`, `CellApp::kDeleteGhost` | Medium-frequency; coalesces with the next ghost-update bundle to the same peer |
| Entity creation (bursty during login / cell migration) | `BaseApp::kCreateBase`, `BaseApp::kCreateBaseFromDb`, `CellApp::kCreateCellEntity`, `CellApp::kDestroyCellEntity` | Login wave / large-scale instance entry sees concurrent fan-out |
| Client event report | `BaseApp::kClientEventSeqReport` | Periodic client-side sequence acks; mergeable into the next outbound bundle |

### Defer evaluation (~ 13 descriptors, mostly DB)

| Descriptor | Reason to defer |
|---|---|
| `DBApp::kWriteEntity`, `kCheckoutEntity`, `kCheckinEntity`, `kLookupEntity`, `kGetEntityIds`, `kPutEntityIds` | DBApp is not in the documented hotspot path; batching DB writes raises durability concerns (a batched-but-not-yet-flushed write loses on crash); risk > reward.  Re-evaluate if a future capture shows DBApp egress as the bottleneck |

### Notes for the PR-2 implementer

- Whitelist encoded **as descriptor field**, not as a runtime
  override.  Per-call overrides (`SendMessageImmediate(...)`)
  are an open question — see `Open decisions` §1 above.
- Combat-action / hit-confirm messages enter the system as raw-id
  `SendMessage(MessageID, span)` from the script layer, so the
  PvP `kImmediate` set must include any descriptor that the C#
  combat pipeline writes through.  Re-grep
  `BaseApp::kClient*` after PR-2 audit to make sure nothing on
  the combat path was misclassified.
- Inter-cellapp ghost messages above are confirmed `kBatched`
  candidates only **for ghost broadcast**.  The same wire ID
  used during the offload handshake (e.g. a single `kGhostDelta`
  pinning state during transfer) must keep its position-of-call
  semantics — re-route those callers through the
  `SendMessageImmediate` override if §1 lands, otherwise carve a
  separate descriptor.
