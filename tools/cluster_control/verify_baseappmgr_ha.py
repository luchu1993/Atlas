#!/usr/bin/env python3
"""Validate BaseAppMgr HA restart on a live Atlas cluster.

Mirrors verify_cellappmgr_ha.py for the BaseAppMgr target — drives
abnormal shutdown via machined, waits for the Reviver to relaunch
BaseAppMgr, and checks that the new mgr restored its snapshot,
re-acquired heartbeats, and the surviving BaseApps reattached.

Required cluster shape: at least one Reviver supervising BaseAppMgr
(needs --revive-baseappmgr-on-start true on the Reviver process).
"""

from __future__ import annotations

import argparse
import json
import platform
import re
import subprocess
import sys
import time
from collections import OrderedDict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Optional

REPO_ROOT = Path(__file__).resolve().parents[2]

SCHEMA_VERSION = 1

PROC_RE = re.compile(
    r"^(?P<type>\w+)\s+(?P<name>\S+)\s+(?P<addr>\S+)\s+(?P<pid>\d+)\s+(?P<load>[\d.]+)%"
)


def default_atlas_tool(build_subdir: str) -> Path:
    exe = "atlas_tool.exe" if platform.system() == "Windows" else "atlas_tool"
    return REPO_ROOT / "bin" / build_subdir / exe


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Validate BaseAppMgr HA restart on a live Atlas cluster."
    )
    p.add_argument("--build", default="debug",
                   help="bin/<build>/atlas_tool directory (default: debug)")
    p.add_argument("--machined", default="127.0.0.1:20018",
                   help="machined host:port (default: 127.0.0.1:20018)")
    p.add_argument("--atlas-tool", type=Path, default=None,
                   help="explicit atlas_tool path (default: None)")
    p.add_argument("--baseappmgr-name", default="baseappmgr",
                   help="target BaseAppMgr name (default: baseappmgr)")
    p.add_argument("--reviver-name", default="",
                   help="target Reviver name (default: <single registered>)")
    p.add_argument("--min-revivers", type=int, default=1,
                   help="minimum registered Revivers (default: 1)")
    p.add_argument("--min-baseapps", type=int, default=1,
                   help="minimum registered BaseApps (default: 1)")
    p.add_argument("--timeout-sec", type=float, default=60.0)
    p.add_argument("--poll-sec", type=float, default=1.0)
    p.add_argument("--stability-sec", type=float, default=5.0,
                   help="post-restore window that must not show another Reviver restart")
    p.add_argument("--cycles", type=int, default=1,
                   help="number of abnormal BaseAppMgr restart cycles to inject (default: 1)")
    p.add_argument("--max-takeover-ms", type=int, default=0,
                   help="maximum allowed shutdown-to-fresh-snapshot takeover time; 0 disables")
    p.add_argument("--shutdown-reason", type=int, default=1,
                   help="machined shutdown reason (default: 1)")
    p.add_argument("--allow-empty-snapshot", action="store_true",
                   help="do not require baseappmgr/ha/snapshot_path or snapshot_saves > 0")
    p.add_argument("--no-inject", action="store_true",
                   help="only verify current watchers without shutting down BaseAppMgr")
    p.add_argument("--summary-json", type=Path, default=None,
                   help="write a machine-readable summary JSON after checks complete")
    return p.parse_args()


def run_atlas_tool(exe: Path, machined: str, *cmd: str) -> str:
    full = [str(exe), "--machined", machined, *cmd]
    proc = subprocess.run(full, capture_output=True, text=True, timeout=10, cwd=REPO_ROOT)
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


def choose_process(processes: list[dict[str, str]], name: str, process_type: str) -> dict[str, str]:
    matches = [proc for proc in processes if not name or proc["name"] == name]
    if not matches:
        wanted = name or process_type
        raise RuntimeError(f"no registered {process_type} matches {wanted}")
    if len(matches) > 1 and not name:
        raise RuntimeError(
            f"multiple {process_type} processes registered; pass --{process_type}-name"
        )
    return matches[0]


