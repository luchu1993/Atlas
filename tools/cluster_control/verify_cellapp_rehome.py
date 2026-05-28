#!/usr/bin/env python3
"""Validate CellApp death rehome on a live Atlas cluster."""

from __future__ import annotations

import argparse
import errno
import os
import platform
import re
import subprocess
import sys
import time
from argparse import Namespace
from pathlib import Path
from typing import NamedTuple

TOOLS_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS_ROOT))
from common.json_io import write_json_atomic  # noqa: E402
from common.stats import latency_summary_ms, percentile_nearest_rank  # noqa: E402

REPO_ROOT = TOOLS_ROOT.parent
PROC_RE = re.compile(
    r"^(?P<type>\S+)\s+(?P<name>\S+)\s+(?P<addr>\S+)\s+(?P<pid>\d+)\s+(?P<load>\S+)%$"
)
CELLAPP_RE = re.compile(
    r"\bapp=(?P<app>\d+)\s+addr=(?P<addr>\S+)\s+load=(?P<load>\S+)\s+"
    r"entities=(?P<entities>\d+)\s+retiring=(?P<retiring>[01])"
)
LEAF_APP_RE = re.compile(r"\bcell=\d+\s+app=(\d+)\b")
BASEAPP_ROUTE_RE = re.compile(
    r"\baddr=(?P<addr>\S+)\s+entities=(?P<entities>\d+)"
    r"\s+payload_candidates=(?P<payload_candidates>\d+)"
    r"\s+ghost_backup_candidates=(?P<ghost_backup_candidates>\d+)\b"
)
BASEAPP_RESTORE_COUNTER_WATCHERS = {
    "notifications": "baseapp/cellapp_death_notifications_total",
    "scheduled": "baseapp/cellapp_death_restore_scheduled_total",
    "payload_scheduled": "baseapp/cellapp_death_restore_payload_scheduled_total",
    "ghost_backup_scheduled": "baseapp/cellapp_death_restore_ghost_backup_scheduled_total",
    "restored": "baseapp/cellapp_death_restored_total",
    "lost": "baseapp/cellapp_death_lost_total",
    "timeouts": "baseapp/cellapp_death_restore_timeouts_total",
}
BASEAPP_RESTORE_GAUGE_WATCHERS = {
    "pending": "baseapp/cellapp_death_pending_restores",
    "last_elapsed_ms": "baseapp/cellapp_death_restore_last_elapsed_ms",
    "max_elapsed_ms": "baseapp/cellapp_death_restore_max_elapsed_ms",
}
BASEAPP_RESTORE_WATCHERS = BASEAPP_RESTORE_COUNTER_WATCHERS | BASEAPP_RESTORE_GAUGE_WATCHERS
CELLAPP_RESTORE_WATCHERS = {
    "total": "cellapp/create_cell_entity_death_restore_total",
    "payload": "cellapp/create_cell_entity_death_restore_payload_total",
    "ghost_backup": "cellapp/create_cell_entity_death_restore_ghost_backup_total",
    "empty": "cellapp/create_cell_entity_death_restore_empty_total",
    "failures": "cellapp/create_cell_entity_death_restore_failures_total",
    "promoted": "cellapp/create_cell_entity_death_restore_promoted_total",
}
SUMMARY_VOLUME_GATES = {
    "min_total_scheduled_restores": "scheduled",
    "min_total_payload_restores": "cell_payload",
    "min_total_ghost_backup_restores": "cell_ghost_backup",
    "min_total_promoted_restores": "cell_promoted",
    "min_restore_latency_samples": "restore_latency_samples",
}
SUMMARY_SHARE_GATES = {
    "min_payload_restore_share": "payload_restore_share",
    "min_ghost_backup_restore_share": "ghost_backup_restore_share",
    "min_promoted_restore_share": "promoted_restore_share",
}
SUMMARY_GATES = {**SUMMARY_VOLUME_GATES, **SUMMARY_SHARE_GATES}


