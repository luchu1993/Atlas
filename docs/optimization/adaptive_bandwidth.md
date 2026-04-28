# Adaptive Bandwidth Budget

**Status:** ✅ Implemented (demand-based fair share with NIC cap).
**Subsystem:** `src/server/cellapp/cellapp.cc::TickWitnesses`,
`src/server/cellapp/witness.h::EstimateOutboundDemandBytes`
**Impact:** Eliminates starvation under high entity density; reclaims
unused budget from sparse observers; bounds fleet egress to NIC.

> The original proposal below is preserved for context.  The shipped
> design generalises it: per-observer demand is computed individually
> (Pass 1) AND the fleet sum is capped (Pass 2 scale-and-clamp).  See
> the **Implementation Notes** section at the end for what actually
> went in and how the defaults were tuned.

## Current Behavior

Each observer has a fixed per-tick bandwidth budget of 4096 bytes
(`witness_per_observer_budget_bytes`). When the AoI contains more entities than
the budget can serve, excess entities accumulate bandwidth deficit and wait for
subsequent ticks. The deficit carries forward linearly.

## Problem

In 100v100, each observer sees ~200 entities. At ~100 bytes per delta, a full
update costs ~20 KB — 5x the 4 KB budget. This means only ~40 entities get
updated per tick; the remaining 160 carry deficit. Under sustained load, some
entities never catch up and oscillate between delta and snapshot modes.

The budget is also wasteful in sparse scenarios — a player alone in the world
still "reserves" 4 KB of headroom that goes unused.

## Proposed Solution

Replace the fixed budget with an adaptive formula:

```cpp
uint32_t ComputeBudget(uint32_t peer_count) const {
  // Base: 1 KB minimum even with zero peers.
  // Per-peer: 64 bytes guaranteed per entity in AoI.
  // Cap: hard ceiling to prevent single observer from saturating the NIC.
  constexpr uint32_t kBase = 1024;
  constexpr uint32_t kPerPeer = 64;
  constexpr uint32_t kCap = 16384;
  return std::min(kBase + peer_count * kPerPeer, kCap);
}
```

| AoI Size | Fixed Budget | Adaptive Budget |
|----------|-------------|-----------------|
| 5 | 4096 | 1344 (saves bandwidth) |
| 50 | 4096 | 4224 (similar) |
| 100 | 4096 | 7424 (+81%) |
| 200 | 4096 | 13824 (+237%) |

### Deficit Damping

Current deficit accumulates unboundedly. Add exponential decay:

```cpp
bandwidth_deficit_ = static_cast<uint32_t>(bandwidth_deficit_ * 0.9f);
```

This prevents runaway deficit from stale periods and helps the system recover
after transient spikes.

## Key Files

- `src/server/cellapp/witness.h` — Replace fixed budget with `ComputeBudget()`
- `src/server/cellapp/witness.cc` — Call in `Update()`, add deficit damping
- `src/server/cellapp/cellapp_config.h` — Add base/per-peer/cap config

## Interaction with Distance LOD

When combined with distance LOD, adaptive bandwidth becomes even more
effective: fewer entities update per tick (due to LOD skipping), so the budget
serves more of the active entities without deficit.

## Risks

- Higher cap means more bytes per observer per tick. With 100 observers at
  16 KB each, peak server egress is ~1.6 MB/tick = ~48 MB/s at 30 Hz.
  Monitor NIC saturation.
- Adaptive budget makes bandwidth consumption less predictable for capacity
  planning. Add server-side metrics (bytes_sent_per_tick histogram).

---

## Implementation Notes

The shipped design keeps the spirit of the proposal but generalises
both axes — per-observer demand and fleet-wide cap — and keeps the hot
path O(1) per observer.

### Why the proposed `ComputeBudget(peer_count)` alone wasn't enough

The proposal sized each observer's budget proportional to its peer
count.  Two gaps surfaced once we ran the demand-based allocator
against the aggressive baseline (200 clients × 120 s, 5–30 s churn,
5 m walk, 5 % teleport):

