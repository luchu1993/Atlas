#!/usr/bin/env python3
"""Tabulate live load/entity_count/space_count for every cellapp.

Wraps atlas_tool: enumerates registered cellapps via `list cellapp`, then
queries the cellapp/* watchers for each one. Use to verify multi-cellapp
LB at runtime — entity_count should diverge once entities cross BSP cell
boundaries and trigger Offload.
"""
from __future__ import annotations

import argparse
import platform
import re
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Display name → cellapp watcher path. Order drives column order.
WATCHERS: list[tuple[str, str]] = [
    ("load",         "cellapp/load"),
    ("real_ents",    "cellapp/real_entity_count"),
    ("total_ents",   "cellapp/total_entity_count"),
    ("spaces",       "cellapp/space_count"),
]


def default_atlas_tool(build_subdir: str) -> Path:
    exe = "atlas_tool.exe" if platform.system() == "Windows" else "atlas_tool"
    return REPO_ROOT / "bin" / build_subdir / exe


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--build", default="debug",
                   help="bin/<build>/atlas_tool directory (default: debug)")
    p.add_argument("--machined", default="127.0.0.1:20018",
                   help="machined host:port (default: 127.0.0.1:20018)")
    p.add_argument("--atlas-tool", type=Path,
                   help="explicit atlas_tool path (overrides --build)")
    p.add_argument("--watch", type=float, default=0.0,
                   help="poll forever, re-printing every N seconds (0 = single shot)")
    return p.parse_args()


def run_atlas_tool(exe: Path, machined: str, *cmd: str) -> str:
    full = [str(exe), "--machined", machined, *cmd]
    proc = subprocess.run(full, capture_output=True, text=True, timeout=10)
    if proc.returncode != 0:
        raise RuntimeError(
            f"atlas_tool {' '.join(cmd)} failed (rc={proc.returncode}): "
            f"{proc.stderr.strip() or proc.stdout.strip()}")
    return proc.stdout


# atlas_tool list output: "TYPE NAME INTERNAL_ADDR PID LOAD" then rows.
LIST_ROW = re.compile(r"^cellapp\s+(\S+)\s+(\S+)\s+(\d+)\s+([\d.]+)%")


def list_cellapps(exe: Path, machined: str) -> list[tuple[str, str, int]]:
    out = run_atlas_tool(exe, machined, "list", "cellapp")
    rows: list[tuple[str, str, int]] = []
    for line in out.splitlines():
        m = LIST_ROW.match(line.strip())
        if m:
            rows.append((m.group(1), m.group(2), int(m.group(3))))
    return rows


# atlas_tool watch output: "<source_name>  <value>" — single row.
def query_watcher(exe: Path, machined: str, name: str, path: str) -> str:
    out = run_atlas_tool(exe, machined, "watch", f"cellapp:{name}", path)
    parts = out.strip().split(maxsplit=1)
    return parts[1] if len(parts) == 2 else "?"


def snapshot(exe: Path, machined: str) -> list[dict[str, str]]:
    cellapps = list_cellapps(exe, machined)
    rows: list[dict[str, str]] = []
    for name, addr, pid in cellapps:
        row = {"name": name, "addr": addr, "pid": str(pid)}
        for label, path in WATCHERS:
            row[label] = query_watcher(exe, machined, name, path)
        rows.append(row)
    return rows


def print_table(rows: list[dict[str, str]]) -> None:
    if not rows:
        print("(no cellapps registered)")
        return
    headers = ["name", "addr", "pid"] + [w[0] for w in WATCHERS]
    widths = {h: max(len(h), max(len(r[h]) for r in rows)) for h in headers}
    print("  ".join(f"{h:<{widths[h]}}" for h in headers))
    print("  ".join("-" * widths[h] for h in headers))
    for r in rows:
        print("  ".join(f"{r[h]:<{widths[h]}}" for h in headers))


def main() -> int:
    args = parse_args()
    exe = args.atlas_tool or default_atlas_tool(args.build)
    if not exe.is_file():
        print(f"[dump_cellapp_load] atlas_tool not found: {exe}", file=sys.stderr)
        print(f"[dump_cellapp_load] build it via tools/bin/build.{{bat,sh}} first",
              file=sys.stderr)
        return 1

    try:
        if args.watch > 0:
            while True:
                rows = snapshot(exe, args.machined)
                print(f"\n--- {time.strftime('%H:%M:%S')} ---")
                print_table(rows)
                time.sleep(args.watch)
        else:
            print_table(snapshot(exe, args.machined))
    except KeyboardInterrupt:
        return 130
    except RuntimeError as ex:
        print(f"[dump_cellapp_load] {ex}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
