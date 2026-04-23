# Adaptive Bandwidth Budget

**Priority:** P0
**Subsystem:** `src/server/cellapp/witness.cc`
**Impact:** Eliminates starvation under high entity density

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