def watcher_value(exe: Path, machined: str, target: str, path: str) -> str:
    out = run_atlas_tool(exe, machined, "watch", target, path)
    stripped = out.strip()
    parts = stripped.split(maxsplit=1)
    target_name = target.split(":", 1)[-1]
    if len(parts) == 1 and parts[0] == target_name:
        return ""
    if len(parts) != 2:
        raise RuntimeError(f"unexpected watcher output for {path}: {stripped}")
    return parts[1]


def int_watcher(exe: Path, machined: str, target: str, path: str) -> int:
    raw = watcher_value(exe, machined, target, path)
    try:
        return int(raw)
    except ValueError as exc:
        raise RuntimeError(f"watcher {path}={raw!r} is not an integer") from exc


def shutdown_process(exe: Path, machined: str, target: str, reason: int) -> None:
    run_atlas_tool(exe, machined, "shutdown", target, str(reason))


def wait_until(timeout_sec: float, poll_sec: float, check: Callable[[], tuple[bool, object]]):
    deadline = time.monotonic() + timeout_sec
    last: object = None
    while True:
        ok, payload = check()
        if ok:
            return payload
        last = payload
        if time.monotonic() >= deadline:
            raise RuntimeError(f"timeout after {timeout_sec}s; last={last}")
        time.sleep(poll_sec)


def summary_fields(status: str) -> "OrderedDict[str, str]":
    fields: OrderedDict[str, str] = OrderedDict()
    for token in status.split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        if key in fields:
            continue
        fields[key] = value
    return fields


def summary_has(fields: "OrderedDict[str, str]", key: str, value: str) -> bool:
    return fields.get(key) == value


@dataclass
class SnapshotHealth:
    saves: int
    restores: int
    fallback_restores: int
    save_failures: int
    restore_failures: int
    failures: int
    backup_skips: int
    dirty: bool
    save_stale: bool
    status: str
    healthy: bool
    detail: str


@dataclass
class ReattachHealth:
    restored: int
    pending: int
    completed_count: int
    stuck: int
    state: str
    status: str
    healthy: bool
    detail: str


@dataclass
class ReviverHealth:
    active: bool
    active_pid: int
    active_generation: int
    launch_count: int
    heartbeat_acks: int
    heartbeat_last_ack_age_ms: int
    last_error: str
    status: str
    healthy: bool
    detail: str


@dataclass
class CycleResult:
    cycle: int
    old_pid: int
    new_pid: int
    takeover_elapsed_ms: int
    snapshot: SnapshotHealth | None
    reattach: ReattachHealth | None
    reviver: ReviverHealth | None
    healthy: bool
    failure_stages: list[str] = field(default_factory=list)


def read_snapshot_health(exe: Path, machined: str, target: str) -> SnapshotHealth:
    saves = int_watcher(exe, machined, target, "baseappmgr/ha/snapshot_saves")
    restores = int_watcher(exe, machined, target, "baseappmgr/ha/snapshot_restores")
    fallback_restores = int_watcher(exe, machined, target,
                                    "baseappmgr/ha/snapshot_fallback_restores")
    save_failures = int_watcher(exe, machined, target, "baseappmgr/ha/snapshot_save_failures")
    restore_failures = int_watcher(exe, machined, target,
                                   "baseappmgr/ha/snapshot_restore_failures")
    failures = int_watcher(exe, machined, target, "baseappmgr/ha/snapshot_failures")
    backup_skips = int_watcher(exe, machined, target, "baseappmgr/ha/snapshot_backup_skips")
    dirty = watcher_value(exe, machined, target, "baseappmgr/ha/snapshot_dirty") == "true"
    save_stale = watcher_value(exe, machined, target, "baseappmgr/ha/snapshot_save_stale") == "true"
    status = watcher_value(exe, machined, target, "baseappmgr/ha/snapshot_status")
    fields = summary_fields(status)
    healthy = (
        save_failures == 0
        and restore_failures == 0
        and failures == 0
        and not save_stale
        and not dirty
        and summary_has(fields, "error_present", "0")
    )
    detail = (
        f"saves={saves} restores={restores} fallback_restores={fallback_restores}"
        f" save_failures={save_failures} restore_failures={restore_failures}"
        f" failures={failures} backup_skips={backup_skips} dirty={dirty}"
        f" save_stale={save_stale} status={status}"
    )
    return SnapshotHealth(saves, restores, fallback_restores, save_failures, restore_failures,
                          failures, backup_skips, dirty, save_stale, status, healthy, detail)