class RehomeCycleResult(NamedTuple):
    detail: str
    scheduled: int
    payload_scheduled: int
    ghost_backup_scheduled: int
    restored: int
    lost: int
    timeouts: int
    cell_total: int
    cell_payload: int
    cell_ghost_backup: int
    cell_empty: int
    cell_failures: int
    cell_promoted: int
    restore_elapsed_ms: int
    cycle: int = 0
    target_name: str = ""
    target_app_id: int = 0
    target_pid: str = ""
    target_addr: str = ""
    target_leaf_count: int = 0
    target_entities: int = 0
    target_base_routes: int = 0
    target_payload_candidates: int = 0
    target_ghost_backup_candidates: int = 0


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
    parser.add_argument("--target-app-id", type=int, default=0, help="CellApp app_id to kill")
    parser.add_argument("--target-name", default="", help="CellApp process name to kill")
    parser.add_argument("--min-cellapps", type=int, default=2, help="minimum registered CellApps")
    parser.add_argument("--min-spaces", type=int, default=1, help="minimum live Spaces")
    parser.add_argument("--timeout-sec", type=float, default=60.0, help="wait timeout")
    parser.add_argument("--poll-sec", type=float, default=1.0, help="watcher poll interval")
    parser.add_argument("--cycles", type=int, default=1, help="number of crash/rehome cycles")
    parser.add_argument(
        "--max-restore-ms",
        type=int,
        default=0,
        help="maximum allowed restore resolution time; 0 disables the check",
    )
    parser.add_argument(
        "--min-target-entities",
        type=int,
        default=0,
        help="minimum entities hosted by the killed CellApp; 0 disables the check",
    )
    parser.add_argument(
        "--min-target-leaves",
        type=int,
        default=0,
        help="minimum BSP leaves owned by the killed CellApp; 0 disables the check",
    )
    parser.add_argument(
        "--min-restores",
        type=int,
        default=0,
        help=(
            "minimum BaseApp scheduled restore delta required per cycle; "
            "target selection uses BaseApp cell routes; 0 disables the check"
        ),
    )
    parser.add_argument(
        "--min-ghost-backup-restores",
        type=int,
        default=0,
        help="minimum death-restore Ghost backup source delta required per cycle",
    )
    parser.add_argument(
        "--min-payload-restores",
        type=int,
        default=0,
        help="minimum death-restore payload source delta required per cycle",
    )
    parser.add_argument(
        "--min-promoted-restores",
        type=int,
        default=0,
        help="minimum death-restore Ghost-to-Real promotion delta required per cycle",
    )
    parser.add_argument(
        "--min-total-scheduled-restores",
        type=int,
        default=0,
        help="minimum aggregate BaseApp scheduled restore delta across all cycles",
    )
    parser.add_argument(
        "--min-total-payload-restores",
        type=int,
        default=0,
        help="minimum aggregate payload restore delta across all cycles",
    )
    parser.add_argument(
        "--min-total-ghost-backup-restores",
        type=int,
        default=0,
        help="minimum aggregate Ghost backup restore delta across all cycles",
    )
    parser.add_argument(
        "--min-total-promoted-restores",
        type=int,
        default=0,
        help="minimum aggregate Ghost-to-Real promotion delta across all cycles",
    )
    parser.add_argument(
        "--min-restore-latency-samples",
        type=int,
        default=0,
        help="minimum cycles with scheduled restores included in restore latency summary",
    )
    parser.add_argument(
        "--min-payload-restore-share",
        type=float,
        default=0.0,
        help="minimum aggregate payload restore share across all cycles",
    )
    parser.add_argument(
        "--min-ghost-backup-restore-share",
        type=float,
        default=0.0,
        help="minimum aggregate Ghost backup restore share across all cycles",
    )
    parser.add_argument(
        "--min-promoted-restore-share",
        type=float,
        default=0.0,
        help="minimum aggregate Ghost-to-Real promotion share across all cycles",
    )
    parser.add_argument("--shutdown-reason", type=int, default=1, help="machined shutdown reason")
    parser.add_argument(
        "--summary-json",
        type=Path,
        help="write a machine-readable summary JSON after all cycles complete",
    )
    parser.add_argument(
        "--allow-no-baseapp",
        action="store_true",
        help="skip BaseApp death-restore watcher verification",
    )
    args = parser.parse_args()
    if args.target_app_id and args.target_name:
        parser.error("--target-app-id cannot be combined with --target-name")
    if args.cycles < 1:
        parser.error("--cycles must be >= 1")
    if args.max_restore_ms < 0:
        parser.error("--max-restore-ms must be >= 0")
    if args.min_target_entities < 0:
        parser.error("--min-target-entities must be >= 0")
    if args.min_target_leaves < 0:
        parser.error("--min-target-leaves must be >= 0")
    if args.min_restores < 0:
        parser.error("--min-restores must be >= 0")
    if args.min_ghost_backup_restores < 0:
        parser.error("--min-ghost-backup-restores must be >= 0")
    if args.min_payload_restores < 0:
        parser.error("--min-payload-restores must be >= 0")
    if args.min_promoted_restores < 0:
        parser.error("--min-promoted-restores must be >= 0")
    if args.min_total_scheduled_restores < 0:
        parser.error("--min-total-scheduled-restores must be >= 0")
    if args.min_total_payload_restores < 0:
        parser.error("--min-total-payload-restores must be >= 0")
    if args.min_total_ghost_backup_restores < 0:
        parser.error("--min-total-ghost-backup-restores must be >= 0")
    if args.min_total_promoted_restores < 0:
        parser.error("--min-total-promoted-restores must be >= 0")
    if args.min_restore_latency_samples < 0:
        parser.error("--min-restore-latency-samples must be >= 0")
    if not 0.0 <= args.min_payload_restore_share <= 1.0:
        parser.error("--min-payload-restore-share must be between 0 and 1")
    if not 0.0 <= args.min_ghost_backup_restore_share <= 1.0:
        parser.error("--min-ghost-backup-restore-share must be between 0 and 1")
    if not 0.0 <= args.min_promoted_restore_share <= 1.0:
        parser.error("--min-promoted-restore-share must be between 0 and 1")
    if args.allow_no_baseapp and args.min_restores > 0:
        parser.error("--allow-no-baseapp cannot be combined with --min-restores")
    if args.allow_no_baseapp and args.min_ghost_backup_restores > 0:
        parser.error("--allow-no-baseapp cannot be combined with --min-ghost-backup-restores")
    if args.allow_no_baseapp and args.min_payload_restores > 0:
        parser.error("--allow-no-baseapp cannot be combined with --min-payload-restores")
    if args.allow_no_baseapp and args.min_promoted_restores > 0:
        parser.error("--allow-no-baseapp cannot be combined with --min-promoted-restores")
    if args.allow_no_baseapp and args.min_total_scheduled_restores > 0:
        parser.error("--allow-no-baseapp cannot be combined with --min-total-scheduled-restores")
    if args.allow_no_baseapp and args.min_total_payload_restores > 0:
        parser.error("--allow-no-baseapp cannot be combined with --min-total-payload-restores")
    if args.allow_no_baseapp and args.min_total_ghost_backup_restores > 0:
        parser.error(
            "--allow-no-baseapp cannot be combined with --min-total-ghost-backup-restores"
        )
    if args.allow_no_baseapp and args.min_total_promoted_restores > 0:
        parser.error("--allow-no-baseapp cannot be combined with --min-total-promoted-restores")
    if args.allow_no_baseapp and args.min_restore_latency_samples > 0:
        parser.error("--allow-no-baseapp cannot be combined with --min-restore-latency-samples")
    if args.allow_no_baseapp and args.min_payload_restore_share > 0:
        parser.error("--allow-no-baseapp cannot be combined with --min-payload-restore-share")
    if args.allow_no_baseapp and args.min_ghost_backup_restore_share > 0:
        parser.error(
            "--allow-no-baseapp cannot be combined with --min-ghost-backup-restore-share"
        )
    if args.allow_no_baseapp and args.min_promoted_restore_share > 0:
        parser.error("--allow-no-baseapp cannot be combined with --min-promoted-restore-share")
    if args.allow_no_baseapp and args.max_restore_ms > 0:
        parser.error("--allow-no-baseapp cannot be combined with --max-restore-ms")
    if args.cycles > 1 and (args.target_app_id or args.target_name):
        parser.error("--cycles > 1 cannot be combined with a fixed target")
    return args


