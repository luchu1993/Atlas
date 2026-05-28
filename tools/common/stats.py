from __future__ import annotations

import math
from collections.abc import Sequence


def percentile_nearest_rank(values: Sequence[int], percentile: int) -> int:
    if not values:
        return 0
    sorted_values = sorted(values)
    rank = math.ceil((percentile / 100.0) * len(sorted_values))
    index = min(max(rank - 1, 0), len(sorted_values) - 1)
    return sorted_values[index]


def latency_summary_ms(samples: Sequence[int], prefix: str) -> dict[str, int | float]:
    sample_key = f"{prefix}_latency_samples"
    if not samples:
        return {
            sample_key: 0,
            f"min_{prefix}_ms": 0,
            f"avg_{prefix}_ms": 0.0,
            f"p50_{prefix}_ms": 0,
            f"p95_{prefix}_ms": 0,
            f"max_{prefix}_ms": 0,
        }
    return {
        sample_key: len(samples),
        f"min_{prefix}_ms": min(samples),
        f"avg_{prefix}_ms": round(sum(samples) / len(samples), 2),
        f"p50_{prefix}_ms": percentile_nearest_rank(samples, 50),
        f"p95_{prefix}_ms": percentile_nearest_rank(samples, 95),
        f"max_{prefix}_ms": max(samples),
    }