def read_reattach_health(exe: Path, machined: str, target: str,
                         require_restored: bool, min_baseapps: int) -> ReattachHealth:
    restored = int_watcher(exe, machined, target, "baseappmgr/ha/restored_baseapps")
    pending = int_watcher(exe, machined, target, "baseappmgr/ha/reattach_pending")
    completed_count = int_watcher(exe, machined, target,
                                  "baseappmgr/ha/reattach_completed_count")
    stuck = int_watcher(exe, machined, target, "baseappmgr/ha/reattach_stuck")
    state = watcher_value(exe, machined, target, "baseappmgr/ha/reattach_state")
    status = watcher_value(exe, machined, target, "baseappmgr/ha/reattach_status")
    expected_state = ("stuck" if stuck else "pending" if pending
                      else "complete" if restored else "idle")
    counts_ok = pending == 0 and completed_count == restored
    if require_restored:
        counts_ok = counts_ok and restored >= min_baseapps
    healthy = counts_ok and stuck == 0 and state == expected_state
    detail = (
        f"restored={restored} pending={pending} completed_count={completed_count}"
        f" stuck={stuck} state={state}/{expected_state} status={status}"
    )
    return ReattachHealth(restored, pending, completed_count, stuck, state, status, healthy,
                          detail)


def read_reviver_health(exe: Path, machined: str, target: str,
                        require_active_pid: Optional[int] = None) -> ReviverHealth:
    active = watcher_value(exe, machined, target, "reviver/baseappmgr/active") == "true"
    active_pid = int_watcher(exe, machined, target, "reviver/baseappmgr/active_pid")
    active_generation = int_watcher(exe, machined, target,
                                    "reviver/baseappmgr/active_generation")
    launch_count = int_watcher(exe, machined, target, "reviver/baseappmgr/launch_count")
    heartbeat_acks = int_watcher(exe, machined, target, "reviver/baseappmgr/heartbeat_acks")
    age_ms = int_watcher(exe, machined, target, "reviver/baseappmgr/heartbeat_last_ack_age_ms")
    last_error = watcher_value(exe, machined, target, "reviver/baseappmgr/last_error")
    status = watcher_value(exe, machined, target, "reviver/baseappmgr/status")
    pid_ok = require_active_pid is None or active_pid == require_active_pid
    healthy = active and pid_ok and heartbeat_acks > 0 and not last_error
    detail = (
        f"active={active} active_pid={active_pid} active_generation={active_generation}"
        f" launch_count={launch_count} heartbeat_acks={heartbeat_acks}"
        f" heartbeat_last_ack_age_ms={age_ms} last_error={last_error} status={status}"
    )
    return ReviverHealth(active, active_pid, active_generation, launch_count, heartbeat_acks,
                          age_ms, last_error, status, healthy, detail)


def wait_for_reviver_target(args: argparse.Namespace, exe: Path,
                            reviver_target: str, manager_pid: int) -> ReviverHealth:
    def check():
        try:
            h = read_reviver_health(exe, args.machined, reviver_target,
                                    require_active_pid=manager_pid)
            return h.healthy, h
        except RuntimeError as ex:
            return False, str(ex)
    return wait_until(args.timeout_sec, args.poll_sec, check)


def wait_for_snapshot_health(args: argparse.Namespace, exe: Path,
                             mgr_target: str, baseline_saves: int) -> SnapshotHealth:
    def check():
        try:
            h = read_snapshot_health(exe, args.machined, mgr_target)
            return h.healthy and h.saves > baseline_saves, h
        except RuntimeError as ex:
            return False, str(ex)
    return wait_until(args.timeout_sec, args.poll_sec, check)


def wait_for_reattach(args: argparse.Namespace, exe: Path, mgr_target: str,
                      require_restored: bool) -> ReattachHealth:
    def check():
        try:
            h = read_reattach_health(exe, args.machined, mgr_target, require_restored,
                                     args.min_baseapps)
            return h.healthy, h
        except RuntimeError as ex:
            return False, str(ex)
    return wait_until(args.timeout_sec, args.poll_sec, check)


def select_baseappmgr(exe: Path, args: argparse.Namespace) -> dict[str, str]:
    return choose_process(list_processes(exe, args.machined, "baseappmgr"),
                          args.baseappmgr_name, "baseappmgr")


