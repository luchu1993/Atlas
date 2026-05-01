# Adaptive Bandwidth Budget

**Status:** ✅ Shipped (demand-based fair share with NIC cap).
**Subsystem:** `src/server/cellapp/cellapp.cc::TickWitnesses`,
`src/server/cellapp/witness.h::EstimateOutboundDemandBytes`.

## Design

Each tick `TickWitnesses` runs a two-pass allocator: per-observer
demand collection, then fleet-cap scale-and-clamp. Both passes are
O(1) per observer; the hot path allocates nothing after the first
tick.

```text
Pass 1 — collect O(1) demand per observer
  demand = aoi_map_.size() * witness_per_peer_bytes
         + max(0, last_tick_bandwidth_deficit)

Pass 2 — fleet-cap then per-observer clamp
  scale = (Σdemand > total_outbound_cap)
          ? total_outbound_cap / Σdemand : 1.0
  budget = clamp(demand * scale, min_per_observer, max_per_observer)
```

Deficit carryover is the load-balancer: an observer short last tick
has its missed bytes added to this tick's request, so backlogged
observers naturally win budget without a feedback controller. The
trade-off is one tick of allocation lag for unmodelled enter bursts.

## Knobs (defaults tuned against 500-cli baseline)

| Knob | Default | Why |
|---|---|---|
| `witness_per_peer_bytes` | 200 | ~30 B position + ~50 B property delta + ~120 B amortised enter snapshot under churn |
| `witness_total_outbound_cap_bytes` | 4 MB/tick (≈ 60 MB/s @ 15 Hz) | NIC-shaped; 1 GbE caps at 125 MB/s gross with 50 % headroom for framing / retransmit / other server traffic |
| `witness_min_per_observer_budget_bytes` | 1024 | Floor so a sparse observer still has room for the next Enter |
| `witness_max_per_observer_budget_bytes` | 16384 | Ceiling so a single dense PvP observer can't monopolise the NIC |

## Tuning cookbook

| Symptom | Knob to turn |
|---|---|
| `bandwidth deficit` warnings on the *same* observers tick after tick | bump `witness_per_peer_bytes` — steady estimate undersized for the scene |
| `bandwidth deficit` only during enter bursts | leave it; carryover absorbs bursts within one-tick lag |
| `Witness::Update::Pump` zone wide and blank | `witness_max_peers_per_tick` is the bottleneck, not bandwidth — peer queue can't fill |
| Cellapp egress saturates NIC | lower `witness_total_outbound_cap_bytes`; observers degrade proportionally instead of a few starving |
| Sparse PvE deployment wastes bandwidth | values fine as-is; demand-based naturally drops to actual need |

## Caveats

- Single-tick allocation lag for enter bursts is the deliberate
  trade against an O(N) demand estimator.
- Per-tick scratch (`witness_demand_scratch_`) reuses capacity; do
  not reset it between ticks.
