#!/usr/bin/env python3
"""Verify Phase D (BSP Unsplit on cellapp death) on a live cluster.

Pre-req: a multi-cellapp cluster is already running, e.g.

    tools/bin/run_mvp_cluster.{bat,sh} --cellapp-count 4

The script enumerates registered cellapps via atlas_tool, picks one (default:
the highest-named, e.g. cellapp_03), kills its PID, then tails the cellappmgr
stdout.log for the unsplit/rehoming event:

    PASS     — `CellAppMgr: unsplit cell_id=X (space Y)` observed
    PARTIAL  — fallback `CellAppMgr: rehoming cell_id=X` (single-leaf path)
    FAIL     — neither observed within --timeout-sec
"""
from __future__ import annotations

import argparse
import os
import platform
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

LIST_ROW = re.compile(r"^cellapp\s+(\S+)\s+(\S+)\s+(\d+)\s+([\d.]+)%")
UNSPLIT_PATTERN = re.compile(r"CellAppMgr: unsplit cell_id=(\d+)\s+\(space (\d+)\)")
REHOMING_PATTERN = re.compile(r"CellAppMgr: rehoming cell_id=(\d+)\s+\(space (\d+)\)")


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
    p.add_argument("--cellappmgr-log", type=Path,
                   help="explicit cellappmgr stdout.log; default: most recent under "
                        ".tmp/world-stress/<ts>/logs/cellappmgr.stdout.log")
    p.add_argument("--target",
                   help="cellapp name to kill (default: lexicographically last, "
                        "e.g. cellapp_03 in a 4-cellapp cluster)")
    p.add_argument("--timeout-sec", type=float, default=15.0,
                   help="seconds to wait for unsplit/rehoming log line (default: 15)")
    return p.parse_args()


def run_atlas_tool(exe: Path, machined: str, *cmd: str) -> str:
    full = [str(exe), "--machined", machined, *cmd]
    proc = subprocess.run(full, capture_output=True, text=True, timeout=10)
    if proc.returncode != 0:
        raise RuntimeError(
            f"atlas_tool {' '.join(cmd)} failed (rc={proc.returncode}): "
            f"{proc.stderr.strip() or proc.stdout.strip()}")
    return proc.stdout


def list_cellapps(exe: Path, machined: str) -> list[tuple[str, str, int]]:
    out = run_atlas_tool(exe, machined, "list", "cellapp")
    rows: list[tuple[str, str, int]] = []
    for line in out.splitlines():
        m = LIST_ROW.match(line.strip())
        if m:
            rows.append((m.group(1), m.group(2), int(m.group(3))))
    return rows


def find_latest_cellappmgr_log() -> Path | None:
    run_root = REPO_ROOT / ".tmp" / "world-stress"
    if not run_root.is_dir():
        return None
    runs = sorted((p for p in run_root.iterdir() if p.is_dir()),
                  key=lambda p: p.stat().st_mtime, reverse=True)
    for run in runs:
        log = run / "logs" / "cellappmgr.stdout.log"
        if log.is_file():
            return log
    return None


def kill_pid(pid: int) -> bool:
    if os.name == "nt":
        result = subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                                capture_output=True, text=True)
        return result.returncode == 0
    try:
        os.kill(pid, signal.SIGKILL)
        return True
    except ProcessLookupError:
        return False


def watch_log(log_path: Path, start_offset: int,
              deadline: float) -> tuple[str, str] | None:
    """Tail log_path from start_offset until an unsplit/rehoming line appears or
    the deadline passes. Returns (kind, line) on match, None on timeout."""
    offset = start_offset
    while time.monotonic() < deadline:
        try:
            with log_path.open("r", encoding="utf-8", errors="replace") as f:
                f.seek(offset)
                while True:
                    line = f.readline()
                    if not line:
                        break
                    if UNSPLIT_PATTERN.search(line):
                        return ("unsplit", line.rstrip())
                    if REHOMING_PATTERN.search(line):
                        return ("rehoming", line.rstrip())
                offset = f.tell()
        except FileNotFoundError:
            pass
        time.sleep(0.25)
    return None


def main() -> int:
    args = parse_args()
    exe = args.atlas_tool or default_atlas_tool(args.build)
    if not exe.is_file():
        print(f"[verify_phase_d] atlas_tool not found: {exe}", file=sys.stderr)
        print(f"[verify_phase_d] build it via tools/bin/build.{{bat,sh}} first",
              file=sys.stderr)
        return 1

    log_path = args.cellappmgr_log or find_latest_cellappmgr_log()
    if log_path is None or not log_path.is_file():
        print(f"[verify_phase_d] cellappmgr stdout log not found "
              f"(searched .tmp/world-stress/<ts>/logs/cellappmgr.stdout.log); "
              f"is the cluster running?", file=sys.stderr)
        print(f"[verify_phase_d] start one via "
              f"tools/bin/run_mvp_cluster.{{bat,sh}} --cellapp-count 4",
              file=sys.stderr)
        return 1

    try:
        cellapps = list_cellapps(exe, args.machined)
    except RuntimeError as ex:
        print(f"[verify_phase_d] {ex}", file=sys.stderr)
        return 1

    if not cellapps:
        print(f"[verify_phase_d] no cellapps registered with machined at "
              f"{args.machined}; is the cluster running?", file=sys.stderr)
        print(f"[verify_phase_d] start one via "
              f"tools/bin/run_mvp_cluster.{{bat,sh}} --cellapp-count 4",
              file=sys.stderr)
        return 1
    if len(cellapps) < 2:
        print(f"[verify_phase_d] need >=2 cellapps to exercise BSP Unsplit; "
              f"only {len(cellapps)} registered "
              f"(single-leaf trees fall back to rehoming)", file=sys.stderr)
        return 1

    if args.target:
        match = next((r for r in cellapps if r[0] == args.target), None)
        if match is None:
            print(f"[verify_phase_d] --target {args.target!r} not registered; "
                  f"available: {[r[0] for r in cellapps]}", file=sys.stderr)
            return 1
        target = match
    else:
        target = sorted(cellapps, key=lambda r: r[0])[-1]

    print(f"[verify_phase_d] {len(cellapps)} cellapps registered: "
          f"{', '.join(r[0] for r in cellapps)}")
    print(f"[verify_phase_d] cellappmgr log: {log_path}")
    print(f"[verify_phase_d] target: {target[0]} addr={target[1]} pid={target[2]}")

    start_offset = log_path.stat().st_size
    print(f"[verify_phase_d] killing pid {target[2]}...")
    if not kill_pid(target[2]):
        print(f"[verify_phase_d] kill failed (pid {target[2]} no longer exists?)",
              file=sys.stderr)
        return 1

    print(f"[verify_phase_d] watching for unsplit/rehoming line "
          f"(timeout {args.timeout_sec}s)...")
    deadline = time.monotonic() + args.timeout_sec
    result = watch_log(log_path, start_offset, deadline)

    if result is None:
        print(f"[verify_phase_d] FAIL — no unsplit/rehoming event in "
              f"{args.timeout_sec}s; inspect {log_path}", file=sys.stderr)
        return 2

    kind, body = result
    if kind == "unsplit":
        print(f"[verify_phase_d] PASS — BSP Unsplit observed")
        print(f"  {body}")
        return 0

    print(f"[verify_phase_d] PARTIAL — fallback rehoming observed "
          f"(Unsplit code path NOT exercised; the target's subtree must have been a "
          f"single-leaf root)")
    print(f"  {body}")
    return 3


if __name__ == "__main__":
    sys.exit(main())