def select_reviver(exe: Path, args: argparse.Namespace) -> dict[str, str]:
    revivers = list_processes(exe, args.machined, "reviver")
    if not revivers:
        raise RuntimeError("no Reviver processes registered")
    if args.reviver_name:
        for r in revivers:
            if r["name"] == args.reviver_name:
                return r
        raise RuntimeError(f"requested Reviver {args.reviver_name} not registered")
    # Prefer the one with baseappmgr leader lock; otherwise first.
    for r in revivers:
        try:
            if watcher_value(exe, args.machined, f"reviver:{r['name']}",
                             "reviver/baseappmgr/leader/active") == "true":
                return r
        except RuntimeError:
            continue
    return revivers[0]


def precheck_topology(exe: Path, args: argparse.Namespace) -> tuple[dict[str, str], dict[str, str]]:
    revivers = list_processes(exe, args.machined, "reviver")
    if len(revivers) < args.min_revivers:
        raise RuntimeError(
            f"registered_revivers below min_revivers: {len(revivers)}<{args.min_revivers}")
    baseapps = list_processes(exe, args.machined, "baseapp")
    if len(baseapps) < args.min_baseapps:
        raise RuntimeError(
            f"registered_baseapps below min_baseapps: {len(baseapps)}<{args.min_baseapps}")
    mgr = select_baseappmgr(exe, args)
    reviver = select_reviver(exe, args)
    return mgr, reviver


def run_no_inject(args: argparse.Namespace, exe: Path,
                  summary: dict) -> tuple[bool, list[str]]:
    mgr, reviver = precheck_topology(exe, args)
    mgr_target = f"baseappmgr:{mgr['name']}"
    reviver_target = f"reviver:{reviver['name']}"

    snapshot = read_snapshot_health(exe, args.machined, mgr_target)
    reattach = read_reattach_health(exe, args.machined, mgr_target,
                                     require_restored=False,
                                     min_baseapps=args.min_baseapps)
    reviver_health = read_reviver_health(exe, args.machined, reviver_target)

    summary["current"] = {
        "manager_pid": mgr["pid"],
        "reviver_name": reviver["name"],
        "snapshot": _snapshot_to_dict(snapshot),
        "reattach": _reattach_to_dict(reattach),
        "reviver": _reviver_to_dict(reviver_health),
    }

    failure_stages: list[str] = []
    if not args.allow_empty_snapshot and snapshot.saves == 0:
        failure_stages.append("snapshot_saves_zero")
    if not snapshot.healthy:
        failure_stages.append("snapshot_unhealthy")
    if not reattach.healthy:
        failure_stages.append("reattach_unhealthy")
    if not reviver_health.healthy:
        failure_stages.append("reviver_unhealthy")

    if not failure_stages:
        # post-check stability window
        time.sleep(args.stability_sec)
        post_snapshot = read_snapshot_health(exe, args.machined, mgr_target)
        post_reviver = read_reviver_health(exe, args.machined, reviver_target)
        if post_snapshot.save_failures > snapshot.save_failures:
            failure_stages.append("stability_snapshot_save_failures")
        if post_snapshot.restore_failures > snapshot.restore_failures:
            failure_stages.append("stability_snapshot_restore_failures")
        if post_reviver.launch_count > reviver_health.launch_count:
            failure_stages.append("stability_reviver_relaunch")
        summary["current"]["stability_healthy"] = len(failure_stages) == 0
    else:
        summary["current"]["stability_healthy"] = False

    return len(failure_stages) == 0, failure_stages


