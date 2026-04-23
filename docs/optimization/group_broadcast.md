# Group Broadcast (Multicast Delta)

**Priority:** P2
**Subsystem:** `src/server/cellapp/witness.cc`, network layer
**Impact:** Reduces serialization CPU by up to N-fold for shared-view entities

## Current Behavior

When entity A moves, the Witness for each of the N observers independently
serializes A's delta and sends it over each observer's channel. For 100
observers watching the same entity, the same delta is serialized 100 times.

## Problem

Serialization is CPU-bound. In 100v100, each entity's position update is
serialized 100+ times per tick. The serialization cost for `other_delta` is
identical across all non-owner observers.

## Proposed Solution

Serialize `other_delta` once per entity per tick, then multicast the pre-
serialized buffer to all observers in the AoI.

### Design

```cpp
// In CellEntity / RealEntityData:
struct PreSerializedDelta {
  uint64_t event_seq;
  std::vector<std::byte> other_delta_bytes;  // serialized once
};

// In Witness::Update():
// Instead of serializing per observer:
//   for each observer: serialize(entity.delta) → send
// Do:
//   auto& prebuilt = entity.PreSerializedOtherDelta();
//   for each observer: send(prebuilt.bytes)  // zero-copy
```

### Savings

| | Before | After |
|---|--------|-------|
| Serializations per entity per tick | N observers | 1 |
| CPU for 200 entities × 100 observers | 20,000 | 200 |

### Limitations

- Owner delta remains per-observer (different view).
- If per-observer filtering is added (e.g. team visibility), the prebuilt
  delta must be parameterized or have variants per visibility group.

## Key Files

- `src/server/cellapp/real_entity_data.h` — Pre-serialized delta cache
- `src/server/cellapp/witness.cc` — Use pre-serialized buffer in send loop

## Risks

- Memory: one extra buffer per entity per tick. ~100 bytes × 200 entities = 20 KB.
- Stale cache: must invalidate when delta changes mid-tick (unlikely in
  single-threaded tick model).