def run_atlas_tool(exe: Path, machined: str, *cmd: str) -> str:
    proc = subprocess.run(
        [str(exe), "--machined", machined, *cmd],
        capture_output=True,
        text=True,
        timeout=10,
        cwd=REPO_ROOT,
    )
    if proc.returncode != 0:
        detail = proc.stderr.strip() or proc.stdout.strip()
        raise RuntimeError(f"atlas_tool {' '.join(cmd)} failed: {detail}")
    return proc.stdout


def list_processes(exe: Path, machined: str, process_type: str) -> list[dict[str, str]]:
    out = run_atlas_tool(exe, machined, "list", process_type)
    processes: list[dict[str, str]] = []
    for raw in out.splitlines():
        match = PROC_RE.match(raw.strip())
        if match:
            processes.append(match.groupdict())
    return processes


def watcher_value(exe: Path, machined: str, target: str, path: str) -> str:
    out = run_atlas_tool(exe, machined, "watch", target, path)
    parts = out.strip().split(maxsplit=1)
    if len(parts) != 2:
        raise RuntimeError(f"unexpected watcher output for {path}: {out.strip()}")
    return parts[1]


def int_watcher(exe: Path, machined: str, target: str, path: str) -> int:
    value = watcher_value(exe, machined, target, path)
    try:
        return int(value)
    except ValueError as ex:
        raise RuntimeError(f"watcher {path} returned non-integer value {value!r}") from ex


def shutdown_process(exe: Path, machined: str, target: str, reason: int) -> None:
    run_atlas_tool(exe, machined, "shutdown", target, str(reason))


def is_pid_alive(pid: int) -> bool:
    # machined dropping a target from its registry only proves the process
    # disconnected; a wedged cellapp can still hold its port / lock / lease.
    if pid <= 0:
        return False
    if platform.system() == "Windows":
        try:
            out = subprocess.run(
                ["tasklist", "/NH", "/FO", "CSV", "/FI", f"PID eq {pid}"],
                capture_output=True, text=True, check=True, timeout=5,
            ).stdout
        except (subprocess.SubprocessError, OSError):
            return True  # probe failure → assume alive, force caller retry
        return f'"{pid}"' in out
    try:
        os.kill(pid, 0)
        return True
    except OSError as ex:
        if ex.errno == errno.ESRCH:
            return False
        return True  # EPERM / other → process exists but we lack rights


def parse_leaf_owners(summary: str) -> dict[int, int]:
    counts: dict[int, int] = {}
    for match in LEAF_APP_RE.finditer(summary):
        app_id = int(match.group(1))
        if app_id:
            counts[app_id] = counts.get(app_id, 0) + 1
    return counts


def parse_space_count(summary: str) -> int:
    match = re.search(r"\bspaces=(\d+)\b", summary)
    return int(match.group(1)) if match else 0


def parse_cellapp_registry(summary: str) -> dict[str, int]:
    return {match.group("addr"): int(match.group("app")) for match in CELLAPP_RE.finditer(summary)}


def parse_cellapp_entities_by_app(summary: str) -> dict[int, int]:
    return {
        int(match.group("app")): int(match.group("entities"))
        for match in CELLAPP_RE.finditer(summary)
    }


def parse_baseapp_route_summary(summary: str) -> dict[str, dict[str, int]]:
    routes: dict[str, dict[str, int]] = {}
    for match in BASEAPP_ROUTE_RE.finditer(summary):
        addr = match.group("addr")
        current = routes.setdefault(
            addr,
            {"entities": 0, "payload_candidates": 0, "ghost_backup_candidates": 0},
        )
        for key in current:
            current[key] += int(match.group(key))
    return routes


def parse_baseapp_routes(summary: str) -> dict[str, int]:
    return {
        addr: counts["entities"]
        for addr, counts in parse_baseapp_route_summary(summary).items()
    }


def app_id_from_process(proc: dict[str, str], app_ids_by_addr: dict[str, int]) -> int:
    app_id = app_ids_by_addr.get(proc["addr"])
    if app_id is None:
        raise RuntimeError(f"CellApp {proc['name']} addr={proc['addr']} missing from lb/cellapps")
    return app_id


