#!/usr/bin/env python3
"""Validate CellAppMgr retire drain on a live Atlas cluster."""

from __future__ import annotations

import argparse
import platform
import re
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

CELLAPP_RE = re.compile(
    r"\bapp=(?P<app>\d+)\s+addr=(?P<addr>\S+)\s+load=(?P<load>\S+)\s+"
    r"entities=(?P<entities>\d+)\s+retiring=(?P<retiring>[01])"
)
STATUS_RE = re.compile(
    r"\bapp=(?P<app>\d+)\s+owned=(?P<owned>\d+)\s+drains=(?P<drains>\d+)\s+"
    r"pending=(?P<pending>\d+)\s+ready=(?P<ready>[01])(?:\s+stuck=(?P<stuck>\d+))?"
)
SPACES_RE = re.compile(r"\bspaces=(\d+)\b")
LEAF_APP_RE = re.compile(r"\bcell=\d+\s+app=(\d+)\b")


def default_atlas_tool(build_subdir: str) -> Path:
    exe = "atlas_tool.exe" if platform.system() == "Windows" else "atlas_tool"
    return REPO_ROOT / "bin" / build_subdir / exe


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument("--build", default="debug", help="bin/<build>/atlas_tool directory")
    parser.add_argument("--machined", default="127.0.0.1:20018", help="machined host:port")
    parser.add_argument("--atlas-tool", type=Path, help="explicit atlas_tool path")
    parser.add_argument("--target-app-id", type=int, default=0, help="CellApp app_id to retire")
    parser.add_argument("--min-cellapps", type=int, default=2, help="minimum registered CellApps")
    parser.add_argument("--min-spaces", type=int, default=2, help="minimum live Spaces")
    parser.add_argument("--timeout-sec", type=float, default=90.0, help="wait timeout")
    parser.add_argument("--poll-sec", type=float, default=1.0, help="watcher poll interval")
    parser.add_argument(
        "--drain-watchdog-ms",
        type=int,
        default=None,
        help="optional override for cellappmgr/lb/retire/drain_watchdog_ms",
    )
    return parser.parse_args()


def run_atlas_tool(exe: Path, machined: str, *cmd: str) -> str:
    full = [str(exe), "--machined", machined, *cmd]
    proc = subprocess.run(full, capture_output=True, text=True, timeout=10, cwd=REPO_ROOT)
    if proc.returncode != 0:
        detail = proc.stderr.strip() or proc.stdout.strip()
        raise RuntimeError(f"atlas_tool {' '.join(cmd)} failed: {detail}")
    return proc.stdout


def watcher_value(exe: Path, machined: str, path: str) -> str:
    out = run_atlas_tool(exe, machined, "watch", "cellappmgr", path)
    parts = out.strip().split(maxsplit=1)
    if len(parts) != 2:
        raise RuntimeError(f"unexpected watcher output for {path}: {out.strip()}")
    return parts[1]


def set_watcher(exe: Path, machined: str, path: str, value: str) -> str:
    out = run_atlas_tool(exe, machined, "set-watch", "cellappmgr", path, value)
    parts = out.strip().split(maxsplit=1)
    if len(parts) != 2:
        raise RuntimeError(f"unexpected set-watch output for {path}: {out.strip()}")
    return parts[1]


def parse_cellapps(summary: str) -> list[dict[str, str]]:
    return [match.groupdict() for match in CELLAPP_RE.finditer(summary)]


def parse_status(summary: str) -> dict[int, dict[str, int]]:
    out: dict[int, dict[str, int]] = {}
    for match in STATUS_RE.finditer(summary):
        values = match.groupdict()
        app_id = int(values["app"])
        out[app_id] = {
            "owned": int(values["owned"]),
            "drains": int(values["drains"]),
            "pending": int(values["pending"]),
            "ready": int(values["ready"]),
            "stuck": int(values["stuck"] or "0"),
        }
    return out