def run_cycle(args: argparse.Namespace, exe: Path, cycle: int,
              summary: dict) -> CycleResult:
    mgr, reviver = precheck_topology(exe, args)
    old_pid = int(mgr["pid"])
    mgr_target = f"baseappmgr:{mgr['name']}"
    reviver_target = f"reviver:{reviver['name']}"

    baseline_snapshot = read_snapshot_health(exe, args.machined, mgr_target)
    baseline_reviver = read_reviver_health(exe, args.machined, reviver_target)
    start = time.monotonic()
    failure_stages: list[str] = []

    try:
        shutdown_process(exe, args.machined, mgr_target, args.shutdown_reason)
    except RuntimeError as ex:
        raise RuntimeError(f"shutdown failed: {ex}") from ex

    # Wait for Reviver to retarget + new manager to register
    def manager_restarted():
        try:
            registered = list_processes(exe, args.machined, "baseappmgr")
            for entry in registered:
                if entry["name"] != mgr["name"]:
                    continue
                new_pid = int(entry["pid"])
                if new_pid != old_pid:
                    return True, entry
        except RuntimeError as ex:
            return False, str(ex)
        return False, "manager not restarted yet"
    try:
        new_mgr = wait_until(args.timeout_sec, args.poll_sec, manager_restarted)
    except RuntimeError as ex:
        failure_stages.append("manager_restart")
        raise

    new_pid = int(new_mgr["pid"])

    # Wait for Reviver to recognise the new manager and finish heartbeat handshake.
    try:
        reviver_health = wait_for_reviver_target(args, exe, reviver_target, new_pid)
    except RuntimeError as ex:
        failure_stages.append("reviver_retarget")
        raise RuntimeError(f"Reviver did not retarget new BaseAppMgr: {ex}") from ex

    # Wait for new manager to restore and write a fresh snapshot.
    try:
        snapshot = wait_for_snapshot_health(args, exe, mgr_target,
                                            baseline_saves=0)
    except RuntimeError as ex:
        failure_stages.append("snapshot_restore")
        raise RuntimeError(f"new BaseAppMgr did not write fresh snapshot: {ex}") from ex

    takeover_elapsed_ms = int((time.monotonic() - start) * 1000)

    # Wait for surviving BaseApps to reattach.
    try:
        reattach = wait_for_reattach(args, exe, mgr_target,
                                      require_restored=not args.allow_empty_snapshot)
    except RuntimeError as ex:
        failure_stages.append("reattach")
        raise RuntimeError(f"BaseApp reattach did not converge: {ex}") from ex

    # Stability window — ensure no second restart in this period.
    time.sleep(args.stability_sec)
    post_snapshot = read_snapshot_health(exe, args.machined, mgr_target)
    post_reviver = read_reviver_health(exe, args.machined, reviver_target,
                                       require_active_pid=new_pid)
    if post_snapshot.save_failures > snapshot.save_failures:
        failure_stages.append("stability_snapshot_save_failures")
    if post_reviver.launch_count > reviver_health.launch_count:
        failure_stages.append("stability_reviver_relaunch")
    if not post_reviver.healthy:
        failure_stages.append("stability_reviver_unhealthy")

    healthy = len(failure_stages) == 0
    return CycleResult(
        cycle=cycle,
        old_pid=old_pid,
        new_pid=new_pid,
        takeover_elapsed_ms=takeover_elapsed_ms,
        snapshot=post_snapshot,
        reattach=reattach,
        reviver=post_reviver,
        healthy=healthy,
        failure_stages=failure_stages,
    )


def _snapshot_to_dict(s: SnapshotHealth) -> dict:
    return {
        "saves": s.saves,
        "restores": s.restores,
        "fallback_restores": s.fallback_restores,
        "save_failures": s.save_failures,
        "restore_failures": s.restore_failures,
        "failures": s.failures,
        "backup_skips": s.backup_skips,
        "dirty": s.dirty,
        "save_stale": s.save_stale,
        "healthy": s.healthy,
        "status": s.status,
    }


def _reattach_to_dict(r: ReattachHealth) -> dict:
    return {
        "restored": r.restored,
        "pending": r.pending,
        "completed_count": r.completed_count,
        "stuck": r.stuck,
        "state": r.state,
        "healthy": r.healthy,
        "status": r.status,
    }


def _reviver_to_dict(r: ReviverHealth) -> dict:
    return {
        "active": r.active,
        "active_pid": r.active_pid,
        "active_generation": r.active_generation,
        "launch_count": r.launch_count,
        "heartbeat_acks": r.heartbeat_acks,
        "heartbeat_last_ack_age_ms": r.heartbeat_last_ack_age_ms,
        "last_error": r.last_error,
        "status": r.status,
        "healthy": r.healthy,
    }