def choose_target(
    args: Namespace,
    apps: list[dict[str, str]],
    leaf_counts: dict[int, int],
    app_ids_by_addr: dict[str, int],
    entities_by_app: dict[int, int],
    base_routes_by_addr: dict[str, int],
    base_payload_routes_by_addr: dict[str, int],
    base_ghost_routes_by_addr: dict[str, int],
) -> dict[str, str]:
    def app_id(app: dict[str, str]) -> int:
        return app_id_from_process(app, app_ids_by_addr)

    def app_leaf_count(app: dict[str, str]) -> int:
        return leaf_counts.get(app_id(app), 0)

    def app_entity_count(app: dict[str, str]) -> int:
        return entities_by_app.get(app_id(app), 0)

    min_target_entities = getattr(args, "min_target_entities", 0)
    min_target_leaves = getattr(args, "min_target_leaves", 0)

    def require_target_capacity(app: dict[str, str]) -> None:
        app_id_value = app_id_from_process(app, app_ids_by_addr)
        leaves = leaf_counts.get(app_id_value, 0)
        if min_target_leaves > 0 and leaves < min_target_leaves:
            raise RuntimeError(
                f"target app_id={app_id_value} has leaves={leaves}; "
                f"--min-target-leaves requires >= {min_target_leaves}"
            )
        entities = entities_by_app.get(app_id_value, 0)
        if min_target_entities > 0 and entities < min_target_entities:
            raise RuntimeError(
                f"target app_id={app_id_value} has entities={entities}; "
                f"--min-target-entities requires >= {min_target_entities}"
            )
        routes = base_routes_by_addr.get(app["addr"], 0)
        if args.min_restores > 0 and routes < args.min_restores:
            raise RuntimeError(
                f"target app_id={app_id_value} has base_routes={routes}; "
                f"--min-restores requires >= {args.min_restores}"
            )
        payload_routes = base_payload_routes_by_addr.get(app["addr"], 0)
        if args.min_payload_restores > 0 and payload_routes < args.min_payload_restores:
            raise RuntimeError(
                f"target app_id={app_id_value} has payload_candidates={payload_routes}; "
                f"--min-payload-restores requires >= {args.min_payload_restores}"
            )
        ghost_routes = base_ghost_routes_by_addr.get(app["addr"], 0)
        if args.min_ghost_backup_restores > 0 and ghost_routes < args.min_ghost_backup_restores:
            raise RuntimeError(
                f"target app_id={app_id_value} has ghost_backup_candidates={ghost_routes}; "
                "--min-ghost-backup-restores requires >= "
                f"{args.min_ghost_backup_restores}"
            )

    if args.target_name:
        for app in apps:
            if app["name"] == args.target_name:
                require_target_capacity(app)
                return app
        raise RuntimeError(f"--target-name {args.target_name} is not registered")

    if args.target_app_id:
        for app in apps:
            if app_id(app) == args.target_app_id:
                require_target_capacity(app)
                return app
        if leaf_counts.get(args.target_app_id, 0) == 0:
            raise RuntimeError(f"--target-app-id {args.target_app_id} owns no BSP leaves")
        raise RuntimeError(f"--target-app-id {args.target_app_id} is not registered")

    owned = [app for app in apps if app_leaf_count(app) > 0]
    if not owned:
        raise RuntimeError("no registered CellApp owns any BSP leaf")
    if (
        args.min_restores > 0
        or args.min_payload_restores > 0
        or args.min_ghost_backup_restores > 0
        or min_target_entities > 0
        or min_target_leaves > 0
    ):
        owned = [
            app
            for app in owned
            if app_leaf_count(app) >= min_target_leaves
            and app_entity_count(app) >= min_target_entities
            and base_routes_by_addr.get(app["addr"], 0) >= args.min_restores
            and base_payload_routes_by_addr.get(app["addr"], 0)
            >= args.min_payload_restores
            and base_ghost_routes_by_addr.get(app["addr"], 0)
            >= args.min_ghost_backup_restores
        ]
        if not owned:
            raise RuntimeError(
                "no leaf-owning CellApp satisfies target and restore gates "
                f"for --min-target-leaves {min_target_leaves} and "
                f"--min-target-entities {min_target_entities} and "
                f"--min-restores {args.min_restores} and "
                f"--min-payload-restores {args.min_payload_restores} and "
                f"--min-ghost-backup-restores {args.min_ghost_backup_restores}"
            )
        return max(
            owned,
            key=lambda app: (
                base_payload_routes_by_addr.get(app["addr"], 0)
                if args.min_payload_restores > 0
                else 0,
                base_ghost_routes_by_addr.get(app["addr"], 0)
                if args.min_ghost_backup_restores > 0
                else 0,
                base_routes_by_addr.get(app["addr"], 0) if args.min_restores > 0 else 0,
                app_entity_count(app),
                app_leaf_count(app),
                app["name"],
            ),
        )
    return max(
        owned,
        key=lambda app: (app_leaf_count(app), app["name"]),
    )


def baseapp_restore_snapshot(
    exe: Path, machined: str, baseapps: list[dict[str, str]]
) -> dict[str, dict[str, int]]:
    snapshots: dict[str, dict[str, int]] = {}
    for baseapp in baseapps:
        target = f"baseapp:{baseapp['name']}"
        snapshots[baseapp["name"]] = {
            key: int_watcher(exe, machined, target, path)
            for key, path in BASEAPP_RESTORE_WATCHERS.items()
        }
    return snapshots


def baseapp_route_counts(
    exe: Path, machined: str, baseapps: list[dict[str, str]]
) -> dict[str, dict[str, int]]:
    counts: dict[str, dict[str, int]] = {}
    for baseapp in baseapps:
        target = f"baseapp:{baseapp['name']}"
        for addr, route_counts in parse_baseapp_route_summary(
            watcher_value(exe, machined, target, "baseapp/cellapp_routes")
        ).items():
            current = counts.setdefault(
                addr,
                {"entities": 0, "payload_candidates": 0, "ghost_backup_candidates": 0},
            )
            for key in current:
                current[key] += route_counts[key]
    return counts


def cellapp_restore_snapshot(
    exe: Path, machined: str, cellapps: list[dict[str, str]]
) -> dict[str, dict[str, int]]:
    snapshots: dict[str, dict[str, int]] = {}
    for cellapp in cellapps:
        target = f"cellapp:{cellapp['name']}"
        snapshots[cellapp["name"]] = {
            key: int_watcher(exe, machined, target, path)
            for key, path in CELLAPP_RESTORE_WATCHERS.items()
        }
    return snapshots


def baseapp_restore_scheduled_delta(
    before: dict[str, dict[str, int]], after: dict[str, dict[str, int]]
) -> int:
    return baseapp_restore_counter_delta(before, after, "scheduled")


def baseapp_restore_counter_delta(
    before: dict[str, dict[str, int]], after: dict[str, dict[str, int]], key: str
) -> int:
    total = 0
    for name, baseline in before.items():
        current = after.get(name)
        if current is not None:
            total += current[key] - baseline[key]
    return total


def baseapp_restore_cycle_elapsed_ms(
    before: dict[str, dict[str, int]], after: dict[str, dict[str, int]]
) -> int:
    elapsed_ms = 0
    for name, baseline in before.items():
        current = after.get(name)
        if current is not None and current["scheduled"] > baseline["scheduled"]:
            elapsed_ms = max(elapsed_ms, current["last_elapsed_ms"])
    return elapsed_ms