def parse_spaces(summary: str) -> int:
    match = SPACES_RE.search(summary)
    return int(match.group(1)) if match else 0


def parse_leaf_owners(summary: str) -> dict[int, int]:
    counts: dict[int, int] = {}
    for match in LEAF_APP_RE.finditer(summary):
        app_id = int(match.group(1))
        if app_id:
            counts[app_id] = counts.get(app_id, 0) + 1
    return counts


def choose_target(
    args: argparse.Namespace, exe: Path, apps: list[dict[str, str]], leaf_counts: dict[int, int]
) -> int:
    app_ids = {int(app["app"]) for app in apps}
    if args.target_app_id:
        if args.target_app_id not in app_ids:
            raise RuntimeError(f"--target-app-id {args.target_app_id} is not registered")
        if leaf_counts.get(args.target_app_id, 0) == 0:
            raise RuntimeError(f"--target-app-id {args.target_app_id} owns no BSP leaves")
        return args.target_app_id

    current = watcher_value(exe, args.machined, "cellappmgr/lb/retire/app_id")
    if current.isdigit() and int(current) in app_ids and leaf_counts.get(int(current), 0) > 0:
        return int(current)

    owned = [app_id for app_id in app_ids if leaf_counts.get(app_id, 0) > 0]
    if not owned:
        raise RuntimeError("no registered CellApp owns any BSP leaf")
    return max(owned, key=lambda app_id: (leaf_counts[app_id], app_id))


def main() -> int:
    args = parse_args()
    exe = args.atlas_tool or default_atlas_tool(args.build)
    if not exe.is_file():
        print(f"[verify_retire_drain] atlas_tool not found: {exe}", file=sys.stderr)
        print("[verify_retire_drain] build it with tools/bin/build first", file=sys.stderr)
        return 1

    try:
        cellapps_summary = watcher_value(exe, args.machined, "cellappmgr/lb/cellapps")
        apps = parse_cellapps(cellapps_summary)
        if len(apps) < args.min_cellapps:
            raise RuntimeError(
                f"need >= {args.min_cellapps} registered CellApps; got {len(apps)}"
            )

        spaces_summary = watcher_value(exe, args.machined, "cellappmgr/lb/spaces")
        space_count = parse_spaces(spaces_summary)
        if space_count < args.min_spaces:
            raise RuntimeError(f"need >= {args.min_spaces} live Spaces; got {space_count}")
        leaf_counts = parse_leaf_owners(spaces_summary)

        target = choose_target(args, exe, apps, leaf_counts)
        if args.drain_watchdog_ms is not None:
            set_watcher(
                exe,
                args.machined,
                "cellappmgr/lb/retire/drain_watchdog_ms",
                str(args.drain_watchdog_ms),
            )
        set_watcher(exe, args.machined, "cellappmgr/lb/retire/app_id", str(target))
        print(
            f"[verify_retire_drain] retiring app_id={target}; "
            f"spaces={space_count}; leaves={leaf_counts[target]}"
        )

        deadline = time.monotonic() + args.timeout_sec
        last_status = ""
        while time.monotonic() < deadline:
            status_text = watcher_value(exe, args.machined, "cellappmgr/lb/retire/status")
            stuck_count = watcher_value(exe, args.machined, "cellappmgr/lb/retire/stuck_count")
            last_status = f"{status_text}; stuck_count={stuck_count}"
            statuses = parse_status(status_text)
            target_status = statuses.get(target)
            if stuck_count != "0":
                raise RuntimeError(f"retire drain stuck: {last_status}")
            if target_status and all(
                target_status[key] == 0 for key in ("owned", "drains", "pending", "stuck")
            ) and target_status["ready"] == 1:
                print(f"[verify_retire_drain] PASS {last_status}")
                return 0
            time.sleep(args.poll_sec)

        raise RuntimeError(f"timeout waiting for app_id={target} ready; last={last_status}")
    except RuntimeError as ex:
        print(f"[verify_retire_drain] {ex}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
