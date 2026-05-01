#!/usr/bin/env python3
"""Compare two CellApp Tracy captures and report zone timing regressions.

Usage:
    python compare_tracy.py <baseline.tracy> <new.tracy> [options]

The script exports both traces to CSV via tracy-csvexport (stdout), then
prints a Markdown table of mean / max / total for the zones that matter
most to CellApp performance. Regressions (new > baseline by more than
--threshold %) are flagged with ▲; improvements with ▼.

Note: tracy-csvexport's aggregate output exposes mean/min/max/std/total but
not p95/p99 (those used to be in earlier Tracy releases). For tail-percentile
analysis open the trace in tracy-profiler directly.

Prerequisites:
    tracy-csvexport.exe must be in bin/profile/ or on PATH,
    or supplied via --csvexport.
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common.paths import REPO_ROOT  # noqa: F401

# Zones to include in the comparison table, in display order.
ZONES_OF_INTEREST = [
    "Tick",
    "CellApp::OnTickComplete",
    "CellApp::TickWitnesses",
    "Witness::Update",
    "Witness::Update::Transitions",
    "Witness::Update::PriorityHeap",
    "Witness::Update::Pump",
    "Witness::SendEntityUpdate",
    "Space::Tick",
    "Script.OnTick",
    "Script.EntityTickAll",
    "Script.PublishReplicationAll",
    "NetworkInterface::OnRudpReadable",
]


class ZoneStats(NamedTuple):
    count: int
    mean_us: float
    max_us: float
    total_us: float


def find_csvexport(hint: str | None) -> Path:
    if hint:
        p = Path(hint)
        if p.exists():
            return p
        sys.exit(f"tracy-csvexport not found at: {hint}")

    candidates = [
        REPO_ROOT / "bin" / "profile" / "tracy-csvexport.exe",
        REPO_ROOT / "bin" / "profile" / "tracy-csvexport",
        # Legacy nested layout (server/, tools/, …) — kept for transitional
        # builds before the flat bin/<build>/ refactor.
        REPO_ROOT / "bin" / "profile" / "tools" / "tracy-csvexport.exe",
        REPO_ROOT / "bin" / "profile" / "tools" / "tracy-csvexport",
        Path("tracy-csvexport.exe"),
        Path("tracy-csvexport"),
    ]
    for c in candidates:
        if c.exists():
            return c
    sys.exit(
        "tracy-csvexport not found. Pass --csvexport <path> or build profile."
    )


def export_to_csv(csvexport: Path, tracy_file: Path, out_csv: Path) -> None:
    # tracy-csvexport writes CSV to stdout; capture and persist it.
    result = subprocess.run(
        [str(csvexport), str(tracy_file)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        sys.exit(
            f"tracy-csvexport failed for {tracy_file.name}:\n"
            f"  stdout: {result.stdout.strip()}\n"
            f"  stderr: {result.stderr.strip()}"
        )
    out_csv.write_text(result.stdout, encoding="utf-8")


def ns_to_us(v: str) -> float:
    return float(v) / 1000.0


# Columns we depend on from tracy-csvexport's aggregate output. A schema
# change in a newer Tracy build silently dropping these would make the
# comparison table empty and the script return "no regressions" — fail
# loud on the first row instead of swallowing the mismatch.
REQUIRED_CSV_COLUMNS = ("name", "counts", "mean_ns", "max_ns", "total_ns")


def parse_csv(csv_path: Path) -> dict[str, ZoneStats]:
    """Parse tracy-csvexport aggregate output into a {zone_name: ZoneStats} map."""
    stats: dict[str, ZoneStats] = {}
    with open(csv_path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            sys.exit(f"{csv_path}: empty or unreadable CSV")
        missing = [c for c in REQUIRED_CSV_COLUMNS if c not in reader.fieldnames]
        if missing:
            sys.exit(
                f"{csv_path}: tracy-csvexport schema mismatch. "
                f"Missing columns: {missing}. Got: {list(reader.fieldnames)}"
            )
        for row in reader:
            name = row["name"].strip()
            if not name:
                continue
            stats[name] = ZoneStats(
                count=int(row["counts"]),
                mean_us=ns_to_us(row["mean_ns"]),
                max_us=ns_to_us(row["max_ns"]),
                total_us=ns_to_us(row["total_ns"]),
            )
    return stats


def pct_change(old: float, new: float) -> float:
    if old == 0:
        return float("inf") if new > 0 else 0.0
    return (new - old) / old * 100.0


def fmt_us(v: float) -> str:
    if v >= 1000:
        return f"{v/1000:.1f} ms"
    return f"{v:.1f} µs"


def fmt_delta(pct: float, threshold: float) -> str:
    if pct > threshold:
        return f"▲ +{pct:.0f}%"
    if pct < -threshold:
        return f"▼ {pct:.0f}%"
    return f"{pct:+.0f}%"


def print_comparison(
    baseline: dict[str, ZoneStats],
    new: dict[str, ZoneStats],
    *,
    baseline_label: str,
    new_label: str,
    threshold: float,
) -> bool:
    """Print a Markdown comparison table. Returns True if any regression found."""
    col_zone = 38
    col_count = 8
    col_val = 10
    col_delta = 10

    header = (
        f"| {'Zone':<{col_zone}} "
        f"| {'calls b':>{col_count}} "
        f"| {'calls n':>{col_count}} "
        f"| {'baseline mean':>{col_val}} "
        f"| {'new mean':>{col_val}} "
        f"| {'Δmean':>{col_delta}} "
        f"| {'baseline max':>{col_val}} "
        f"| {'new max':>{col_val}} "
        f"| {'Δmax':>{col_delta}} "
        f"| {'baseline total':>{col_val}} "
        f"| {'new total':>{col_val}} "
        f"| {'Δtotal':>{col_delta}} "
        f"|"
    )
    sep = re.sub(r"[^|]", "-", header)
    sep = re.sub(r"\|-+\|", lambda m: "|" + "-" * (len(m.group()) - 2) + "|", sep)

    print(f"\n## CellApp Tracy comparison")
    print(f"  baseline : {baseline_label}")
    print(f"  new      : {new_label}\n")
    print(header)
    print(sep)

    any_regression = False
    for zone in ZONES_OF_INTEREST:
        b = baseline.get(zone)
        n = new.get(zone)
        if b is None and n is None:
            continue

        def v(stats: ZoneStats | None, attr: str) -> str:
            return fmt_us(getattr(stats, attr)) if stats else "—"

        def cnt(stats: ZoneStats | None) -> str:
            return str(stats.count) if stats else "—"

        def d(attr: str) -> str:
            if b is None or n is None:
                return "n/a"
            pct = pct_change(getattr(b, attr), getattr(n, attr))
            if pct > threshold:
                nonlocal any_regression
                any_regression = True
            return fmt_delta(pct, threshold)

        print(
            f"| {zone:<{col_zone}} "
            f"| {cnt(b):>{col_count}} "
            f"| {cnt(n):>{col_count}} "
            f"| {v(b,'mean_us'):>{col_val}} "
            f"| {v(n,'mean_us'):>{col_val}} "
            f"| {d('mean_us'):>{col_delta}} "
            f"| {v(b,'max_us'):>{col_val}} "
            f"| {v(n,'max_us'):>{col_val}} "
            f"| {d('max_us'):>{col_delta}} "
            f"| {v(b,'total_us'):>{col_val}} "
            f"| {v(n,'total_us'):>{col_val}} "
            f"| {d('total_us'):>{col_delta}} "
            f"|"
        )

    print()
    if any_regression:
        print(f"⚠  Regressions detected (threshold {threshold:.0f}% on mean / max / total).")
    else:
        print("✓  No regressions above threshold.")
    return any_regression


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare two CellApp Tracy captures and report timing regressions."
    )
    parser.add_argument("baseline", help="Baseline .tracy file")
    parser.add_argument("new", help="New .tracy file to compare against baseline")
    parser.add_argument(
        "--csvexport",
        default=None,
        help="Path to tracy-csvexport binary (auto-detected if omitted)",
    )
    parser.add_argument(
        "--threshold",
        type=float,
        default=10.0,
        help="Regression threshold in %% (default: 10)",
    )
    parser.add_argument(
        "--keep-csv",
        action="store_true",
        help="Keep exported CSV files instead of deleting them",
    )
    args = parser.parse_args()

    baseline_path = Path(args.baseline)
    new_path = Path(args.new)
    for p in (baseline_path, new_path):
        if not p.exists():
            sys.exit(f"File not found: {p}")

    csvexport = find_csvexport(args.csvexport)

    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        baseline_csv = tmp_dir / "baseline.csv"
        new_csv = tmp_dir / "new.csv"

        print(f"Exporting {baseline_path.name} …", flush=True)
        export_to_csv(csvexport, baseline_path, baseline_csv)

        print(f"Exporting {new_path.name} …", flush=True)
        export_to_csv(csvexport, new_path, new_csv)

        if args.keep_csv:
            import shutil
            kept_dir = Path("tracy_csv_export")
            kept_dir.mkdir(exist_ok=True)
            shutil.copy(baseline_csv, kept_dir / f"{baseline_path.stem}.csv")
            shutil.copy(new_csv, kept_dir / f"{new_path.stem}.csv")
            print(f"CSVs saved to {kept_dir}/")

        baseline_stats = parse_csv(baseline_csv)
        new_stats = parse_csv(new_csv)

    has_regression = print_comparison(
        baseline_stats,
        new_stats,
        baseline_label=baseline_path.name,
        new_label=new_path.name,
        threshold=args.threshold,
    )

    return 1 if has_regression else 0


if __name__ == "__main__":
    sys.exit(main())