def _cycle_to_dict(c: CycleResult) -> dict:
    return {
        "cycle": c.cycle,
        "old_pid": c.old_pid,
        "new_pid": c.new_pid,
        "takeover_elapsed_ms": c.takeover_elapsed_ms,
        "healthy": c.healthy,
        "failure_stages": c.failure_stages,
        "snapshot": _snapshot_to_dict(c.snapshot) if c.snapshot else None,
        "reattach": _reattach_to_dict(c.reattach) if c.reattach else None,
        "reviver": _reviver_to_dict(c.reviver) if c.reviver else None,
    }


def build_summary(args: argparse.Namespace, cycles: list[CycleResult],
                  current_healthy: bool, failure_stages: list[str],
                  gate_failures: list[dict]) -> dict:
    successful = sum(1 for c in cycles if c.healthy)
    failed = len(cycles) - successful
    return {
        "schema_version": SCHEMA_VERSION,
        "mode": "no-inject" if args.no_inject else "inject",
        "parameters": {
            "build": args.build,
            "machined": args.machined,
            "baseappmgr_name": args.baseappmgr_name,
            "reviver_name": args.reviver_name,
            "cycles": args.cycles,
            "min_revivers": args.min_revivers,
            "min_baseapps": args.min_baseapps,
            "timeout_sec": args.timeout_sec,
            "poll_sec": args.poll_sec,
            "stability_sec": args.stability_sec,
            "max_takeover_ms": args.max_takeover_ms,
            "shutdown_reason": args.shutdown_reason,
            "allow_empty_snapshot": args.allow_empty_snapshot,
            "no_inject": args.no_inject,
        },
        "summary": {
            "cycles": len(cycles),
            "successful_cycles": successful,
            "failed_cycles": failed,
            "current_failure_stages": failure_stages,
            "current_healthy": current_healthy,
            "gate_failures": gate_failures,
            "overall_healthy": current_healthy and failed == 0 and not gate_failures,
        },
        "cycles": [_cycle_to_dict(c) for c in cycles],
        "gates": gate_failures,
    }


def write_summary(path: Path, summary: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(summary, indent=2))


def main() -> int:
    args = parse_args()
    exe = args.atlas_tool or default_atlas_tool(args.build)
    if not exe.is_file():
        print(f"[verify_baseappmgr_ha] atlas_tool not found: {exe}", file=sys.stderr)
        return 1

    cycles: list[CycleResult] = []
    failure_stages: list[str] = []
    gate_failures: list[dict] = []
    current_healthy = False
    summary_scratch: dict = {}

    try:
        if args.no_inject:
            current_healthy, failure_stages = run_no_inject(args, exe, summary_scratch)
        else:
            for i in range(1, max(1, args.cycles) + 1):
                cr = run_cycle(args, exe, i, summary_scratch)
                cycles.append(cr)
                if args.max_takeover_ms > 0 and cr.takeover_elapsed_ms > args.max_takeover_ms:
                    gate_failures.append({
                        "name": "max_takeover_ms",
                        "metric": "max_takeover_ms",
                        "maximum": args.max_takeover_ms,
                        "value": cr.takeover_elapsed_ms,
                        "ok": False,
                    })
            current_healthy = all(c.healthy for c in cycles)
            failure_stages = [s for c in cycles for s in c.failure_stages]
    except RuntimeError as ex:
        print(f"[verify_baseappmgr_ha] FAIL {ex}", file=sys.stderr)
        if args.summary_json:
            summary = build_summary(args, cycles, current_healthy,
                                    failure_stages + [str(ex)], gate_failures)
            summary["current"] = summary_scratch.get("current", {})
            write_summary(args.summary_json, summary)
        return 1

    summary = build_summary(args, cycles, current_healthy, failure_stages, gate_failures)
    summary["current"] = summary_scratch.get("current", {})
    if args.summary_json:
        write_summary(args.summary_json, summary)

    if not summary["summary"]["overall_healthy"]:
        print(
            f"[verify_baseappmgr_ha] FAIL cycles={len(cycles)}"
            f" successful={summary['summary']['successful_cycles']}"
            f" failure_stages={failure_stages}"
            f" gate_failures={gate_failures}",
            file=sys.stderr,
        )
        return 1

    print(
        f"[verify_baseappmgr_ha] PASS cycles={len(cycles)}"
        f" successful={summary['summary']['successful_cycles']}"
        f" mode={summary['mode']}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