def baseapp_restore_health_detail(
    before: dict[str, dict[str, int]],
    after: dict[str, dict[str, int]],
    max_restore_ms: int | None = None,
) -> tuple[bool, str]:
    if not before:
        return True, "baseapps=0"

    ok = True
    details: list[str] = []
    for name, baseline in sorted(before.items()):
        current = after.get(name)
        if current is None:
            details.append(f"{name}=missing")
            ok = False
            continue
        deltas = {
            key: current[key] - baseline[key] for key in BASEAPP_RESTORE_COUNTER_WATCHERS
        }
        monotonic = all(value >= 0 for value in deltas.values())
        monotonic = monotonic and current["max_elapsed_ms"] >= baseline["max_elapsed_ms"]
        notifications_ok = deltas["notifications"] > 0
        restored_ok = deltas["restored"] >= deltas["scheduled"]
        scheduled_source_ok = (
            deltas["payload_scheduled"] + deltas["ghost_backup_scheduled"]
            == deltas["scheduled"]
        )
        loss_ok = deltas["lost"] == 0 and deltas["timeouts"] == 0
        pending_ok = current["pending"] == 0
        elapsed_ok = True
        if max_restore_ms is not None and deltas["scheduled"] > 0:
            last_ok = current["last_elapsed_ms"] <= max_restore_ms
            max_ok = (
                current["max_elapsed_ms"] == baseline["max_elapsed_ms"]
                or current["max_elapsed_ms"] <= max_restore_ms
            )
            elapsed_ok = last_ok and max_ok
        base_ok = (
            monotonic
            and notifications_ok
            and restored_ok
            and scheduled_source_ok
            and loss_ok
            and pending_ok
            and elapsed_ok
        )
        ok = ok and base_ok
        details.append(
            f"{name}=ok:{int(base_ok)} notif:{deltas['notifications']} "
            f"scheduled:{deltas['scheduled']} restored:{deltas['restored']} "
            f"payload_scheduled:{deltas['payload_scheduled']} "
            f"ghost_backup_scheduled:{deltas['ghost_backup_scheduled']} "
            f"lost:{deltas['lost']} timeouts:{deltas['timeouts']} "
            f"pending:{current['pending']} last_ms:{current['last_elapsed_ms']} "
            f"max_ms:{current['max_elapsed_ms']}"
        )
    return ok, ";".join(details)


def cellapp_restore_health_detail(
    before: dict[str, dict[str, int]],
    after: dict[str, dict[str, int]],
    scheduled_delta: int,
    payload_scheduled_delta: int = 0,
    ghost_backup_scheduled_delta: int = 0,
) -> tuple[bool, str]:
    if not before:
        return True, "cellapps=0"

    totals = {key: 0 for key in CELLAPP_RESTORE_WATCHERS}
    missing: list[str] = []
    for name, current in sorted(after.items()):
        baseline = before.get(name)
        if baseline is None:
            missing.append(f"{name}=missing_baseline")
            continue
        for key in CELLAPP_RESTORE_WATCHERS:
            totals[key] += current[key] - baseline[key]

    source_restores = totals["payload"] + totals["ghost_backup"]
    total_ok = scheduled_delta <= 0 or totals["total"] >= scheduled_delta
    failures_ok = totals["failures"] == 0
    empty_ok = totals["empty"] == 0
    source_ok = scheduled_delta <= 0 or source_restores >= scheduled_delta
    payload_ok = totals["payload"] >= payload_scheduled_delta
    ghost_backup_ok = totals["ghost_backup"] >= ghost_backup_scheduled_delta
    ok = (
        total_ok
        and failures_ok
        and empty_ok
        and source_ok
        and payload_ok
        and ghost_backup_ok
        and not missing
    )
    detail = (
        f"ok:{int(ok)} scheduled:{scheduled_delta} total:{totals['total']} "
        f"payload:{totals['payload']} "
        f"ghost_backup:{totals['ghost_backup']} empty:{totals['empty']} "
        f"failures:{totals['failures']} promoted:{totals['promoted']} "
        f"payload_expected:{payload_scheduled_delta} "
        f"ghost_backup_expected:{ghost_backup_scheduled_delta}"
    )
    if missing:
        detail = f"{detail} missing:{','.join(missing)}"
    return ok, detail


def restore_volume_health_detail(scheduled_delta: int, min_restores: int) -> tuple[bool, str]:
    ok = scheduled_delta >= min_restores
    return ok, f"scheduled:{scheduled_delta} min:{min_restores}"


def cellapp_restore_delta(
    before: dict[str, dict[str, int]],
    after: dict[str, dict[str, int]],
    key: str,
) -> int:
    total = 0
    for name, current in after.items():
        baseline = before.get(name)
        if baseline is not None:
            total += current[key] - baseline[key]
    return total


def ghost_backup_volume_health_detail(
    ghost_backup_delta: int, min_ghost_backup_restores: int
) -> tuple[bool, str]:
    ok = ghost_backup_delta >= min_ghost_backup_restores
    return ok, f"ghost_backup:{ghost_backup_delta} min:{min_ghost_backup_restores}"


def payload_volume_health_detail(
    payload_delta: int, min_payload_restores: int
) -> tuple[bool, str]:
    ok = payload_delta >= min_payload_restores
    return ok, f"payload:{payload_delta} min:{min_payload_restores}"


def promoted_volume_health_detail(
    promoted_delta: int, min_promoted_restores: int
) -> tuple[bool, str]:
    ok = promoted_delta >= min_promoted_restores
    return ok, f"promoted:{promoted_delta} min:{min_promoted_restores}"


def percentile_nearest_rank_ms(values: list[int], percentile: int) -> int:
    return percentile_nearest_rank(values, percentile)


def restore_latency_summary(results: list[RehomeCycleResult]) -> dict[str, int | float]:
    samples = [result.restore_elapsed_ms for result in results if result.scheduled > 0]
    return latency_summary_ms(samples, "restore")


def expected_coverage_rate(numerator: int, denominator: int) -> float:
    if denominator <= 0:
        return 1.0
    return round(numerator / denominator, 6)


def observed_share(numerator: int, denominator: int) -> float:
    if denominator <= 0:
        return 0.0
    return round(numerator / denominator, 6)


def summary_gate_minimums(args: argparse.Namespace) -> dict[str, int | float]:
    return {parameter: getattr(args, parameter, 0) for parameter in SUMMARY_GATES}


def summary_gate_evaluations(
    summary: dict[str, int | float],
    minimums: dict[str, object],
) -> dict[str, dict[str, object]]:
    gates: dict[str, dict[str, object]] = {}
    for parameter, metric in SUMMARY_GATES.items():
        minimum = minimums.get(parameter, 0)
        if (
            isinstance(minimum, bool)
            or not isinstance(minimum, (int, float))
            or minimum <= 0
        ):
            continue
        value = summary[metric]
        gates[parameter] = {
            "metric": metric,
            "value": value,
            "minimum": minimum,
            "ok": value >= minimum,
        }
    return gates


