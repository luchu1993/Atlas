# Envelope Cache (Shared Delta Serialisation)

**Status:** ✅ Shipped (`0c1e755`).
**Subsystem:** `src/server/cellapp/cell_entity.h` (cache members on
`ReplicationFrame`), `src/server/cellapp/witness.cc::SendEntityUpdate`
(cache lookup + `Witness::Event::Build` / `Witness::Event::Send` zones).

## Design

A `ReplicationFrame` owns two opportunistic byte caches —
`cached_owner_envelope` and `cached_other_envelope`. The first witness
that needs to send a delta for that frame builds the wire envelope and
stores it; every subsequent witness for the same frame (same tick,
same peer, late observers replaying older frames) memcpy's the bytes
verbatim.

Keying the cache on `ReplicationFrame` (which already lives in the
entity's history window) instead of on the entity per-tick covers both
same-tick fan-out and history-window replay with a single mechanism.

## Scope

- Covers `SendEntityUpdate` deltas only.
- Does **not** cover `SendEntityEnter` baseline snapshots — those remain
  per-observer. Baseline sharing is a separate problem; see
  [lazy_baseline.md](lazy_baseline.md) Tactic 3.
- Owner deltas (per-observer view) are *not* shareable; the cache holds
  only the "other" envelope on shared paths.

## Caveats

- One extra `std::vector<std::byte>` per active `ReplicationFrame` per
  audience. The frames live in a bounded history window so retained
  capacity is bounded by `kReplicationHistoryWindow × peers × payload`.
- Per-observer filtering (team visibility, fog-of-war) would invalidate
  the shared cache and need either parameterisation or one cache slot
  per visibility class.
