# Incremental Witness Ranking

**Status:** 🔵 Deferred — `Witness::Update::PerBandPump` is not on any
visible critical path in the last recorded captures. Re-profile before
choosing a data structure here.
**Subsystem:** `src/server/cellapp/witness.cc`

## What this would be

`Witness::Update()` currently rebuilds per-band ranking scratch each
tick: clear `band_scratch_`, recompute squared distance priority for
eligible AoI peers, `nth_element` to the band's cap, then sort the
selected peers by distance.

Most peers move slowly relative to tick rate, so the rank order
barely changes between ticks. A future incremental rank index could
store each peer's current band and approximate rank on `EntityCache`,
then update only peers whose squared distance moved past a threshold.

## Trigger to revisit

Open this doc when a fresh capture shows
`Witness::Update::PerBandPump` exceeding ~5 % of cellapp CPU, or
when the per-observer peer count crosses 500 on the dense PvP path.
Until then, the rebuild cost is below other zones worth attacking.
