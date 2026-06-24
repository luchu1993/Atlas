#!/usr/bin/env python3
"""Validate DBAppMgr HA restart on a live Atlas cluster.

The script drives DBAppMgr abnormal shutdown through machined, waits for a
Reviver-supervised replacement, and verifies DBApp worker re-registration rebuilt
the shard table without a manager snapshot.
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
from pathlib import Path
from typing import Callable

REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA_VERSION = 1

PROC_RE = re.compile(
    r"^(?P<type>\w+)\s+(?P<name>\S+)\s+(?P<addr>\S+)\s+(?P<pid>\d+)\s+(?P<load>[\d.]+)%"
)


def default_atlas_tool(build_subdir: str) -> Path:
    exe = "atlas_tool.exe" if platform.system() == "Windows" else "atlas_tool"
    return REPO_ROOT / "bin" / build_subdir / exe


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Validate DBAppMgr HA restart on a live Atlas cluster.")
    p.add_argument("--build", default="debug", help="bin/<build>/atlas_tool directory")
    p.add_argument("--machined", default="127.0.0.1:20018")
    p.add_argument("--atlas-tool", type=Path, default=None)
    p.add_argument("--dbappmgr-name", default="dbappmgr")
    p.add_argument("--reviver-name", default="", help="target Reviver name; default active monitor")
    p.add_argument("--min-revivers", type=int, default=1)
    p.add_argument("--min-dbapps", type=int, default=1)
    p.add_argument("--timeout-sec", type=float, default=60.0)
    p.add_argument("--poll-sec", type=float, default=1.0)
    p.add_argument("--stability-sec", type=float, default=5.0)
    p.add_argument("--cycles", type=int, default=1)
    p.add_argument("--max-takeover-ms", type=int, default=0)
    p.add_argument("--shutdown-reason", type=int, default=1)
    p.add_argument("--allow-empty-cluster", action="store_true")
    p.add_argument("--no-inject", action="store_true")
    p.add_argument("--verify-reviver-failover", action="store_true")
    p.add_argument("--max-reviver-failover-ms", type=int, default=0)
    p.add_argument("--check-active-reviver", action="store_true")
    p.add_argument("--summary-json", type=Path, default=None)
    return p.parse_args()


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


def choose_process(processes: list[dict[str, str]], name: str, process_type: str) -> dict[str, str]:
    matches = [proc for proc in processes if not name or proc["name"] == name]
    if not matches:
        wanted = name or process_type
        raise RuntimeError(f"no registered {process_type} matches {wanted}")
    if len(matches) > 1 and not name:
        raise RuntimeError(f"multiple {process_type} processes registered; pass --{process_type}-name")
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


def target(process_type: str, name: str) -> str:
    return f"{process_type}:{name}"


def select_dbappmgr(exe: Path, args: argparse.Namespace) -> dict[str, str]:
    return choose_process(list_processes(exe, args.machined, "dbappmgr"), args.dbappmgr_name, "dbappmgr")


def select_reviver(exe: Path, args: argparse.Namespace) -> dict[str, str]:
    revivers = list_processes(exe, args.machined, "reviver")
    if not revivers:
        raise RuntimeError("no Reviver processes registered")
    if args.reviver_name:
        return choose_process(revivers, args.reviver_name, "reviver")
    for reviver in revivers:
        try:
            if (
                watcher_value(
                    exe,
                    args.machined,
                    target("reviver", reviver["name"]),
                    "reviver/dbappmgr/active_reviver",
                )
                == "true"
            ):
                return reviver
        except RuntimeError:
            continue
    return revivers[0]


def read_manager_health(
    exe: Path, machined: str, mgr_target: str, expected_dbapps: int
) -> OrderedDict[str, object]:
    dbapp_count = int_watcher(exe, machined, mgr_target, "dbappmgr/dbapp_count")
    shard_count = int_watcher(exe, machined, mgr_target, "dbappmgr/shards/count")
    version = int_watcher(exe, machined, mgr_target, "dbappmgr/shards/version")
    table = watcher_value(exe, machined, mgr_target, "dbappmgr/shards/table")
    recovery_window = watcher_value(exe, machined, mgr_target, "dbappmgr/ha/recovery_window_active")
    has_expected_workers = dbapp_count >= expected_dbapps
    has_shards = expected_dbapps == 0 or (shard_count > 0 and version > 0 and bool(table))
    healthy = has_expected_workers and has_shards
    return OrderedDict(
        dbapp_count=dbapp_count,
        expected_dbapps=expected_dbapps,
        shard_count=shard_count,
        shard_table_version=version,
        shard_table=table,
        recovery_window_active=recovery_window,
        healthy=healthy,
        detail=(
            f"dbapp_count={dbapp_count} expected>={expected_dbapps} shards={shard_count}"
            f" version={version} recovery_window_active={recovery_window}"
        ),
    )


def read_reviver_health(
    exe: Path, machined: str, reviver_target: str, required_active_pid: int | None = None
) -> OrderedDict[str, object]:
    active = watcher_value(exe, machined, reviver_target, "reviver/dbappmgr/active") == "true"
    active_pid = int_watcher(exe, machined, reviver_target, "reviver/dbappmgr/active_pid")
    active_generation = int_watcher(
        exe, machined, reviver_target, "reviver/dbappmgr/active_generation"
    )
    launch_count = int_watcher(exe, machined, reviver_target, "reviver/dbappmgr/launch_count")
    heartbeat_acks = int_watcher(exe, machined, reviver_target, "reviver/dbappmgr/heartbeat_acks")
    heartbeat_age = int_watcher(
        exe, machined, reviver_target, "reviver/dbappmgr/heartbeat_last_ack_age_ms"
    )
    priority = int_watcher(exe, machined, reviver_target, "reviver/dbappmgr/priority")
    active_reviver = (
        watcher_value(exe, machined, reviver_target, "reviver/dbappmgr/active_reviver") == "true"
    )
    last_error = watcher_value(exe, machined, reviver_target, "reviver/dbappmgr/last_error")
    status = watcher_value(exe, machined, reviver_target, "reviver/dbappmgr/status")
    pid_ok = required_active_pid is None or active_pid == required_active_pid
    healthy = active and pid_ok and heartbeat_acks > 0 and not last_error
    return OrderedDict(
        active=active,
        active_pid=active_pid,
        active_generation=active_generation,
        launch_count=launch_count,
        heartbeat_acks=heartbeat_acks,
        heartbeat_last_ack_age_ms=heartbeat_age,
        priority=priority,
        active_reviver=active_reviver,
        last_error=last_error,
        status=status,
        healthy=healthy,
        detail=(
            f"active={active} active_pid={active_pid} active_generation={active_generation}"
            f" launch_count={launch_count} heartbeat_acks={heartbeat_acks}"
            f" heartbeat_last_ack_age_ms={heartbeat_age} priority={priority}"
            f" active_reviver={active_reviver} last_error={last_error} status={status}"
        ),
    )


def list_revivers_with_arbitration(exe: Path, machined: str) -> list[dict[str, str]]:
    revivers = list_processes(exe, machined, "reviver")
    enriched: list[dict[str, str]] = []
    for reviver in revivers:
        reviver_target = target("reviver", reviver["name"])
        try:
            reviver["dbappmgr_active_reviver"] = watcher_value(
                exe, machined, reviver_target, "reviver/dbappmgr/active_reviver"
            )
            reviver["dbappmgr_priority"] = watcher_value(
                exe, machined, reviver_target, "reviver/dbappmgr/priority"
            )
            reviver["dbappmgr_active_pid"] = watcher_value(
                exe, machined, reviver_target, "reviver/dbappmgr/active_pid"
            )
        except RuntimeError:
            reviver["dbappmgr_active_reviver"] = ""
            reviver["dbappmgr_priority"] = "0"
            reviver["dbappmgr_active_pid"] = "0"
        enriched.append(reviver)
    return enriched


def precheck_topology(exe: Path, args: argparse.Namespace) -> tuple[dict[str, str], dict[str, str], int]:
    revivers = list_processes(exe, args.machined, "reviver")
    if len(revivers) < args.min_revivers:
        raise RuntimeError(f"registered_revivers below min: {len(revivers)}<{args.min_revivers}")
    dbapps = list_processes(exe, args.machined, "dbapp")
    if not args.allow_empty_cluster and len(dbapps) < args.min_dbapps:
        raise RuntimeError(f"registered_dbapps below min: {len(dbapps)}<{args.min_dbapps}")
    mgr = select_dbappmgr(exe, args)
    reviver = select_reviver(exe, args)
    expected = 0 if args.allow_empty_cluster else max(args.min_dbapps, len(dbapps))
    return mgr, reviver, expected


def wait_for_new_dbappmgr(
    args: argparse.Namespace, exe: Path, old_pid: int
) -> dict[str, str]:
    def check():
        try:
            mgr = select_dbappmgr(exe, args)
        except RuntimeError as ex:
            return False, str(ex)
        if int(mgr["pid"]) != old_pid:
            return True, mgr
        return False, f"dbappmgr pid still {old_pid}"

    return wait_until(args.timeout_sec, args.poll_sec, check)


def wait_for_manager_health(
    args: argparse.Namespace, exe: Path, mgr_target: str, expected_dbapps: int
) -> OrderedDict[str, object]:
    def check():
        try:
            health = read_manager_health(exe, args.machined, mgr_target, expected_dbapps)
            return bool(health["healthy"]), health
        except RuntimeError as ex:
            return False, str(ex)

    return wait_until(args.timeout_sec, args.poll_sec, check)


def wait_for_reviver_health(
    args: argparse.Namespace, exe: Path, reviver_target: str, active_pid: int
) -> OrderedDict[str, object]:
    def check():
        try:
            health = read_reviver_health(exe, args.machined, reviver_target, active_pid)
            return bool(health["healthy"]), health
        except RuntimeError as ex:
            return False, str(ex)

    return wait_until(args.timeout_sec, args.poll_sec, check)


def run_no_inject(args: argparse.Namespace, exe: Path) -> tuple[bool, OrderedDict[str, object]]:
    mgr, reviver, expected = precheck_topology(exe, args)
    mgr_target = target("dbappmgr", mgr["name"])
    reviver_target = target("reviver", reviver["name"])
    manager = read_manager_health(exe, args.machined, mgr_target, expected)
    reviver_health = read_reviver_health(exe, args.machined, reviver_target, int(mgr["pid"]))
    failures: list[str] = []
    if not manager["healthy"]:
        failures.append("manager_unhealthy")
    if not reviver_health["healthy"]:
        failures.append("reviver_unhealthy")
    if args.check_active_reviver and not reviver_health["active_reviver"]:
        failures.append("not_active_monitor")
    return not failures, OrderedDict(
        mode="no_inject",
        manager_pid=int(mgr["pid"]),
        reviver_name=reviver["name"],
        manager=manager,
        reviver=reviver_health,
        failures=failures,
    )


def run_cycle(args: argparse.Namespace, exe: Path, cycle: int) -> OrderedDict[str, object]:
    mgr, reviver, expected = precheck_topology(exe, args)
    old_pid = int(mgr["pid"])
    reviver_target = target("reviver", reviver["name"])
    before_reviver = read_reviver_health(exe, args.machined, reviver_target, old_pid)

    start = time.monotonic()
    shutdown_process(exe, args.machined, target("dbappmgr", mgr["name"]), args.shutdown_reason)
    new_mgr = wait_for_new_dbappmgr(args, exe, old_pid)
    new_pid = int(new_mgr["pid"])
    manager = wait_for_manager_health(args, exe, target("dbappmgr", new_mgr["name"]), expected)
    reviver_health = wait_for_reviver_health(args, exe, reviver_target, new_pid)
    elapsed_ms = int((time.monotonic() - start) * 1000)

    failures: list[str] = []
    if args.max_takeover_ms and elapsed_ms > args.max_takeover_ms:
        failures.append("takeover_slo_exceeded")
    if args.check_active_reviver and not reviver_health["active_reviver"]:
        failures.append("not_active_monitor")

    time.sleep(args.stability_sec)
    stable_mgr = select_dbappmgr(exe, args)
    stable_reviver = read_reviver_health(exe, args.machined, reviver_target, new_pid)
    if int(stable_mgr["pid"]) != new_pid:
        failures.append("stability_manager_restarted")
    if int(stable_reviver["launch_count"]) > int(reviver_health["launch_count"]):
        failures.append("stability_reviver_relaunch")
    if not stable_reviver["healthy"]:
        failures.append("stability_reviver_unhealthy")

    return OrderedDict(
        cycle=cycle,
        old_pid=old_pid,
        new_pid=new_pid,
        takeover_elapsed_ms=elapsed_ms,
        expected_dbapps=expected,
        manager=manager,
        reviver_before=before_reviver,
        reviver=reviver_health,
        stability_reviver=stable_reviver,
        failures=failures,
        healthy=not failures,
    )


def run_reviver_failover(
    args: argparse.Namespace, exe: Path, manager_pid: int
) -> OrderedDict[str, object]:
    revivers = list_revivers_with_arbitration(exe, args.machined)
    if len(revivers) < 2:
        raise RuntimeError(f"reviver failover requires >=2 Revivers; found {len(revivers)}")
    monitors = [r for r in revivers if r["dbappmgr_active_reviver"] == "true"]
    if len(monitors) != 1:
        raise RuntimeError(f"expected one active DBAppMgr monitor, found {[r['name'] for r in monitors]}")
    monitor = monitors[0]
    start = time.monotonic()
    shutdown_process(exe, args.machined, target("reviver", monitor["name"]), args.shutdown_reason)

    def check():
        try:
            current = list_revivers_with_arbitration(exe, args.machined)
        except RuntimeError as ex:
            return False, str(ex)
        new_monitors = [
            r
            for r in current
            if r["dbappmgr_active_reviver"] == "true" and r["name"] != monitor["name"]
        ]
        if len(new_monitors) == 1:
            return True, new_monitors[0]
        return False, f"waiting for standby monitor; current={[r['name'] for r in new_monitors]}"

    new_monitor = wait_until(args.timeout_sec, args.poll_sec, check)
    elapsed_ms = int((time.monotonic() - start) * 1000)
    mgr = select_dbappmgr(exe, args)
    manager_pid_after = int(mgr["pid"])
    failures: list[str] = []
    if manager_pid_after != manager_pid:
        failures.append("manager_restarted_during_reviver_failover")
    if args.max_reviver_failover_ms and elapsed_ms > args.max_reviver_failover_ms:
        failures.append("reviver_failover_slo_exceeded")
    return OrderedDict(
        old_monitor=monitor["name"],
        new_monitor=new_monitor["name"],
        old_pid=int(monitor["pid"]),
        new_pid=int(new_monitor["pid"]),
        new_priority=int(new_monitor["dbappmgr_priority"] or 0),
        manager_pid=manager_pid,
        manager_pid_after=manager_pid_after,
        elapsed_ms=elapsed_ms,
        failures=failures,
        healthy=not failures,
    )


def write_summary(path: Path, summary: OrderedDict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(summary, indent=2), encoding="utf-8")


def main() -> int:
    args = parse_args()
    exe = args.atlas_tool or default_atlas_tool(args.build)
    summary: OrderedDict[str, object] = OrderedDict(
        schema_version=SCHEMA_VERSION,
        tool="verify_dbappmgr_ha",
        atlas_tool=str(exe),
        machined=args.machined,
        ok=False,
        cycles=[],
    )
    failures: list[str] = []

    try:
        if not exe.exists():
            raise RuntimeError(f"atlas_tool not found: {exe}")
        if args.no_inject:
            ok, current = run_no_inject(args, exe)
            summary["current"] = current
            if not ok:
                failures.extend(current["failures"])
        else:
            for cycle in range(1, args.cycles + 1):
                result = run_cycle(args, exe, cycle)
                summary["cycles"].append(result)
                if not result["healthy"]:
                    failures.extend(f"cycle_{cycle}:{stage}" for stage in result["failures"])
        if args.verify_reviver_failover:
            mgr = select_dbappmgr(exe, args)
            failover = run_reviver_failover(args, exe, int(mgr["pid"]))
            summary["reviver_failover"] = failover
            if not failover["healthy"]:
                failures.extend(f"reviver_failover:{stage}" for stage in failover["failures"])
    except Exception as exc:
        failures.append(str(exc))

    summary["ok"] = not failures
    summary["failures"] = failures
    if args.summary_json is not None:
        write_summary(args.summary_json, summary)

    if failures:
        print("FAIL verify_dbappmgr_ha")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("PASS verify_dbappmgr_ha")
    return 0


if __name__ == "__main__":
    sys.exit(main())