def summarize_cycles(results: list[RehomeCycleResult]) -> str:
    summary = summarize_cycle_metrics(results)
    return (
        f"cycles={summary['cycles']} target_leaves={summary['target_leaves']} "
        f"target_entities={summary['target_entities']} "
        f"target_base_routes={summary['target_base_routes']} "
        f"target_payload_candidates={summary['target_payload_candidates']} "
        f"target_ghost_backup_candidates={summary['target_ghost_backup_candidates']} "
        f"scheduled={summary['scheduled']} "
        f"restored={summary['restored']} "
        f"payload_scheduled={summary['payload_scheduled']} "
        f"ghost_backup_scheduled={summary['ghost_backup_scheduled']} "
        f"lost={summary['lost']} timeouts={summary['timeouts']} "
        f"cell_total={summary['cell_total']} cell_payload={summary['cell_payload']} "
        f"cell_ghost_backup={summary['cell_ghost_backup']} "
        f"cell_promoted={summary['cell_promoted']} "
        f"cell_empty={summary['cell_empty']} cell_failures={summary['cell_failures']} "
        f"restore_completion_rate={summary['restore_completion_rate']} "
        f"restore_source_coverage_rate={summary['restore_source_coverage_rate']} "
        f"payload_expected_coverage_rate={summary['payload_expected_coverage_rate']} "
        f"ghost_backup_expected_coverage_rate="
        f"{summary['ghost_backup_expected_coverage_rate']} "
        f"payload_restore_share={summary['payload_restore_share']} "
        f"ghost_backup_restore_share={summary['ghost_backup_restore_share']} "
        f"promoted_restore_share={summary['promoted_restore_share']} "
        f"restore_latency_samples={summary['restore_latency_samples']} "
        f"avg_restore_ms={summary['avg_restore_ms']} "
        f"p50_restore_ms={summary['p50_restore_ms']} "
        f"p95_restore_ms={summary['p95_restore_ms']} "
        f"max_restore_ms={summary['max_restore_ms']}"
    )


def summarize_cycle_metrics(results: list[RehomeCycleResult]) -> dict[str, int | float]:
    target_leaves = sum(result.target_leaf_count for result in results)
    target_entities = sum(result.target_entities for result in results)
    target_base_routes = sum(result.target_base_routes for result in results)
    target_payload_candidates = sum(result.target_payload_candidates for result in results)
    target_ghost_backup_candidates = sum(
        result.target_ghost_backup_candidates for result in results
    )
    scheduled = sum(result.scheduled for result in results)
    payload_scheduled = sum(result.payload_scheduled for result in results)
    ghost_backup_scheduled = sum(result.ghost_backup_scheduled for result in results)
    restored = sum(result.restored for result in results)
    lost = sum(result.lost for result in results)
    timeouts = sum(result.timeouts for result in results)
    cell_total = sum(result.cell_total for result in results)
    cell_payload = sum(result.cell_payload for result in results)
    cell_ghost_backup = sum(result.cell_ghost_backup for result in results)
    cell_empty = sum(result.cell_empty for result in results)
    cell_failures = sum(result.cell_failures for result in results)
    cell_promoted = sum(result.cell_promoted for result in results)
    latency = restore_latency_summary(results)
    source_restores = cell_payload + cell_ghost_backup
    return {
        "cycles": len(results),
        "success_rate": 1.0 if results else 0.0,
        "target_leaves": target_leaves,
        "target_entities": target_entities,
        "target_base_routes": target_base_routes,
        "target_payload_candidates": target_payload_candidates,
        "target_ghost_backup_candidates": target_ghost_backup_candidates,
        "scheduled": scheduled,
        "payload_scheduled": payload_scheduled,
        "ghost_backup_scheduled": ghost_backup_scheduled,
        "restored": restored,
        "lost": lost,
        "timeouts": timeouts,
        "cell_total": cell_total,
        "cell_payload": cell_payload,
        "cell_ghost_backup": cell_ghost_backup,
        "cell_empty": cell_empty,
        "cell_failures": cell_failures,
        "cell_promoted": cell_promoted,
        "restore_completion_rate": expected_coverage_rate(restored, scheduled),
        "restore_source_coverage_rate": expected_coverage_rate(source_restores, scheduled),
        "payload_expected_coverage_rate": expected_coverage_rate(
            cell_payload, payload_scheduled
        ),
        "ghost_backup_expected_coverage_rate": expected_coverage_rate(
            cell_ghost_backup, ghost_backup_scheduled
        ),
        "payload_restore_share": observed_share(cell_payload, cell_total),
        "ghost_backup_restore_share": observed_share(cell_ghost_backup, cell_total),
        "promoted_restore_share": observed_share(cell_promoted, cell_total),
        **latency,
    }


def validate_summary_gates(args: argparse.Namespace, results: list[RehomeCycleResult]) -> None:
    summary = summarize_cycle_metrics(results)
    minimums = summary_gate_minimums(args)
    for parameter, gate in summary_gate_evaluations(summary, minimums).items():
        if gate["ok"]:
            continue
        raise RuntimeError(
            f"{gate['metric']} below {parameter}: "
            f"{gate['value']}<{gate['minimum']}"
        )


def summary_parameters(args: argparse.Namespace) -> dict[str, object]:
    return {
        "build": args.build,
        "machined": args.machined,
        "atlas_tool": str(args.atlas_tool) if args.atlas_tool else "",
        "target_app_id": args.target_app_id,
        "target_name": args.target_name,
        "min_cellapps": args.min_cellapps,
        "min_spaces": args.min_spaces,
        "timeout_sec": args.timeout_sec,
        "poll_sec": args.poll_sec,
        "cycles": args.cycles,
        "max_restore_ms": args.max_restore_ms,
        "min_target_entities": args.min_target_entities,
        "min_target_leaves": args.min_target_leaves,
        "min_restores": args.min_restores,
        "min_ghost_backup_restores": args.min_ghost_backup_restores,
        "min_payload_restores": args.min_payload_restores,
        "min_promoted_restores": args.min_promoted_restores,
        "min_total_scheduled_restores": args.min_total_scheduled_restores,
        "min_total_payload_restores": args.min_total_payload_restores,
        "min_total_ghost_backup_restores": args.min_total_ghost_backup_restores,
        "min_total_promoted_restores": args.min_total_promoted_restores,
        "min_restore_latency_samples": args.min_restore_latency_samples,
        "min_payload_restore_share": args.min_payload_restore_share,
        "min_ghost_backup_restore_share": args.min_ghost_backup_restore_share,
        "min_promoted_restore_share": args.min_promoted_restore_share,
        "shutdown_reason": args.shutdown_reason,
        "allow_no_baseapp": args.allow_no_baseapp,
    }