1. **Enter bursts dominate the tick that they happen on**, not the
   steady state.  A 30-peer observer that just had 5 peers re-enter
   sees a 30×~50 B + 5×~200 B = 2.5 KB tick — the steady-state
   estimate would under-provision by ~40 %.
2. **The fleet aggregate must be bounded by the NIC**, not just
   per-observer.  Without a fleet cap, 200 dense observers all asking
   for 16 KB would produce 3.2 MB/tick = 48 MB/s at 15 Hz — fine on a
   1 GbE link but blows past on a 10 GbE-shared multi-tenant host.

### The two-pass shipped design

```cpp
// Pass 1 — collect O(1) demand per observer
demand = aoi_map_.size() * witness_per_peer_bytes
       + max(0, last_tick_bandwidth_deficit)

// Pass 2 — fleet-cap then per-observer clamp
scale = (sum_of_demands > total_outbound_cap)
        ? total_outbound_cap / sum_of_demands : 1.0
budget = clamp(demand * scale, min_per_observer, max_per_observer)
```

The deficit carryover term is the elegant bit: an observer that was
short last tick has its missed bytes added to this tick's request, so
the allocator naturally redirects budget to backlogged observers
without needing a feedback controller.  The trade-off is one tick
(~66 ms at 15 Hz) of allocation lag for unmodelled enter bursts.

### Empirical defaults (tuned against 200/120 s aggressive baseline)

| Knob | Default | Why |
|---|---|---|
| `witness_per_peer_bytes` | **150** | ~30 B position + ~50 B property delta + ~70 B amortised enter snapshot.  Measured from `aoi_*` counters across the baseline. |
| `witness_total_outbound_cap_bytes` | **1.6 MB/tick** (24 MB/s @ 15 Hz) | NIC-shaped; 1 GbE caps at 125 MB/s gross so we leave 80 % headroom for framing, retransmit, and other server traffic. |
| `witness_min_per_observer_budget_bytes` | 1024 | Floor so a sparse observer with `peers=0` still has room for the next Enter. |
| `witness_max_per_observer_budget_bytes` | 16384 | Ceiling so a single dense PvP observer can't monopolise the NIC. |

### Tuning cookbook

| Symptom | Knob to turn |
|---|---|
| `bandwidth deficit` warning at the *same* observers tick after tick | bump `witness_per_peer_bytes` (the steady estimate is too low for your scene) |
| `bandwidth deficit` warnings during enter bursts only | leave `witness_per_peer_bytes`; the deficit carryover handles bursts within 1 tick lag.  If lag is unacceptable, model enter bursts explicitly — currently rejected because keeping the demand estimate O(1) requires a separately-maintained counter and the carryover already absorbs bursts within one tick. |
| `Witness::Update::Pump` Tracy zone wide and blank | `witness_max_peers_per_tick` is the bottleneck, not bandwidth — peer queue isn't filling up because budget gets consumed before peer count.  Raise `witness_max_peers_per_tick`. |
| Cellapp egress saturates NIC | lower `witness_total_outbound_cap_bytes`; observers will gracefully degrade (everyone scaled proportionally rather than some starved). |
| Sparse-only deployment (BDO-style PvE) wastes bandwidth | values fine as-is; demand-based naturally drops to actual need. |

### Hot-path cost

| Phase | Per-tick cost @ N=200 |
|---|---|
| Pass 1 (demand collection) | ~10 µs (200 × O(1) reads) |
| Sum + scale division | <1 µs |
| Pass 2 (clamp + Update dispatch) | ~6 µs (excludes Update body) |
| **Allocator overhead** | **<20 µs (0.03 % of 66 ms tick)** |

Zero managed/native heap allocation after the first tick — the
`witness_demand_scratch_` vector reuses its capacity across ticks.

### Verified
Aggressive baseline (200 clients × 120 s, churn 5–30 s):

| | Equal-share (old) | Demand-share (current) |
|---|---|---|
| `bandwidth deficit` warnings | 24 | **0** |
| `leave fan-out missed` | 0 | 0 |
| `stale enter-pending` | 0 | 0 |
| `bytes_rx_per_sec` | 682 KB | 664 KB |
| audit gate exit | 0 | 0 |
| 9 cellapp/witness/aoi unit tests under ASan | pass | **pass** |
