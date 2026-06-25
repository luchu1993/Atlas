# Envelope Cache (Shared Delta Serialisation)

**Status:** ✅ Shipped.
**Subsystem:** `src/server/cellapp/cell_entity.h` (cache members on
`ReplicationFrame`), `src/server/cellapp/witness.cc::SendEntityUpdate`
(cache lookup + `Witness::Event::Build` / `Witness::Event::Send` zones).

## Design

A `ReplicationFrame` owns one opportunistic byte cache:
`cached_other_envelope`. The first witness that needs to send a
shareable "other" delta for that frame builds the wire envelope and
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

- One extra `std::vector<std::byte>` per active `ReplicationFrame`.
  The frames live in a bounded history window so retained capacity is
  bounded by `kReplicationHistoryWindow × peers × payload`.
- Per-observer filtering (team visibility, fog-of-war) would invalidate
  the shared cache and need either parameterisation or one cache slot
  per visibility class.