def build_summary_payload(
    results: list[RehomeCycleResult],
    parameters: dict[str, object] | None = None,
) -> dict[str, object]:
    summary = summarize_cycle_metrics(results)
    payload: dict[str, object] = {
        "schema_version": 1,
        "summary": summary,
        "cycles": [result._asdict() for result in results],
    }
    if parameters is not None:
        payload["parameters"] = parameters
        gates = summary_gate_evaluations(summary, parameters)
        if gates:
            payload["gates"] = gates
    return payload


def write_summary_json(
    path: Path,
    results: list[RehomeCycleResult],
    parameters: dict[str, object] | None = None,
) -> None:
    try:
        write_json_atomic(path, build_summary_payload(results, parameters))
    except OSError as ex:
        raise RuntimeError(f"failed to write summary JSON {path}: {ex}") from ex


def run_rehome_cycle(args: argparse.Namespace, exe: Path, cycle_index: int) -> RehomeCycleResult:
    apps = list_processes(exe, args.machined, "cellapp")
    if len(apps) < args.min_cellapps:
        raise RuntimeError(f"need >= {args.min_cellapps} registered CellApps; got {len(apps)}")
    spaces_summary = watcher_value(exe, args.machined, "cellappmgr", "cellappmgr/lb/spaces")
    if parse_space_count(spaces_summary) < args.min_spaces:
        raise RuntimeError(f"need >= {args.min_spaces} live Spaces: {spaces_summary}")
    leaf_counts = parse_leaf_owners(spaces_summary)
    cellapps_summary = watcher_value(exe, args.machined, "cellappmgr", "cellappmgr/lb/cellapps")
    app_ids_by_addr = parse_cellapp_registry(cellapps_summary)
    entities_by_app = parse_cellapp_entities_by_app(cellapps_summary)
    baseapps = [] if args.allow_no_baseapp else list_processes(exe, args.machined, "baseapp")
    if not baseapps and not args.allow_no_baseapp:
        raise RuntimeError("no registered BaseApp; pass --allow-no-baseapp to skip it")
    base_route_counts_by_addr = baseapp_route_counts(exe, args.machined, baseapps)
    base_routes_by_addr = {
        addr: counts["entities"] for addr, counts in base_route_counts_by_addr.items()
    }
    base_payload_routes_by_addr = {
        addr: counts["payload_candidates"]
        for addr, counts in base_route_counts_by_addr.items()
    }
    base_ghost_routes_by_addr = {
        addr: counts["ghost_backup_candidates"]
        for addr, counts in base_route_counts_by_addr.items()
    }
    target = choose_target(
        args,
        apps,
        leaf_counts,
        app_ids_by_addr,
        entities_by_app,
        base_routes_by_addr,
        base_payload_routes_by_addr,
        base_ghost_routes_by_addr,
    )
    target_name = target["name"]
    target_app_id = app_id_from_process(target, app_ids_by_addr)
    try:
        target_pid_int = int(target["pid"])
    except (TypeError, ValueError) as ex:
        raise RuntimeError(f"target {target_name} has non-integer pid {target['pid']!r}") from ex
    if leaf_counts.get(target_app_id, 0) == 0:
        raise RuntimeError(f"target app_id={target_app_id} owns no BSP leaves")
    target_entities = entities_by_app.get(target_app_id, 0)
    target_base_routes = base_routes_by_addr.get(target["addr"], 0)
    target_base_payload_routes = base_payload_routes_by_addr.get(target["addr"], 0)
    target_base_ghost_routes = base_ghost_routes_by_addr.get(target["addr"], 0)
    before_base_restore = baseapp_restore_snapshot(exe, args.machined, baseapps)
    before_cellapp_restore = cellapp_restore_snapshot(exe, args.machined, apps)
    before_decisions = int_watcher(
        exe, args.machined, "cellappmgr", "cellappmgr/lb/decision_count"
    )
    before_cellapp_count = int_watcher(
        exe, args.machined, "cellappmgr", "cellappmgr/cellapp_count"
    )

    print(
        f"[verify_cellapp_rehome] cycle={cycle_index} killing {target_name} "
        f"app_id={target_app_id}; leaves={leaf_counts[target_app_id]} "
        f"entities={target_entities} base_routes={target_base_routes} "
        f"payload_candidates={target_base_payload_routes} "
        f"ghost_backup_candidates={target_base_ghost_routes}"
    )
    shutdown_process(exe, args.machined, f"cellapp:{target_name}", int(args.shutdown_reason))

    deadline = time.monotonic() + args.timeout_sec
    last = ""
    max_restore_ms = args.max_restore_ms if args.max_restore_ms > 0 else None
    while time.monotonic() < deadline:
        apps_after = list_processes(exe, args.machined, "cellapp")
        names_after = {app["name"] for app in apps_after}
        spaces_after = watcher_value(exe, args.machined, "cellappmgr", "cellappmgr/lb/spaces")
        leaf_counts_after = parse_leaf_owners(spaces_after)
        decisions_after = int_watcher(
            exe, args.machined, "cellappmgr", "cellappmgr/lb/decision_count"
        )
        cellapp_count_after = int_watcher(
            exe, args.machined, "cellappmgr", "cellappmgr/cellapp_count"
        )
        last_decision = watcher_value(
            exe, args.machined, "cellappmgr", "cellappmgr/lb/last_decision"
        )
        decision_history = watcher_value(
            exe, args.machined, "cellappmgr", "cellappmgr/lb/decision_history"
        )
        base_restore_after = baseapp_restore_snapshot(exe, args.machined, baseapps)
        base_ok, base_detail = baseapp_restore_health_detail(
            before_base_restore, base_restore_after, max_restore_ms=max_restore_ms
        )
        scheduled_delta = baseapp_restore_scheduled_delta(before_base_restore, base_restore_after)
        payload_scheduled_delta = baseapp_restore_counter_delta(
            before_base_restore, base_restore_after, "payload_scheduled"
        )
        ghost_backup_scheduled_delta = baseapp_restore_counter_delta(
            before_base_restore, base_restore_after, "ghost_backup_scheduled"
        )
        restored_delta = baseapp_restore_counter_delta(
            before_base_restore, base_restore_after, "restored"
        )
        lost_delta = baseapp_restore_counter_delta(before_base_restore, base_restore_after, "lost")
        timeout_delta = baseapp_restore_counter_delta(
            before_base_restore, base_restore_after, "timeouts"
        )
        restore_elapsed_ms = baseapp_restore_cycle_elapsed_ms(
            before_base_restore, base_restore_after
        )
        volume_ok, volume_detail = restore_volume_health_detail(
            scheduled_delta, args.min_restores
        )
        cellapp_restore_after = cellapp_restore_snapshot(exe, args.machined, apps_after)
        cell_total_delta = cellapp_restore_delta(
            before_cellapp_restore, cellapp_restore_after, "total"
        )
        cell_payload_delta = cellapp_restore_delta(
            before_cellapp_restore, cellapp_restore_after, "payload"
        )
        cell_ok, cell_detail = cellapp_restore_health_detail(
            before_cellapp_restore,
            cellapp_restore_after,
            scheduled_delta,
            payload_scheduled_delta,
            ghost_backup_scheduled_delta,
        )
        ghost_backup_delta = cellapp_restore_delta(
            before_cellapp_restore, cellapp_restore_after, "ghost_backup"
        )
        cell_empty_delta = cellapp_restore_delta(
            before_cellapp_restore, cellapp_restore_after, "empty"
        )
        cell_failures_delta = cellapp_restore_delta(
            before_cellapp_restore, cellapp_restore_after, "failures"
        )
        cell_promoted_delta = cellapp_restore_delta(
            before_cellapp_restore, cellapp_restore_after, "promoted"
        )
        ghost_volume_ok, ghost_volume_detail = ghost_backup_volume_health_detail(
            ghost_backup_delta, args.min_ghost_backup_restores
        )
        payload_volume_ok, payload_volume_detail = payload_volume_health_detail(
            cell_payload_delta, args.min_payload_restores
        )
        promoted_volume_ok, promoted_volume_detail = promoted_volume_health_detail(
            cell_promoted_delta, args.min_promoted_restores
        )
        rehomed = leaf_counts_after.get(target_app_id, 0) == 0
        decision_ok = (
            decisions_after > before_decisions
            and (
                "reason=cellapp-death" in last_decision
                or "reason=cellapp-death" in decision_history
            )
        )
        count_ok = cellapp_count_after + 1 == before_cellapp_count
        # machined deregister + PID gone are both required: a wedged cellapp can
        # drop its machined connection while still holding port / lock / lease.
        pid_alive = is_pid_alive(target_pid_int)
        gone = (target_name not in names_after) and not pid_alive
        last = (
            f"cycle={cycle_index} gone={gone} pid_alive={pid_alive} "
            f"rehomed={rehomed} count_ok={count_ok} "
            f"base_ok={base_ok} cell_ok={cell_ok} volume_ok={volume_ok} "
            f"payload_volume_ok={payload_volume_ok} "
            f"ghost_volume_ok={ghost_volume_ok} "
            f"promoted_volume_ok={promoted_volume_ok} "
            f"cycle_metrics=scheduled:{scheduled_delta} restored:{restored_delta} "
            f"payload_scheduled:{payload_scheduled_delta} "
            f"ghost_backup_scheduled:{ghost_backup_scheduled_delta} "
            f"cell_total:{cell_total_delta} cell_payload:{cell_payload_delta} "
            f"cell_ghost_backup:{ghost_backup_delta} cell_promoted:{cell_promoted_delta} "
            f"cell_empty:{cell_empty_delta} cell_failures:{cell_failures_delta} "
            f"restore_elapsed_ms:{restore_elapsed_ms} "
            f"restore_volume={volume_detail} base_restore={base_detail} "
            f"cell_restore={cell_detail} payload_restore_volume={payload_volume_detail} "
            f"ghost_restore_volume={ghost_volume_detail} "
            f"promoted_restore_volume={promoted_volume_detail} "
            f"decisions={before_decisions}->{decisions_after} last_decision={last_decision}"
        )
        result = RehomeCycleResult(
            detail=last,
            scheduled=scheduled_delta,
            payload_scheduled=payload_scheduled_delta,
            ghost_backup_scheduled=ghost_backup_scheduled_delta,
            restored=restored_delta,
            lost=lost_delta,
            timeouts=timeout_delta,
            cell_total=cell_total_delta,
            cell_payload=cell_payload_delta,
            cell_ghost_backup=ghost_backup_delta,
            cell_empty=cell_empty_delta,
            cell_failures=cell_failures_delta,
            cell_promoted=cell_promoted_delta,
            restore_elapsed_ms=restore_elapsed_ms,
            cycle=cycle_index,
            target_name=target_name,
            target_app_id=target_app_id,
            target_pid=target["pid"],
            target_addr=target["addr"],
            target_leaf_count=leaf_counts[target_app_id],
            target_entities=target_entities,
            target_base_routes=target_base_routes,
            target_payload_candidates=target_base_payload_routes,
            target_ghost_backup_candidates=target_base_ghost_routes,
        )
        if (
            gone
            and rehomed
            and count_ok
            and base_ok
            and cell_ok
            and volume_ok
            and payload_volume_ok
            and ghost_volume_ok
            and promoted_volume_ok
            and decision_ok
        ):
            print(f"[verify_cellapp_rehome] PASS {result.detail}")
            return result
        time.sleep(args.poll_sec)

    raise RuntimeError(f"timeout waiting for CellApp rehome; last={last}")


def main() -> int:
    args = parse_args()
    exe = args.atlas_tool or default_atlas_tool(args.build)
    if not exe.is_file():
        print(f"[verify_cellapp_rehome] atlas_tool not found: {exe}", file=sys.stderr)
        print("[verify_cellapp_rehome] build it with tools/bin/build first", file=sys.stderr)
        return 1

    try:
        results: list[RehomeCycleResult] = []
        for cycle_index in range(1, args.cycles + 1):
            results.append(run_rehome_cycle(args, exe, cycle_index))
        if args.summary_json:
            write_summary_json(args.summary_json, results, summary_parameters(args))
        validate_summary_gates(args, results)
        if len(results) > 1:
            print(
                f"[verify_cellapp_rehome] PASS {summarize_cycles(results)} "
                f"last={results[-1].detail}"
            )
        return 0
    except RuntimeError as ex:
        print(f"[verify_cellapp_rehome] {ex}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
