#!/usr/bin/env python3
"""Validate CellAppMgr HA restart on a live Atlas cluster."""

from __future__ import annotations

import argparse
import platform
import re
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import NamedTuple

TOOLS_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS_ROOT))
from common.json_io import write_json_atomic  # noqa: E402
from common.stats import latency_summary_ms  # noqa: E402

REPO_ROOT = TOOLS_ROOT.parent
PROC_RE = re.compile(
    r"^(?P<type>\S+)\s+(?P<name>\S+)\s+(?P<addr>\S+)\s+(?P<pid>\d+)\s+(?P<load>\S+)%$"
)
SPACE_RE = re.compile(
    r"\bspace=(?P<space>\d+) "
    r"version=(?P<version>\d+) "
    r"freeze_epoch=(?P<freeze>\d+) "
    r"leaves=(?P<leaves>\d+) "
    r"primary=(?P<primary>\d+) "
    r"pending_ack=(?P<pending>\d+)"
)
CELL_RE = re.compile(
    r"\bcell=(?P<cell>\d+) "
    r"app=(?P<app>\d+).*?"
    r"bounds=\((?P<bounds>[^)]*)\)"
)
SUMMARY_FIELD_RE = re.compile(
    r"(?<!\S)(?P<key>[A-Za-z_][A-Za-z0-9_]*)=(?P<value>[^\s|]+)"
)
CELLAPP_LOAD_SUMMARY_RE = re.compile(
    r"\bapp=(?P<app>\d+)\s+addr=(?P<addr>\S+)\s+load=(?P<load>\S+)\s+"
    r"entities=(?P<entities>\d+)\s+retiring=(?P<retiring>[01])\s+"
    r"load_age_ms=(?P<load_age_ms>-?\d+)\s+load_stale=(?P<load_stale>[01])"
)
SUMMARY_MAX_GATES = {
    "max_takeover_ms": "max_takeover_ms",
    "max_reviver_failover_ms": "max_reviver_failover_ms",
    "max_load_report_age_ms": "max_load_report_age_ms",
}
SUMMARY_MIN_GATES = {
    "min_revivers": "registered_revivers",
    "min_post_failover_standbys": "min_post_failover_standbys",
}


class HaCycleResult(NamedTuple):
    cycle: int
    old_pid: str
    new_pid: str
    generation_before: int
    generation_after: int
    launch_count_before: int
    launch_count_after: int
    restore_status: str
    pre_topology_status: str
    pre_snapshot_status: str
    restored_topology_status: str
    post_restore_snapshot_status: str
    output_status: str
    stability_status: str
    manager_restart_ms: int
    reviver_retarget_ms: int
    restore_converged_ms: int
    takeover_elapsed_ms: int
    recovery_status: str = ""
    recovery: dict[str, object] | None = None
    load_report_status: str = ""
    load_report: dict[str, object] | None = None
    stability_healthy: bool = True
    takeover_healthy: bool = True
    failure_stage: str = ""


class ReviverLeadership(NamedTuple):
    leader: dict[str, str]
    active_count: int
    standby_count: int
    status: str


class StandbyReviverHealthReport(NamedTuple):
    ok: bool
    status: str
    records: list[dict[str, object]]


class LoadReportHealthReport(NamedTuple):
    ok: bool
    status: str
    payload: dict[str, object]


class RecoveryHealthReport(NamedTuple):
    ok: bool
    status: str
    stuck: bool
    payload: dict[str, object]


class StabilityHealthReport(NamedTuple):
    ok: bool
    status: str


class ReviverFailoverResult(NamedTuple):
    cycle: int
    old_leader: str
    new_leader: str
    old_pid: str
    new_pid: str
    manager_pid: str
    surviving_revivers: int
    standby_after: int
    generation_before: int
    generation_after: int
    launch_count_before: int
    launch_count_after: int
    heartbeat_acks_before: int
    heartbeat_acks_after: int
    failover_elapsed_ms: int
    leadership_status: str
    standby_health_status: str
    standby_health: list[dict[str, object]]
    status: str
    healthy: bool = True


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
    parser.add_argument("--cellappmgr-name", default="cellappmgr", help="target CellAppMgr name")
    parser.add_argument("--reviver-name", default="", help="target Reviver name")
    parser.add_argument("--min-revivers", type=int, default=1, help="minimum registered Revivers")
    parser.add_argument("--min-cellapps", type=int, default=1, help="minimum registered CellApps")
    parser.add_argument("--timeout-sec", type=float, default=60.0, help="wait timeout")
    parser.add_argument("--poll-sec", type=float, default=1.0, help="watcher poll interval")
    parser.add_argument(
        "--stability-sec",
        type=float,
        default=5.0,
        help="post-restore window that must not show another Reviver restart",
    )
    parser.add_argument(
        "--cycles",
        type=int,
        default=1,
        help="number of abnormal CellAppMgr restart cycles to inject",
    )
    parser.add_argument(
        "--max-takeover-ms",
        type=int,
        default=0,
        help="maximum allowed shutdown-to-fresh-snapshot takeover time; 0 disables the check",
    )
    parser.add_argument(
        "--max-reviver-failover-ms",
        type=int,
        default=0,
        help="maximum allowed active Reviver shutdown-to-standby takeover time; 0 disables",
    )
    parser.add_argument(
        "--max-load-report-age-ms",
        type=int,
        default=0,
        help="maximum allowed CellApp load report age after reattach; 0 disables",
    )
    parser.add_argument(
        "--min-post-failover-standbys",
        type=int,
        default=0,
        help="minimum standby Revivers required after active Reviver leader failover",
    )
    parser.add_argument("--shutdown-reason", type=int, default=1, help="machined shutdown reason")
    parser.add_argument(
        "--allow-empty-snapshot",
        action="store_true",
        help="do not require cellappmgr/ha/snapshot_path or snapshot_saves > 0",
    )
    parser.add_argument(
        "--allow-missing-backup-snapshot",
        action="store_true",
        help="do not require cellappmgr/ha/snapshot_backup_status state=ready",
    )
    parser.add_argument(
        "--allow-empty-output-log",
        action="store_true",
        help="do not require reviver/cellappmgr/output_path or local revived process log content",
    )
    parser.add_argument(
        "--allow-topology-change",
        action="store_true",
        help="do not require cellappmgr/lb/spaces topology to match after restore",
    )
    parser.add_argument(
        "--no-inject",
        action="store_true",
        help="only verify current watchers without shutting down CellAppMgr",
    )
    parser.add_argument(
        "--verify-reviver-failover",
        action="store_true",
        help="kill the active Reviver leader and require standby takeover",
    )
    parser.add_argument(
        "--reviver-failover-cycles",
        type=int,
        default=1,
        help="number of active Reviver leader failover cycles to inject",
    )
    parser.add_argument(
        "--check-leader-lock-mode",
        default="",
        help="if non-empty, require reviver/leader/mode to equal this value (e.g. 'machined')"
             " and (for 'machined') leader_active=true at check time",
    )
    parser.add_argument(
        "--summary-json",
        type=Path,
        help="write a machine-readable summary JSON after checks complete",
    )
    args = parser.parse_args()
    if args.cycles < 1:
        parser.error("--cycles must be >= 1")
    if args.reviver_failover_cycles < 1:
        parser.error("--reviver-failover-cycles must be >= 1")
    if args.max_takeover_ms < 0:
        parser.error("--max-takeover-ms must be >= 0")
    if args.max_reviver_failover_ms < 0:
        parser.error("--max-reviver-failover-ms must be >= 0")
    if args.max_load_report_age_ms < 0:
        parser.error("--max-load-report-age-ms must be >= 0")
    if args.min_post_failover_standbys < 0:
        parser.error("--min-post-failover-standbys must be >= 0")
    if args.min_revivers < 1:
        parser.error("--min-revivers must be >= 1")
    if args.verify_reviver_failover and args.min_revivers < 2:
        parser.error("--verify-reviver-failover requires --min-revivers >= 2")
    if args.max_reviver_failover_ms and not args.verify_reviver_failover:
        parser.error("--max-reviver-failover-ms requires --verify-reviver-failover")
    if args.min_post_failover_standbys and not args.verify_reviver_failover:
        parser.error("--min-post-failover-standbys requires --verify-reviver-failover")
    if args.reviver_failover_cycles != 1 and not args.verify_reviver_failover:
        parser.error("--reviver-failover-cycles requires --verify-reviver-failover")
    required_revivers = args.min_post_failover_standbys + args.reviver_failover_cycles + 1
    if args.verify_reviver_failover and args.min_revivers < required_revivers:
        parser.error(
            "--min-revivers must be >= "
            "--min-post-failover-standbys + --reviver-failover-cycles + 1 "
            "for Reviver failover"
        )
    if args.no_inject and args.cycles != 1:
        parser.error("--cycles cannot be combined with --no-inject")
    if args.no_inject and args.max_takeover_ms:
        parser.error("--max-takeover-ms cannot be combined with --no-inject")
    return args


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


def choose_process(
    processes: list[dict[str, str]], name: str, process_type: str
) -> dict[str, str]:
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


def shutdown_process(exe: Path, machined: str, target: str, reason: int) -> None:
    run_atlas_tool(exe, machined, "shutdown", target, str(reason))


def elapsed_ms(start: float) -> int:
    return max(0, int((time.monotonic() - start) * 1000))


def int_watcher(exe: Path, machined: str, target: str, path: str) -> int:
    value = watcher_value(exe, machined, target, path)
    try:
        return int(value)
    except ValueError as ex:
        raise RuntimeError(f"watcher {path} returned non-integer value {value!r}") from ex


def summary_fields(summary: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for match in SUMMARY_FIELD_RE.finditer(summary):
        fields.setdefault(match["key"], match["value"])
    return fields


def summary_has(fields: dict[str, str], key: str, value: str) -> bool:
    return fields.get(key) == value


def cycle_load_report_payload(result: HaCycleResult) -> dict[str, object] | None:
    if result.load_report is not None:
        return result.load_report
    if result.recovery is None:
        return None
    payload = result.recovery.get("load_report")
    return payload if isinstance(payload, dict) else None


def count_list_field(payload: dict[str, object], key: str) -> int:
    value = payload.get(key)
    return len(value) if isinstance(value, list) else 0


def numeric_value(value: object) -> int | float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return value


def current_load_report_max_age_ms(current: dict[str, object] | None) -> int | None:
    if current is None:
        return None
    report = current.get("load_report")
    if not isinstance(report, dict):
        return None
    records = report.get("records")
    if not isinstance(records, list):
        return None
    ages: list[int] = []
    for record in records:
        if not isinstance(record, dict):
            continue
        age = numeric_value(record.get("load_age_ms"))
        if age is not None:
            ages.append(int(age))
    return max(ages) if ages else 0


def summary_metric_value(
    summary: dict[str, int | float],
    current: dict[str, object] | None,
    metric: str,
) -> int | float | None:
    if metric == "registered_revivers" and current is not None:
        topology = current.get("reviver_topology")
        if isinstance(topology, dict):
            value = numeric_value(topology.get("registered_revivers"))
            if value is not None:
                return value
    if metric == "max_load_report_age_ms":
        current_value = current_load_report_max_age_ms(current)
        summary_value = numeric_value(summary.get(metric))
        if current_value is not None and summary_value is not None:
            return max(current_value, summary_value)
        if current_value is not None:
            return current_value
        if summary_value is not None:
            return summary_value
    return numeric_value(summary.get(metric))


def summarize_cycle_recovery(results: list[HaCycleResult]) -> dict[str, int]:
    recovery_payloads = [result.recovery for result in results if result.recovery is not None]
    load_reports = [
        payload for result in results if (payload := cycle_load_report_payload(result)) is not None
    ]
    load_ages: list[int] = []
    load_record_count = 0
    stale_apps = 0
    over_age_apps = 0
    for report in load_reports:
        records = report.get("records")
        if isinstance(records, list):
            load_record_count += len(records)
            for record in records:
                if not isinstance(record, dict):
                    continue
                age = record.get("load_age_ms")
                if isinstance(age, (int, float)):
                    load_ages.append(int(age))
        stale_apps += count_list_field(report, "stale_apps")
        over_age_apps += count_list_field(report, "over_age_apps")
    return {
        "recovery_health_checks": len(recovery_payloads),
        "recovery_healthy": sum(
            1 for payload in recovery_payloads if payload.get("healthy") is True
        ),
        "recovery_unhealthy": sum(
            1 for payload in recovery_payloads if payload.get("healthy") is False
        ),
        "load_report_health_checks": len(load_reports),
        "load_report_healthy": sum(
            1 for payload in load_reports if payload.get("healthy") is True
        ),
        "load_report_unhealthy": sum(
            1 for payload in load_reports if payload.get("healthy") is False
        ),
        "load_report_records": load_record_count,
        "max_load_report_age_ms": max(load_ages) if load_ages else 0,
        "load_report_stale_apps": stale_apps,
        "load_report_over_age_apps": over_age_apps,
    }


def ha_cycle_is_successful(result: HaCycleResult) -> bool:
    load_report = cycle_load_report_payload(result)
    return (
        result.takeover_healthy is not False
        and result.stability_healthy is not False
        and (result.recovery is None or result.recovery.get("healthy") is not False)
        and (load_report is None or load_report.get("healthy") is not False)
    )


def summarize_ha_cycles(results: list[HaCycleResult]) -> dict[str, int | float]:
    pid_changes = sum(1 for result in results if result.old_pid != result.new_pid)
    generation_delta = sum(
        result.generation_after - result.generation_before for result in results
    )
    launch_count_delta = sum(
        result.launch_count_after - result.launch_count_before for result in results
    )
    successful_cycles = sum(1 for result in results if ha_cycle_is_successful(result))
    return {
        "cycles": len(results),
        "successful_cycles": successful_cycles,
        "failed_cycles": len(results) - successful_cycles,
        "success_rate": successful_cycles / len(results) if results else 0.0,
        "pid_changes": pid_changes,
        "generation_delta": generation_delta,
        "launch_count_delta": launch_count_delta,
        "takeover_health_checks": len(results),
        "takeover_healthy": sum(
            1 for result in results if result.takeover_healthy is not False
        ),
        "takeover_unhealthy": sum(
            1 for result in results if result.takeover_healthy is False
        ),
        "stability_health_checks": len(results),
        "stability_healthy": sum(
            1 for result in results if result.stability_healthy is not False
        ),
        "stability_unhealthy": sum(
            1 for result in results if result.stability_healthy is False
        ),
        **summarize_cycle_recovery(results),
        **latency_summary_ms([result.takeover_elapsed_ms for result in results], "takeover"),
    }


def summarize_reviver_failovers(
    results: list[ReviverFailoverResult],
) -> dict[str, int | float]:
    standby_counts = [result.standby_after for result in results]
    survivor_counts = [result.surviving_revivers for result in results]
    return {
        "reviver_failovers": len(results),
        "reviver_failover_health_checks": len(results),
        "reviver_failover_healthy": sum(
            1 for result in results if result.healthy is not False
        ),
        "reviver_failover_unhealthy": sum(
            1 for result in results if result.healthy is False
        ),
        "min_surviving_revivers": min(survivor_counts) if survivor_counts else 0,
        "min_post_failover_standbys": min(standby_counts) if standby_counts else 0,
        "max_post_failover_standbys": max(standby_counts) if standby_counts else 0,
        **latency_summary_ms(
            [result.failover_elapsed_ms for result in results], "reviver_failover"
        ),
    }


def summary_gate_evaluations(
    summary: dict[str, int | float],
    parameters: dict[str, object],
    current: dict[str, object] | None = None,
) -> dict[str, dict[str, object]]:
    gates: dict[str, dict[str, object]] = {}
    for parameter, metric in SUMMARY_MAX_GATES.items():
        maximum = numeric_value(parameters.get(parameter))
        if maximum is None or maximum <= 0:
            continue
        value = summary_metric_value(summary, current, metric)
        if value is None:
            continue
        gates[parameter] = {
            "metric": metric,
            "value": value,
            "maximum": maximum,
            "ok": value <= maximum,
        }
    for parameter, metric in SUMMARY_MIN_GATES.items():
        minimum = numeric_value(parameters.get(parameter))
        if minimum is None or minimum <= 0:
            continue
        value = summary_metric_value(summary, current, metric)
        if value is None:
            continue
        gates[parameter] = {
            "metric": metric,
            "value": value,
            "minimum": minimum,
            "ok": value >= minimum,
        }
    return gates


def summary_health_gate_evaluations(
    summary: dict[str, int | float],
) -> dict[str, dict[str, object]]:
    gates: dict[str, dict[str, object]] = {}
    takeover_unhealthy = numeric_value(summary.get("takeover_unhealthy"))
    if takeover_unhealthy is not None and takeover_unhealthy > 0:
        gates["cycle_takeover_health"] = {
            "metric": "takeover_unhealthy",
            "value": takeover_unhealthy,
            "maximum": 0,
            "ok": False,
        }
    stability_unhealthy = numeric_value(summary.get("stability_unhealthy"))
    if stability_unhealthy is not None and stability_unhealthy > 0:
        gates["cycle_stability_health"] = {
            "metric": "stability_unhealthy",
            "value": stability_unhealthy,
            "maximum": 0,
            "ok": False,
        }
    failover_unhealthy = numeric_value(summary.get("reviver_failover_unhealthy"))
    if failover_unhealthy is not None and failover_unhealthy > 0:
        gates["reviver_failover_health"] = {
            "metric": "reviver_failover_unhealthy",
            "value": failover_unhealthy,
            "maximum": 0,
            "ok": False,
        }
    load_report_unhealthy = numeric_value(summary.get("load_report_unhealthy"))
    if load_report_unhealthy is not None and load_report_unhealthy > 0:
        gates["cycle_load_report_health"] = {
            "metric": "load_report_unhealthy",
            "value": load_report_unhealthy,
            "maximum": 0,
            "ok": False,
        }
    recovery_unhealthy = numeric_value(summary.get("recovery_unhealthy"))
    if recovery_unhealthy is not None and recovery_unhealthy > 0:
        gates["cycle_recovery_health"] = {
            "metric": "recovery_unhealthy",
            "value": recovery_unhealthy,
            "maximum": 0,
            "ok": False,
        }
    return gates


def current_health_gate_evaluations(
    current: dict[str, object] | None,
) -> dict[str, dict[str, object]]:
    if current is None:
        return {}
    gates: dict[str, dict[str, object]] = {}
    if current.get("run_healthy") is False:
        gates["run_health"] = {
            "metric": "run.healthy",
            "value": False,
            "expected": True,
            "ok": False,
            "stage": current.get("failure_stage", ""),
            "detail": current.get("failure_detail", ""),
        }
    if current.get("stability_healthy") is False:
        gates["stability_health"] = {
            "metric": "stability.healthy",
            "value": False,
            "expected": True,
            "ok": False,
        }
    recovery = current.get("recovery")
    load_report = current.get("load_report")
    if not isinstance(load_report, dict) and isinstance(recovery, dict):
        load_report = recovery.get("load_report")
    if isinstance(load_report, dict) and load_report.get("healthy") is False:
        gates["load_report_health"] = {
            "metric": "load_report.healthy",
            "value": False,
            "expected": True,
            "ok": False,
        }
    if isinstance(recovery, dict) and recovery.get("healthy") is False:
        gates["recovery_health"] = {
            "metric": "recovery.healthy",
            "value": False,
            "expected": True,
            "ok": False,
        }
    topology = current.get("reviver_topology")
    if not isinstance(topology, dict) or topology.get("standby_health_ok") is not False:
        return gates
    gates["reviver_standby_health"] = {
        "metric": "standby_health_ok",
        "value": False,
        "expected": True,
        "ok": False,
    }
    return gates


def summary_failure_context(payload: dict[str, object]) -> str:
    summary = payload.get("summary")
    if not isinstance(summary, dict):
        return ""
    parts: list[str] = []
    first_gate = summary.get("first_failed_gate")
    if isinstance(first_gate, str) and first_gate:
        parts.append(f"first_failed_gate={first_gate}")
    first_stage = summary.get("first_failure_stage")
    if isinstance(first_stage, str) and first_stage:
        parts.append(f"first_failure_stage={first_stage}")
    return f" ({', '.join(parts)})" if parts else ""


def failed_gate_details(gates: dict[str, dict[str, object]]) -> list[dict[str, object]]:
    failures: list[dict[str, object]] = []
    for name, gate in gates.items():
        if gate.get("ok") is not False:
            continue
        failures.append({"name": name, **gate})
    return failures


def validate_summary_gates(payload: dict[str, object]) -> None:
    gates = payload.get("gates")
    if not isinstance(gates, dict):
        return
    context = summary_failure_context(payload)
    for parameter, value in gates.items():
        if not isinstance(value, dict) or value.get("ok") is True:
            continue
        metric = value.get("metric", parameter)
        observed = value.get("value")
        if "maximum" in value:
            raise RuntimeError(
                f"{metric} above {parameter}: {observed}>{value['maximum']}{context}"
            )
        if "expected" in value:
            raise RuntimeError(
                f"{metric} expected {value['expected']}: {observed}{context}"
            )
        raise RuntimeError(
            f"{metric} below {parameter}: {observed}<{value.get('minimum')}{context}"
        )


def build_reviver_topology_payload(
    leadership: ReviverLeadership,
    standby_health_status: str,
    standby_health_ok: bool = True,
    standby_health: list[dict[str, object]] | None = None,
) -> dict[str, object]:
    return {
        "leader": leadership.leader["name"],
        "leader_pid": leadership.leader.get("pid", ""),
        "registered_revivers": leadership.active_count + leadership.standby_count,
        "active_count": leadership.active_count,
        "standby_count": leadership.standby_count,
        "standby_health_ok": standby_health_ok,
        "leadership_status": leadership.status,
        "standby_health_status": standby_health_status,
        "standby_health": standby_health or [],
    }


def summary_parameters(args: argparse.Namespace) -> dict[str, object]:
    return {
        "build": args.build,
        "machined": args.machined,
        "atlas_tool": str(args.atlas_tool) if args.atlas_tool else "",
        "cellappmgr_name": args.cellappmgr_name,
        "reviver_name": args.reviver_name,
        "min_revivers": args.min_revivers,
        "min_cellapps": args.min_cellapps,
        "timeout_sec": args.timeout_sec,
        "poll_sec": args.poll_sec,
        "stability_sec": args.stability_sec,
        "cycles": args.cycles,
        "max_takeover_ms": args.max_takeover_ms,
        "max_reviver_failover_ms": args.max_reviver_failover_ms,
        "max_load_report_age_ms": args.max_load_report_age_ms,
        "min_post_failover_standbys": args.min_post_failover_standbys,
        "shutdown_reason": args.shutdown_reason,
        "allow_empty_snapshot": args.allow_empty_snapshot,
        "allow_missing_backup_snapshot": args.allow_missing_backup_snapshot,
        "allow_empty_output_log": args.allow_empty_output_log,
        "allow_topology_change": args.allow_topology_change,
        "no_inject": args.no_inject,
        "verify_reviver_failover": args.verify_reviver_failover,
        "reviver_failover_cycles": args.reviver_failover_cycles,
    }


def recovery_has_non_load_report_failure(recovery: dict[str, object]) -> bool:
    saw_component_health = False
    for key, value in recovery.items():
        if key in ("healthy", "load_report") or not isinstance(value, dict):
            continue
        if "healthy" not in value:
            continue
        saw_component_health = True
        if value.get("healthy") is False:
            return True
    return not saw_component_health and recovery.get("healthy") is False


def ha_cycle_failure_stages(result: HaCycleResult) -> list[str]:
    stages: list[str] = []
    if result.failure_stage:
        stages.append(result.failure_stage)
    elif result.takeover_healthy is False:
        stages.append("cycle_takeover")
    if result.stability_healthy is False:
        stages.append("cycle_stability")
    report = cycle_load_report_payload(result)
    if report is not None and report.get("healthy") is False:
        stages.append("cycle_load_report")
    if (
        result.recovery is not None
        and result.recovery.get("healthy") is False
        and recovery_has_non_load_report_failure(result.recovery)
    ):
        stages.append("cycle_recovery")
    return stages


def cycle_failure_stage_names(results: list[HaCycleResult]) -> list[str]:
    return [stage for result in results for stage in ha_cycle_failure_stages(result)]


def ha_cycle_payload(result: HaCycleResult) -> dict[str, object]:
    stages = ha_cycle_failure_stages(result)
    return {
        **result._asdict(),
        "healthy": ha_cycle_is_successful(result),
        "failure_stages": stages,
        "first_failure_stage": stages[0] if stages else "",
    }


def build_summary_payload(
    mode: str,
    results: list[HaCycleResult],
    current: dict[str, object] | None = None,
    reviver_failovers: list[ReviverFailoverResult] | None = None,
    parameters: dict[str, object] | None = None,
) -> dict[str, object]:
    summary = summarize_ha_cycles(results)
    reviver_failover_results = reviver_failovers or []
    if reviver_failover_results:
        summary.update(summarize_reviver_failovers(reviver_failover_results))
    health_gates = summary_health_gate_evaluations(summary)
    health_gates.update(current_health_gate_evaluations(current))
    if mode == "no-inject":
        summary["success_rate"] = 0.0 if health_gates else 1.0
    run_failure_stage = ""
    if current is not None:
        run_healthy = current.get("run_healthy") is not False
        summary["run_health_checks"] = 1
        summary["run_healthy"] = 1 if run_healthy else 0
        summary["run_unhealthy"] = 0 if run_healthy else 1
        if not run_healthy and isinstance(current.get("failure_stage"), str):
            run_failure_stage = str(current["failure_stage"])
    current_failure_stages: list[str] = []
    if current is not None:
        if current.get("stability_healthy") is False:
            current_failure_stages.append("stability")
        recovery = current.get("recovery")
        load_report = current.get("load_report")
        if not isinstance(load_report, dict) and isinstance(recovery, dict):
            load_report = recovery.get("load_report")
        if isinstance(load_report, dict) and load_report.get("healthy") is False:
            current_failure_stages.append("load_report")
        if (
            isinstance(recovery, dict)
            and recovery.get("healthy") is False
            and recovery_has_non_load_report_failure(recovery)
        ):
            current_failure_stages.append("recovery")
        topology = current.get("reviver_topology")
        if isinstance(topology, dict) and topology.get("standby_health_ok") is False:
            current_failure_stages.append("reviver_standby_health")
    cycle_failure_stages = cycle_failure_stage_names(results)
    reviver_failover_failure_stages = [
        "reviver_failover"
        for result in reviver_failover_results
        if result.healthy is False
    ]
    failure_stages = list(
        dict.fromkeys(
            [
                run_failure_stage,
                *current_failure_stages,
                *cycle_failure_stages,
                *reviver_failover_failure_stages,
            ]
        )
    )
    summary["run_failure_stage"] = run_failure_stage
    summary["current_failure_stages"] = current_failure_stages
    summary["cycle_failure_stages"] = cycle_failure_stages
    summary["reviver_failover_failure_stages"] = reviver_failover_failure_stages
    summary["failure_stages"] = [stage for stage in failure_stages if stage]
    summary["first_failure_stage"] = (
        summary["failure_stages"][0] if summary["failure_stages"] else ""
    )
    payload: dict[str, object] = {
        "schema_version": 1,
        "mode": mode,
        "summary": summary,
        "cycles": [ha_cycle_payload(result) for result in results],
    }
    if parameters is not None:
        payload["parameters"] = parameters
    gates = summary_gate_evaluations(summary, parameters or {}, current)
    gates.update(health_gates)
    failed_gates = failed_gate_details(gates)
    failed_gate_names = [str(gate["name"]) for gate in failed_gates]
    summary["gate_count"] = len(gates)
    summary["gate_failures"] = len(failed_gate_names)
    summary["failed_gate_names"] = failed_gate_names
    summary["failed_gates"] = failed_gates
    summary["first_failed_gate"] = failed_gate_names[0] if failed_gate_names else ""
    summary["overall_healthy"] = summary["gate_failures"] == 0
    summary["overall_success_rate"] = 1.0 if summary["overall_healthy"] else 0.0
    if gates:
        payload["gates"] = gates
    if reviver_failover_results:
        payload["reviver_failovers"] = [
            result._asdict() for result in reviver_failover_results
        ]
    if current is not None:
        payload["current"] = current
    return payload


def failed_run_current_payload(stage: str, detail: str) -> dict[str, object]:
    return {
        "run_healthy": False,
        "failure_stage": stage,
        "failure_detail": detail,
    }


def failed_ha_cycle_result(
    cycle: int,
    old_pid: str,
    generation_before: int,
    launch_count_before: int,
    started_at: float,
    stage: str,
    detail: str,
) -> HaCycleResult:
    status = f"takeover=failed stage={stage} detail={detail}"
    elapsed = elapsed_ms(started_at)
    return HaCycleResult(
        cycle=cycle,
        old_pid=old_pid,
        new_pid="",
        generation_before=generation_before,
        generation_after=generation_before,
        launch_count_before=launch_count_before,
        launch_count_after=launch_count_before,
        restore_status=status,
        pre_topology_status=status,
        pre_snapshot_status=status,
        restored_topology_status=status,
        post_restore_snapshot_status=status,
        output_status=status,
        stability_status=status,
        manager_restart_ms=elapsed,
        reviver_retarget_ms=elapsed,
        restore_converged_ms=elapsed,
        takeover_elapsed_ms=elapsed,
        recovery_status=status,
        takeover_healthy=False,
        failure_stage=stage,
    )


def write_summary_json(
    path: Path,
    mode: str,
    results: list[HaCycleResult],
    current: dict[str, object] | None = None,
    reviver_failovers: list[ReviverFailoverResult] | None = None,
    parameters: dict[str, object] | None = None,
) -> dict[str, object]:
    try:
        payload = build_summary_payload(
            mode, results, current, reviver_failovers, parameters
        )
        write_json_atomic(path, payload)
        return payload
    except OSError as ex:
        raise RuntimeError(f"failed to write summary JSON {path}: {ex}") from ex


def expected_reattach_state(restored_cellapps: int, pending: int, stuck: int) -> str:
    if stuck:
        return "stuck"
    if pending:
        return "pending"
    if restored_cellapps == 0:
        return "idle"
    return "complete"


def reattach_health_detail(
    restored_cellapps: int,
    pending: int,
    completed_count: int,
    stuck: int,
    completed: str,
    reattach_state: str,
    status: str,
    min_cellapps: int,
    require_restored: bool,
) -> tuple[bool, str, bool]:
    status_fields = summary_fields(status)
    expected_state = expected_reattach_state(restored_cellapps, pending, stuck)
    counts_ok = pending == 0 and completed_count == restored_cellapps
    if require_restored:
        counts_ok = counts_ok and restored_cellapps >= min_cellapps
    completed_ok = completed == "true"
    state_ok = reattach_state == expected_state
    status_ok = (
        summary_has(status_fields, "restored", str(restored_cellapps))
        and summary_has(status_fields, "pending", str(pending))
        and summary_has(status_fields, "stuck", str(stuck))
        and summary_has(status_fields, "completed_count", str(completed_count))
        and summary_has(status_fields, "completed", "1" if completed_ok else "0")
        and summary_has(status_fields, "state", expected_state)
    )
    ok = counts_ok and completed_ok and stuck == 0 and state_ok and status_ok
    detail = (
        f"restored_cellapps={restored_cellapps} pending={pending} "
        f"completed_count={completed_count} completed={completed} stuck={stuck} "
        f"reattach_counts_ok={counts_ok} "
        f"reattach_state={reattach_state}/{expected_state} "
        f"reattach_state_ok={state_ok} "
        f"reattach_status_ok={status_ok}; status={status}"
    )
    return ok, detail, stuck != 0


def build_reattach_health_payload(
    restored_cellapps: int,
    pending: int,
    completed_count: int,
    stuck: int,
    completed: str,
    reattach_state: str,
    status: str,
    min_cellapps: int,
    require_restored: bool,
) -> dict[str, object]:
    ok, detail, has_stuck = reattach_health_detail(
        restored_cellapps,
        pending,
        completed_count,
        stuck,
        completed,
        reattach_state,
        status,
        min_cellapps,
        require_restored,
    )
    return {
        "healthy": ok,
        "detail": detail,
        "restored_cellapps": restored_cellapps,
        "pending": pending,
        "completed_count": completed_count,
        "stuck": stuck,
        "has_stuck": has_stuck,
        "completed": completed == "true",
        "state": reattach_state,
        "expected_state": expected_reattach_state(restored_cellapps, pending, stuck),
        "min_cellapps": min_cellapps,
        "require_restored": require_restored,
        "status": status,
    }


def restore_gate_health_detail(
    pending: int,
    active: str,
    pending_geometry: int,
    blocked_pending_geometry: int,
    status: str,
) -> tuple[bool, str]:
    status_fields = summary_fields(status)
    expected_active = pending != 0 or blocked_pending_geometry != 0
    expected_state = "closed" if expected_active else "open"
    active_ok = active == ("true" if expected_active else "false")
    status_ok = (
        summary_has(status_fields, "state", expected_state)
        and summary_has(status_fields, "active", "1" if expected_active else "0")
        and summary_has(status_fields, "lb_frozen", "1" if pending != 0 else "0")
        and summary_has(status_fields, "pending_reattach", str(pending))
        and summary_has(status_fields, "pending_geometry", str(pending_geometry))
        and summary_has(status_fields, "blocked_pending_geometry", str(blocked_pending_geometry))
    )
    open_ok = pending == 0 and blocked_pending_geometry == 0 and active == "false"
    ok = open_ok and status_ok
    detail = (
        f"restore_gate_active={active}/{expected_active} "
        f"restore_gate_active_ok={active_ok} "
        f"restore_gate_pending_geometry={pending_geometry} "
        f"restore_gate_blocked_pending_geometry={blocked_pending_geometry} "
        f"restore_gate_status_ok={status_ok}; restore_gate_status={status}"
    )
    return ok, detail


def build_restore_gate_health_payload(
    pending: int,
    active: str,
    pending_geometry: int,
    blocked_pending_geometry: int,
    status: str,
) -> dict[str, object]:
    ok, detail = restore_gate_health_detail(
        pending, active, pending_geometry, blocked_pending_geometry, status
    )
    expected_active = pending != 0 or blocked_pending_geometry != 0
    return {
        "healthy": ok,
        "detail": detail,
        "active": active == "true",
        "expected_active": expected_active,
        "pending_reattach": pending,
        "pending_geometry": pending_geometry,
        "blocked_pending_geometry": blocked_pending_geometry,
        "status": status,
    }


def reattach_registry_health_detail(status: str) -> tuple[bool, str]:
    fields = summary_fields(status)
    state = fields.get("state", "")
    query_pending = fields.get("query_pending", "")
    last_blocked = fields.get("last_blocked", "")
    error_detail = fields.get("error_detail", "")
    ok = (
        state in {"idle", "healthy", "reconciled"}
        and query_pending == "0"
        and last_blocked == "0"
        and error_detail == "none"
    )
    detail = (
        f"reattach_registry_state={state} "
        f"reattach_registry_query_pending={query_pending} "
        f"reattach_registry_last_blocked={last_blocked} "
        f"reattach_registry_ok={ok}; reattach_registry_status={status}"
    )
    return ok, detail


def build_reattach_registry_health_payload(status: str) -> dict[str, object]:
    ok, detail = reattach_registry_health_detail(status)
    fields = summary_fields(status)
    return {
        "healthy": ok,
        "detail": detail,
        "state": fields.get("state", ""),
        "query_pending": fields.get("query_pending", ""),
        "last_missing": fields.get("last_missing", ""),
        "last_blocked": fields.get("last_blocked", ""),
        "last_reconciled": fields.get("last_reconciled", ""),
        "reconciled_total": fields.get("reconciled_total", ""),
        "error_detail": fields.get("error_detail", ""),
        "status": status,
    }


def build_load_report_health_report(
    stale_count: int,
    cellapps_summary: str,
    min_cellapps: int,
    max_age_ms: int = 0,
    enforce_max_age: bool = True,
) -> LoadReportHealthReport:
    count_match = re.search(r"\bcellapps=(\d+)\b", cellapps_summary)
    reported_count = int(count_match.group(1)) if count_match else -1
    raw_records = [
        match.groupdict() for match in CELLAPP_LOAD_SUMMARY_RE.finditer(cellapps_summary)
    ]
    records: list[dict[str, object]] = []
    for record in raw_records:
        records.append(
            {
                "app_id": int(record["app"]),
                "addr": record["addr"],
                "load": float(record["load"]),
                "entities": int(record["entities"]),
                "retiring": record["retiring"] != "0",
                "load_age_ms": int(record["load_age_ms"]),
                "load_stale": record["load_stale"] != "0",
            }
        )
    stale_records = [str(record["app_id"]) for record in records if record["load_stale"]]
    negative_age_records = [
        str(record["app_id"]) for record in records if int(record["load_age_ms"]) < 0
    ]
    over_age_records = [
        str(record["app_id"])
        for record in records
        if max_age_ms > 0 and int(record["load_age_ms"]) > max_age_ms
    ]
    count_ok = (
        count_match is not None
        and reported_count >= min_cellapps
        and reported_count == len(records)
    )
    stale_ok = stale_count == 0 and not stale_records
    age_ok = not negative_age_records
    max_age_ok = not over_age_records
    ok = count_ok and stale_ok and age_ok and (max_age_ok or not enforce_max_age)
    detail = (
        f"load_report_count={reported_count}/{min_cellapps} "
        f"load_report_records={len(records)} "
        f"load_report_count_ok={count_ok} "
        f"load_report_stale_count={stale_count} "
        f"load_report_stale_apps={','.join(stale_records) or 'none'} "
        f"load_report_age_ok={age_ok} "
        f"load_report_max_age_ms={max_age_ms} "
        f"load_report_over_age_apps={','.join(over_age_records) or 'none'}; "
        f"cellapps={cellapps_summary}"
    )
    payload: dict[str, object] = {
        "healthy": ok,
        "detail": detail,
        "reported_count": reported_count,
        "min_cellapps": min_cellapps,
        "record_count": len(records),
        "count_ok": count_ok,
        "stale_count": stale_count,
        "stale_apps": stale_records,
        "negative_age_apps": negative_age_records,
        "age_ok": age_ok,
        "max_age_ms": max_age_ms,
        "max_age_ok": max_age_ok,
        "max_age_enforced": enforce_max_age,
        "over_age_apps": over_age_records,
        "records": records,
    }
    return LoadReportHealthReport(ok, detail, payload)


def load_report_health_detail(
    stale_count: int,
    cellapps_summary: str,
    min_cellapps: int,
    max_age_ms: int = 0,
) -> tuple[bool, str]:
    report = build_load_report_health_report(
        stale_count, cellapps_summary, min_cellapps, max_age_ms
    )
    return report.ok, report.status


def reviver_heartbeat_snapshot_health_detail(
    acks: int,
    min_acks: int,
    baseline_failures: int,
    snapshot_dirty: str,
    snapshot_stale: str,
    snapshot_failures: int,
    snapshot_status: str,
) -> tuple[bool, str, bool]:
    fields = summary_fields(snapshot_status)
    expected_state = "ready"
    if snapshot_failures:
        expected_state = "failed"
    elif snapshot_dirty == "true":
        expected_state = "dirty"
    elif snapshot_stale == "true":
        expected_state = "stale"
    elif acks < min_acks:
        expected_state = "unknown"
    dirty_digit = "1" if snapshot_dirty == "true" else "0"
    stale_digit = "1" if snapshot_stale == "true" else "0"
    status_ok = (
        summary_has(fields, "state", expected_state)
        and summary_has(fields, "failures", str(snapshot_failures))
        and summary_has(fields, "dirty", dirty_digit)
        and summary_has(fields, "stale", stale_digit)
    )
    new_failures = snapshot_failures > baseline_failures
    ok = (
        acks >= min_acks
        and snapshot_dirty == "false"
        and snapshot_stale == "false"
        and not new_failures
        and status_ok
    )
    detail = (
        f"heartbeat_acks={acks}/{min_acks} "
        f"heartbeat_snapshot_dirty={snapshot_dirty} "
        f"heartbeat_snapshot_save_stale={snapshot_stale} "
        f"heartbeat_snapshot_failures={baseline_failures}->{snapshot_failures} "
        f"heartbeat_snapshot_status_ok={status_ok}; "
        f"heartbeat_snapshot_status={snapshot_status}"
    )
    return ok, detail, new_failures


def reviver_failover_health_detail(
    expected_manager_pid: str,
    current_manager_pid: str,
    active_pid: str,
    baseline_generation: int,
    generation: int,
    baseline_launch_count: int,
    launch_count: int,
    baseline_heartbeat_acks: int,
    heartbeat_acks: int,
) -> tuple[bool, str]:
    manager_pid_ok = current_manager_pid == expected_manager_pid
    active_pid_ok = active_pid == expected_manager_pid
    generation_ok = generation >= baseline_generation
    launch_count_ok = launch_count == baseline_launch_count
    heartbeat_ok = heartbeat_acks > baseline_heartbeat_acks
    ok = (
        manager_pid_ok
        and active_pid_ok
        and generation_ok
        and launch_count_ok
        and heartbeat_ok
    )
    detail = (
        f"manager_pid={current_manager_pid}/{expected_manager_pid} "
        f"manager_pid_ok={manager_pid_ok} "
        f"reviver_active_pid={active_pid}/{expected_manager_pid} "
        f"reviver_active_pid_ok={active_pid_ok} "
        f"generation={baseline_generation}->{generation} "
        f"generation_ok={generation_ok} "
        f"launch_count={baseline_launch_count}->{launch_count} "
        f"launch_count_ok={launch_count_ok} "
        f"heartbeat_acks={baseline_heartbeat_acks}->{heartbeat_acks} "
        f"heartbeat_ok={heartbeat_ok}"
    )
    return ok, detail


def standby_reviver_health_detail(
    leader_active: str,
    status: str,
    launch_pending: str,
    restart_limit: str,
) -> tuple[bool, str]:
    leader_inactive_ok = leader_active == "false"
    status_ok = status == "standby"
    launch_pending_ok = launch_pending == "false"
    restart_limit_ok = restart_limit == "false"
    ok = leader_inactive_ok and status_ok and launch_pending_ok and restart_limit_ok
    detail = (
        f"standby_leader_active={leader_active} "
        f"standby_leader_inactive_ok={leader_inactive_ok} "
        f"standby_status={status} standby_status_ok={status_ok} "
        f"standby_launch_pending={launch_pending} "
        f"standby_launch_pending_ok={launch_pending_ok} "
        f"standby_restart_limit={restart_limit} "
        f"standby_restart_limit_ok={restart_limit_ok}"
    )
    return ok, detail


def build_standby_reviver_health_record(
    reviver: dict[str, str],
    leader_active: str,
    status: str,
    launch_pending: str,
    restart_limit: str,
) -> tuple[bool, dict[str, object], str]:
    ok, detail = standby_reviver_health_detail(
        leader_active, status, launch_pending, restart_limit
    )
    return (
        ok,
        {
            "name": reviver["name"],
            "pid": reviver.get("pid", ""),
            "leader_active": leader_active == "true",
            "status": status,
            "launch_pending": launch_pending == "true",
            "restart_limit_reached": restart_limit == "true",
            "healthy": ok,
            "detail": detail,
        },
        detail,
    )


def read_recovery_health(
    args: argparse.Namespace, exe: Path, target: str, require_restored: bool
) -> RecoveryHealthReport:
    restored_cellapps = int_watcher(exe, args.machined, target, "cellappmgr/ha/restored_cellapps")
    pending = int_watcher(exe, args.machined, target, "cellappmgr/ha/reattach_pending")
    completed_count = int_watcher(
        exe, args.machined, target, "cellappmgr/ha/reattach_completed_count"
    )
    stuck = int_watcher(exe, args.machined, target, "cellappmgr/ha/reattach_stuck")
    completed = watcher_value(exe, args.machined, target, "cellappmgr/ha/reattach_completed")
    reattach_state = watcher_value(exe, args.machined, target, "cellappmgr/ha/reattach_state")
    status = watcher_value(exe, args.machined, target, "cellappmgr/ha/reattach_status")
    reattach_payload = build_reattach_health_payload(
        restored_cellapps,
        pending,
        completed_count,
        stuck,
        completed,
        reattach_state,
        status,
        args.min_cellapps,
        require_restored,
    )
    gate_active = watcher_value(exe, args.machined, target, "cellappmgr/ha/restore_gate_active")
    pending_geometry = int_watcher(
        exe, args.machined, target, "cellappmgr/lb/pending_geometry_broadcasts"
    )
    blocked_geometry = int_watcher(
        exe, args.machined, target, "cellappmgr/ha/restore_gate_blocked_pending_geometry"
    )
    gate_status = watcher_value(exe, args.machined, target, "cellappmgr/ha/restore_gate_status")
    gate_payload = build_restore_gate_health_payload(
        pending, gate_active, pending_geometry, blocked_geometry, gate_status
    )
    registry_status = watcher_value(
        exe, args.machined, target, "cellappmgr/ha/reattach_registry_status"
    )
    registry_payload = build_reattach_registry_health_payload(registry_status)
    load_report = read_load_report_health(args, exe, target, enforce_max_age=False)
    ok = (
        bool(reattach_payload["healthy"])
        and bool(gate_payload["healthy"])
        and bool(registry_payload["healthy"])
        and load_report.ok
    )
    status_text = (
        f"{reattach_payload['detail']}; {gate_payload['detail']}; "
        f"{registry_payload['detail']}; {load_report.status}"
    )
    payload: dict[str, object] = {
        "healthy": ok,
        "reattach": reattach_payload,
        "restore_gate": gate_payload,
        "reattach_registry": registry_payload,
        "load_report": load_report.payload,
    }
    return RecoveryHealthReport(ok, status_text, bool(reattach_payload["has_stuck"]), payload)


def read_reattach_health(
    args: argparse.Namespace, exe: Path, target: str, require_restored: bool
) -> tuple[bool, str, bool]:
    report = read_recovery_health(args, exe, target, require_restored)
    return report.ok, report.status, report.stuck


def read_load_report_health(
    args: argparse.Namespace,
    exe: Path,
    target: str,
    enforce_max_age: bool = True,
) -> LoadReportHealthReport:
    stale_count = int_watcher(
        exe, args.machined, target, "cellappmgr/lb/load_report_stale_count"
    )
    cellapps_summary = watcher_value(exe, args.machined, target, "cellappmgr/lb/cellapps")
    return build_load_report_health_report(
        stale_count,
        cellapps_summary,
        args.min_cellapps,
        args.max_load_report_age_ms,
        enforce_max_age,
    )


def wait_for_load_report_health(
    args: argparse.Namespace, exe: Path, target: str
) -> LoadReportHealthReport:
    def check():
        report = read_load_report_health(args, exe, target)
        return report.ok, report

    return wait_until(args.timeout_sec, args.poll_sec, check)


def wait_for_recovery_health(
    args: argparse.Namespace, exe: Path, target: str, require_restored: bool
) -> RecoveryHealthReport:
    def check():
        report = read_recovery_health(args, exe, target, require_restored)
        if report.stuck:
            raise RuntimeError(f"reattach stuck; {report.status}")
        return report.ok, report

    return wait_until(args.timeout_sec, args.poll_sec, check)


def wait_or_capture_recovery_health(
    args: argparse.Namespace,
    exe: Path,
    target: str,
    require_restored: bool,
    capture_unhealthy: bool = False,
) -> RecoveryHealthReport:
    try:
        return wait_for_recovery_health(args, exe, target, require_restored)
    except RuntimeError:
        if not capture_unhealthy:
            raise
        return read_recovery_health(args, exe, target, require_restored)


def wait_for_reattach_health(
    args: argparse.Namespace, exe: Path, target: str, require_restored: bool
) -> str:
    return wait_for_recovery_health(args, exe, target, require_restored).status


def snapshot_failure_counters(
    args: argparse.Namespace, exe: Path, target: str
) -> tuple[int, int, int]:
    save_failures = int_watcher(
        exe, args.machined, target, "cellappmgr/ha/snapshot_save_failures"
    )
    restore_failures = int_watcher(
        exe, args.machined, target, "cellappmgr/ha/snapshot_restore_failures"
    )
    failures = int_watcher(exe, args.machined, target, "cellappmgr/ha/snapshot_failures")
    return save_failures, restore_failures, failures


def snapshot_failure_counter_mismatch(
    save_failures: int, restore_failures: int, failures: int
) -> str:
    if failures == save_failures + restore_failures:
        return ""
    return (
        f"snapshot_failures={failures} does not equal "
        f"save_failures={save_failures} + restore_failures={restore_failures}"
    )


def wait_until(timeout_sec: float, poll_sec: float, fn):
    deadline = time.monotonic() + timeout_sec
    last = None
    while time.monotonic() < deadline:
        ok, last = fn()
        if ok:
            return last
        time.sleep(poll_sec)
    raise RuntimeError(str(last or "timeout"))


def topology_fingerprint(summary: str) -> tuple[str, int]:
    parts = [part.strip() for part in summary.split("|")]
    if not parts or not parts[0].startswith("spaces="):
        raise RuntimeError(f"unexpected cellappmgr/lb/spaces summary: {summary}")
    try:
        expected_spaces = int(parts[0].split("=", 1)[1])
    except ValueError as ex:
        raise RuntimeError(f"unexpected cellappmgr/lb/spaces count: {parts[0]}") from ex

    spaces: list[str] = []
    pending_total = 0
    for part in parts[1:]:
        match = SPACE_RE.search(part)
        if not match:
            raise RuntimeError(f"unexpected space topology segment: {part}")
        leaves = int(match["leaves"])
        pending_total += int(match["pending"])
        cells: list[str] = []
        for cell in CELL_RE.finditer(part):
            bounds = ",".join(piece.strip() for piece in cell["bounds"].split(","))
            cells.append(f"cell={cell['cell']}:app={cell['app']}:bounds={bounds}")
        if len(cells) != leaves:
            raise RuntimeError(
                f"space={match['space']} expected {leaves} leaves but parsed {len(cells)}"
            )
        spaces.append(
            f"space={match['space']}:version={match['version']}:"
            f"freeze={match['freeze']}:primary={match['primary']}:"
            f"leaves={match['leaves']}:cells=[{';'.join(cells)}]"
        )

    if len(spaces) != expected_spaces:
        raise RuntimeError(
            f"expected {expected_spaces} spaces but parsed {len(spaces)}: {summary}"
        )
    return f"spaces={expected_spaces}|{'|'.join(spaces)}", pending_total


def topology_brief(fingerprint: str) -> str:
    count = "?"
    if fingerprint.startswith("spaces="):
        count = fingerprint.split("|", 1)[0].split("=", 1)[1]
    return f"topology=spaces={count} cells={fingerprint.count('cell=')}"


def read_topology_fingerprint(
    args: argparse.Namespace, exe: Path, target: str
) -> tuple[str, int]:
    summary = watcher_value(exe, args.machined, target, "cellappmgr/lb/spaces")
    return topology_fingerprint(summary)


def read_snapshot_topology_fingerprint(
    args: argparse.Namespace, exe: Path, target: str
) -> tuple[str, int]:
    summary = watcher_value(
        exe, args.machined, target, "cellappmgr/ha/snapshot_last_save_topology"
    )
    if not summary:
        return "", 0
    return topology_fingerprint(summary)


def read_restore_topology_fingerprint(
    args: argparse.Namespace, exe: Path, target: str
) -> tuple[str, int]:
    summary = watcher_value(
        exe, args.machined, target, "cellappmgr/ha/snapshot_last_restore_topology"
    )
    if not summary:
        return "", 0
    return topology_fingerprint(summary)


def wait_for_topology_quiescence(
    args: argparse.Namespace, exe: Path, target: str, expected: str | None = None
) -> tuple[str | None, str]:
    if args.allow_topology_change:
        return None, "topology=skipped"

    def check():
        fingerprint, pending = read_topology_fingerprint(args, exe, target)
        if pending:
            return False, f"waiting for topology pending_ack=0; {topology_brief(fingerprint)}"
        if expected is not None and fingerprint != expected:
            return False, (
                "waiting for restored topology to match; "
                f"expected={topology_brief(expected)} actual={topology_brief(fingerprint)}"
            )
        return True, (fingerprint, topology_brief(fingerprint))

    return wait_until(args.timeout_sec, args.poll_sec, check)


@dataclass
class LeaderLockHealth:
    mode: str
    leader_active: bool
    holder_id: str
    acquire_count: int
    lease_renew_count: int
    lease_failure_count: int
    healthy: bool
    detail: str


def read_cellappmgr_leader_lock_health(exe: Path, machined: str, target: str,
                                       required_mode: str) -> LeaderLockHealth:
    """Mirror of verify_baseappmgr_ha.read_leader_lock_health for the
    reviver/leader/* (cellappmgr) surface. The cellappmgr Reviver still
    uses the legacy reviver/leader/* watcher names for backward compat
    with the verify_cellappmgr_ha summary schema."""
    mode = watcher_value(exe, machined, target, "reviver/leader/mode")
    leader_active = watcher_value(exe, machined, target, "reviver/leader/active") == "true"
    holder = watcher_value(exe, machined, target, "reviver/leader/holder_id")
    acquire_count = int_watcher(exe, machined, target, "reviver/leader/acquire_count")
    renew_count = int_watcher(exe, machined, target, "reviver/leader/lease_renew_count")
    failure_count = int_watcher(exe, machined, target, "reviver/leader/lease_failure_count")
    healthy = True
    parts = [
        f"mode={mode}", f"leader_active={leader_active}", f"acquire_count={acquire_count}",
        f"lease_renew_count={renew_count}", f"lease_failure_count={failure_count}"
    ]
    if required_mode and mode != required_mode:
        healthy = False
        parts.append(f"required_mode={required_mode} mismatch")
    if required_mode == "machined" and not leader_active:
        healthy = False
        parts.append("leader not active under machined mode")
    return LeaderLockHealth(mode, leader_active, holder, acquire_count, renew_count,
                            failure_count, healthy, " ".join(parts))


def select_leader_reviver(
    revivers: list[dict[str, str]],
    requested_name: str,
    is_active: Callable[[dict[str, str]], bool],
) -> ReviverLeadership:
    active = [reviver for reviver in revivers if is_active(reviver)]
    if len(active) > 1:
        names = ",".join(reviver["name"] for reviver in active)
        raise RuntimeError(f"multiple active Reviver leaders: {names}")
    if requested_name:
        leader = choose_process(revivers, requested_name, "reviver")
        if not active or active[0]["name"] != leader["name"]:
            raise RuntimeError(f"requested Reviver {requested_name} is not the active leader")
    else:
        if not active:
            raise RuntimeError("no active Reviver leader found")
        leader = active[0]
    standby_count = len(revivers) - 1
    status = (
        f"revivers={len(revivers)} active=1 standby={standby_count} "
        f"leader={leader['name']}"
    )
    return ReviverLeadership(leader, 1, standby_count, status)


def find_leader_reviver(
    exe: Path, machined: str, revivers: list[dict[str, str]], requested_name: str
) -> ReviverLeadership:
    def is_active(reviver: dict[str, str]) -> bool:
        target = f"reviver:{reviver['name']}"
        return watcher_value(exe, machined, target, "reviver/leader/active") == "true"

    return select_leader_reviver(revivers, requested_name, is_active)


def read_standby_reviver_health(
    args: argparse.Namespace,
    exe: Path,
    revivers: list[dict[str, str]],
    leader_name: str,
) -> StandbyReviverHealthReport:
    details: list[str] = []
    records: list[dict[str, object]] = []
    ok = True
    standby_count = 0
    for reviver in revivers:
        if reviver["name"] == leader_name:
            continue
        standby_count += 1
        target = f"reviver:{reviver['name']}"
        leader_active = watcher_value(exe, args.machined, target, "reviver/leader/active")
        status = watcher_value(exe, args.machined, target, "reviver/cellappmgr/status")
        launch_pending = watcher_value(
            exe, args.machined, target, "reviver/cellappmgr/launch_pending"
        )
        restart_limit = watcher_value(
            exe, args.machined, target, "reviver/cellappmgr/restart_limit_reached"
        )
        standby_ok, record, detail = build_standby_reviver_health_record(
            reviver, leader_active, status, launch_pending, restart_limit
        )
        ok = ok and standby_ok
        records.append(record)
        details.append(f"{reviver['name']}:{detail}")
    detail_text = "; ".join(details) if details else "none"
    return StandbyReviverHealthReport(
        ok, f"standby_revivers={standby_count} standby_health={detail_text}", records
    )


def wait_for_standby_reviver_health(
    args: argparse.Namespace,
    exe: Path,
    leadership: ReviverLeadership,
    revivers: list[dict[str, str]],
) -> StandbyReviverHealthReport:
    def check():
        report = read_standby_reviver_health(
            args, exe, revivers, leadership.leader["name"]
        )
        return report.ok, report

    return wait_until(args.timeout_sec, args.poll_sec, check)


def wait_or_capture_standby_reviver_health(
    args: argparse.Namespace,
    exe: Path,
    leadership: ReviverLeadership,
    revivers: list[dict[str, str]],
    capture_unhealthy: bool = False,
) -> StandbyReviverHealthReport:
    try:
        return wait_for_standby_reviver_health(args, exe, leadership, revivers)
    except RuntimeError:
        if not capture_unhealthy:
            raise
        return read_standby_reviver_health(args, exe, revivers, leadership.leader["name"])


def refresh_reviver_topology(
    args: argparse.Namespace,
    exe: Path,
    requested_name: str,
    capture_unhealthy: bool = False,
) -> tuple[ReviverLeadership, StandbyReviverHealthReport, list[dict[str, str]]]:
    revivers = list_processes(exe, args.machined, "reviver")
    leadership = find_leader_reviver(exe, args.machined, revivers, requested_name)
    standby_health = wait_or_capture_standby_reviver_health(
        args, exe, leadership, revivers, capture_unhealthy
    )
    return leadership, standby_health, revivers


def read_reviver_failover_baselines(
    args: argparse.Namespace,
    exe: Path,
    revivers: list[dict[str, str]],
    old_leader_name: str,
) -> dict[str, tuple[int, int, int]]:
    baselines: dict[str, tuple[int, int, int]] = {}
    for reviver in revivers:
        if reviver["name"] == old_leader_name:
            continue
        target = f"reviver:{reviver['name']}"
        baselines[reviver["name"]] = (
            int_watcher(exe, args.machined, target, "reviver/cellappmgr/active_generation"),
            int_watcher(exe, args.machined, target, "reviver/cellappmgr/launch_count"),
            int_watcher(exe, args.machined, target, "reviver/cellappmgr/heartbeat_acks"),
        )
    if not baselines:
        raise RuntimeError("Reviver failover requires at least one standby Reviver")
    return baselines


def active_reviver_leaders(
    exe: Path, machined: str, revivers: list[dict[str, str]]
) -> list[dict[str, str]]:
    active: list[dict[str, str]] = []
    for reviver in revivers:
        target = f"reviver:{reviver['name']}"
        if watcher_value(exe, machined, target, "reviver/leader/active") == "true":
            active.append(reviver)
    return active


def failed_reviver_failover_result(
    args: argparse.Namespace,
    exe: Path,
    leadership: ReviverLeadership,
    manager: dict[str, str],
    cycle: int,
    started_at: float,
    detail: str,
) -> ReviverFailoverResult:
    current_revivers: list[dict[str, str]] = []
    active: list[dict[str, str]] = []
    try:
        current_revivers = list_processes(exe, args.machined, "reviver")
        active = active_reviver_leaders(exe, args.machined, current_revivers)
    except RuntimeError:
        current_revivers = []
        active = []
    new_leader = active[0] if len(active) == 1 else {}
    return ReviverFailoverResult(
        cycle=cycle,
        old_leader=leadership.leader["name"],
        new_leader=new_leader.get("name", ""),
        old_pid=leadership.leader.get("pid", ""),
        new_pid=new_leader.get("pid", ""),
        manager_pid=manager.get("pid", ""),
        surviving_revivers=len(current_revivers),
        standby_after=max(0, len(current_revivers) - 1) if new_leader else 0,
        generation_before=0,
        generation_after=0,
        launch_count_before=0,
        launch_count_after=0,
        heartbeat_acks_before=0,
        heartbeat_acks_after=0,
        failover_elapsed_ms=elapsed_ms(started_at),
        leadership_status=(
            f"revivers={len(current_revivers)} active={len(active)}"
            if current_revivers
            else "revivers=unknown active=unknown"
        ),
        standby_health_status="standby_health=unknown",
        standby_health=[],
        status=f"reviver_failover=failed detail={detail}",
        healthy=False,
    )


def wait_for_reviver_leader_failover(
    args: argparse.Namespace,
    exe: Path,
    leadership: ReviverLeadership,
    manager: dict[str, str],
    revivers: list[dict[str, str]],
    cycle: int,
    capture_unhealthy: bool = False,
) -> tuple[ReviverLeadership, ReviverFailoverResult]:
    old_leader = leadership.leader
    baselines = read_reviver_failover_baselines(args, exe, revivers, old_leader["name"])
    start = time.monotonic()
    shutdown_process(exe, args.machined, f"reviver:{old_leader['name']}", args.shutdown_reason)

    def check():
        current_revivers = list_processes(exe, args.machined, "reviver")
        if any(reviver["name"] == old_leader["name"] for reviver in current_revivers):
            return False, f"waiting for old Reviver leader {old_leader['name']} to stop"
        active = active_reviver_leaders(exe, args.machined, current_revivers)
        if len(active) > 1:
            names = ",".join(reviver["name"] for reviver in active)
            raise RuntimeError(f"multiple active Reviver leaders after failover: {names}")
        if not active:
            return False, "waiting for standby Reviver to acquire leader lock"
        new_leader = active[0]
        baseline = baselines.get(new_leader["name"])
        if baseline is None:
            raise RuntimeError(
                f"new Reviver leader {new_leader['name']} was not a baseline standby"
            )
        new_target = f"reviver:{new_leader['name']}"
        current_manager = choose_process(
            list_processes(exe, args.machined, "cellappmgr"),
            args.cellappmgr_name,
            "cellappmgr",
        )
        active_pid = watcher_value(
            exe, args.machined, new_target, "reviver/cellappmgr/active_pid"
        )
        generation = int_watcher(
            exe, args.machined, new_target, "reviver/cellappmgr/active_generation"
        )
        launch_count = int_watcher(
            exe, args.machined, new_target, "reviver/cellappmgr/launch_count"
        )
        heartbeat_acks = int_watcher(
            exe, args.machined, new_target, "reviver/cellappmgr/heartbeat_acks"
        )
        baseline_generation, baseline_launch_count, baseline_heartbeat_acks = baseline
        ok, detail = reviver_failover_health_detail(
            manager["pid"],
            current_manager["pid"],
            active_pid,
            baseline_generation,
            generation,
            baseline_launch_count,
            launch_count,
            baseline_heartbeat_acks,
            heartbeat_acks,
        )
        if not ok:
            return False, f"waiting for Reviver failover health; {detail}"
        new_leadership = select_leader_reviver(
            current_revivers, "", lambda reviver: reviver["name"] == new_leader["name"]
        )
        standby_report = read_standby_reviver_health(
            args, exe, current_revivers, new_leader["name"]
        )
        if not standby_report.ok:
            return False, f"waiting for standby Reviver health; {standby_report.status}"
        return True, (
            new_leadership,
            ReviverFailoverResult(
                cycle=cycle,
                old_leader=old_leader["name"],
                new_leader=new_leader["name"],
                old_pid=old_leader["pid"],
                new_pid=new_leader["pid"],
                manager_pid=manager["pid"],
                surviving_revivers=len(current_revivers),
                standby_after=new_leadership.standby_count,
                generation_before=baseline_generation,
                generation_after=generation,
                launch_count_before=baseline_launch_count,
                launch_count_after=launch_count,
                heartbeat_acks_before=baseline_heartbeat_acks,
                heartbeat_acks_after=heartbeat_acks,
                failover_elapsed_ms=elapsed_ms(start),
                leadership_status=new_leadership.status,
                standby_health_status=standby_report.status,
                standby_health=standby_report.records,
                status=detail,
            ),
        )

    try:
        new_leadership, result = wait_until(args.timeout_sec, args.poll_sec, check)
        new_target = f"reviver:{new_leadership.leader['name']}"
        wait_for_reviver_health(args, exe, new_target, result.heartbeat_acks_before + 1)
        result = result._replace(failover_elapsed_ms=elapsed_ms(start))
        return new_leadership, result
    except RuntimeError as ex:
        if not capture_unhealthy:
            raise
        return leadership, failed_reviver_failover_result(
            args, exe, leadership, manager, cycle, start, str(ex)
        )


def wait_for_snapshot_save(
    args: argparse.Namespace,
    exe: Path,
    target: str,
    require_after_restore: bool = False,
    expected_topology: str | None = None,
) -> str:
    snapshot_path = watcher_value(exe, args.machined, target, "cellappmgr/ha/snapshot_path")
    if not snapshot_path and not args.allow_empty_snapshot:
        raise RuntimeError("CellAppMgr snapshot_path is empty")
    baseline_save_failures, baseline_restore_failures, baseline_failures = (
        snapshot_failure_counters(args, exe, target)
    )
    mismatch = snapshot_failure_counter_mismatch(
        baseline_save_failures, baseline_restore_failures, baseline_failures
    )
    if mismatch:
        raise RuntimeError(mismatch)
    if args.allow_empty_snapshot:
        return "snapshot_save=skipped"

    def check():
        saves = int_watcher(exe, args.machined, target, "cellappmgr/ha/snapshot_saves")
        save_failures, restore_failures, failures = snapshot_failure_counters(args, exe, target)
        mismatch = snapshot_failure_counter_mismatch(save_failures, restore_failures, failures)
        save_path = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_last_save_path"
        )
        save_error = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_last_save_error"
        )
        file_present = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_file_present"
        )
        file_bytes = int_watcher(
            exe, args.machined, target, "cellappmgr/ha/snapshot_file_bytes"
        )
        file_status = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_file_status"
        )
        file_topology_status = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_file_topology_status"
        )
        backup_path = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_backup_path"
        )
        backup_present = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_backup_present"
        )
        backup_bytes = int_watcher(
            exe, args.machined, target, "cellappmgr/ha/snapshot_backup_bytes"
        )
        backup_status = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_backup_status"
        )
        backup_topology_status = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_backup_topology_status"
        )
        attempt_age_ms = int_watcher(
            exe, args.machined, target, "cellappmgr/ha/snapshot_last_save_attempt_age_ms"
        )
        save_age_ms = int_watcher(
            exe, args.machined, target, "cellappmgr/ha/snapshot_last_save_age_ms"
        )
        save_stale = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_save_stale"
        )
        save_dirty = watcher_value(exe, args.machined, target, "cellappmgr/ha/snapshot_dirty")
        save_dirty_age_ms = int_watcher(
            exe, args.machined, target, "cellappmgr/ha/snapshot_dirty_age_ms"
        )
        save_dirty_reason = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_dirty_reason"
        )
        save_status = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_status"
        )
        file_status_fields = summary_fields(file_status)
        file_topology_fields = summary_fields(file_topology_status)
        backup_status_fields = summary_fields(backup_status)
        backup_topology_fields = summary_fields(backup_topology_status)
        save_status_fields = summary_fields(save_status)
        snapshot_topology = None
        snapshot_topology_pending = 0
        snapshot_topology_pending_watcher = 0
        snapshot_topology_ok = args.allow_topology_change or expected_topology is None
        snapshot_status_ok = True
        if expected_topology is not None and not args.allow_topology_change:
            snapshot_topology, snapshot_topology_pending = read_snapshot_topology_fingerprint(
                args, exe, target
            )
            snapshot_topology_pending_watcher = int_watcher(
                exe,
                args.machined,
                target,
                "cellappmgr/ha/snapshot_last_save_topology_pending_ack",
            )
            snapshot_topology_ok = (
                snapshot_topology == expected_topology
                and snapshot_topology_pending == 0
                and snapshot_topology_pending_watcher == 0
            )
            snapshot_status_ok = (
                summary_has(save_status_fields, "topology_present", "1")
                and summary_has(save_status_fields, "topology_pending_ack", "0")
            )
        snapshot_error_ok = (
            summary_has(save_status_fields, "error_present", "0")
            and summary_has(save_status_fields, "error_detail", "none")
        )
        restore_age_ms = -1
        if require_after_restore:
            restore_age_ms = int_watcher(
                exe, args.machined, target, "cellappmgr/ha/snapshot_last_restore_age_ms"
            )
        save_after_restore = (
            not require_after_restore
            or (restore_age_ms >= 0 and save_age_ms <= restore_age_ms)
        )
        file_ready = (
            file_present == "true"
            and file_bytes > 0
            and summary_has(file_status_fields, "state", "ready")
            and summary_has(file_status_fields, "valid", "1")
            and summary_has(file_status_fields, "error_present", "0")
            and summary_has(file_status_fields, "error_detail", "none")
        )
        file_topology_ready = (
            summary_has(file_topology_fields, "state", "ready")
            and summary_has(file_topology_fields, "restorable", "1")
            and summary_has(file_topology_fields, "topology_pending_ack", "0")
            and summary_has(file_topology_fields, "error_present", "0")
            and summary_has(file_topology_fields, "error_detail", "none")
            and (
                args.allow_topology_change
                or expected_topology is None
                or summary_has(file_topology_fields, "matches_expected", "1")
            )
        )
        expected_backup_path = f"{snapshot_path}.bak"
        backup_ready = (
            backup_present == "true"
            and backup_bytes > 0
            and summary_has(backup_status_fields, "state", "ready")
            and summary_has(backup_status_fields, "valid", "1")
            and summary_has(backup_status_fields, "error_present", "0")
            and summary_has(backup_status_fields, "error_detail", "none")
        )
        backup_topology_ready = (
            summary_has(backup_topology_fields, "state", "ready")
            and summary_has(backup_topology_fields, "restorable", "1")
            and summary_has(backup_topology_fields, "topology_pending_ack", "0")
            and summary_has(backup_topology_fields, "error_present", "0")
            and summary_has(backup_topology_fields, "error_detail", "none")
        )
        backup_ok = (
            backup_path == expected_backup_path
            and (
                (backup_ready and backup_topology_ready)
                or (
                    args.allow_missing_backup_snapshot
                    and backup_present == "false"
                    and backup_bytes == 0
                    and summary_has(backup_status_fields, "state", "missing")
                    and summary_has(backup_status_fields, "error_detail", "none")
                    and summary_has(backup_topology_fields, "state", "missing")
                    and summary_has(backup_topology_fields, "error_detail", "none")
                )
            )
        )
        ok = (
            not mismatch
            and saves > 0
            and save_failures == baseline_save_failures
            and save_path == snapshot_path
            and not save_error
            and file_ready
            and file_topology_ready
            and backup_ok
            and attempt_age_ms >= 0
            and save_age_ms >= 0
            and save_stale == "false"
            and save_dirty == "false"
            and save_dirty_age_ms == -1
            and not save_dirty_reason
            and summary_has(save_status_fields, "state", "healthy")
            and summary_has(save_status_fields, "dirty", "0")
            and summary_has(save_status_fields, "dirty_reason", "none")
            and save_after_restore
            and snapshot_topology_ok
            and snapshot_status_ok
            and snapshot_error_ok
        )
        mismatch_prefix = f"{mismatch}; " if mismatch else ""
        topology_status = "skipped"
        if expected_topology is not None and not args.allow_topology_change:
            topology_status = "matched" if snapshot_topology_ok else "stale"
            if snapshot_topology_pending:
                topology_status = f"pending_ack={snapshot_topology_pending}"
            elif snapshot_topology_pending_watcher:
                topology_status = f"pending_ack={snapshot_topology_pending_watcher}"
        return ok, (
            f"{mismatch_prefix}snapshot_saves={saves} "
            f"save_failures={baseline_save_failures}->{save_failures} "
            f"restore_failures={restore_failures} snapshot_failures={failures} "
            f"attempt_age_ms={attempt_age_ms} save_age_ms={save_age_ms} "
            f"restore_age_ms={restore_age_ms} save_after_restore={save_after_restore} "
            f"save_stale={save_stale} save_path={save_path} "
            f"save_dirty={save_dirty} dirty_age_ms={save_dirty_age_ms} "
            f"dirty_reason={save_dirty_reason} "
            f"file_present={file_present} file_bytes={file_bytes} "
            f"file_ready={file_ready} file_status={file_status} "
            f"file_topology_ready={file_topology_ready} "
            f"file_topology_status={file_topology_status} "
            f"backup_path={backup_path} backup_present={backup_present} "
            f"backup_bytes={backup_bytes} backup_ready={backup_ready} "
            f"backup_status={backup_status} "
            f"backup_topology_ready={backup_topology_ready} "
            f"backup_topology_status={backup_topology_status} "
            f"snapshot_topology={topology_status} "
            f"snapshot_status_topology={snapshot_status_ok} "
            f"snapshot_status_error={snapshot_error_ok} "
            f"expected_save_path={snapshot_path} save_error={save_error} "
            f"snapshot_status={save_status}"
        )

    return wait_until(args.timeout_sec, args.poll_sec, check)


def resolve_watcher_path(value: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = REPO_ROOT / path
    return path


def require_reviver_output_path(args: argparse.Namespace, exe: Path, target: str) -> Path | None:
    value = watcher_value(exe, args.machined, target, "reviver/cellappmgr/output_path")
    if not value:
        if args.allow_empty_output_log:
            return None
        raise RuntimeError("Reviver output_path is empty")
    return resolve_watcher_path(value)


def wait_for_revived_output_log(args: argparse.Namespace, path: Path | None, new_pid: str) -> str:
    if path is None:
        return "output_log=skipped"

    needle = f"pid={new_pid}"

    def check():
        if not path.is_file():
            return False, f"waiting for revived output log: {path}"
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as ex:
            return False, f"waiting for revived output log readable: {path}: {ex}"
        return needle in text, f"waiting for {needle} in revived output log: {path}"

    wait_until(args.timeout_sec, args.poll_sec, check)
    return f"output_log={path}"


def wait_for_reviver_health(
    args: argparse.Namespace, exe: Path, target: str, min_heartbeat_acks: int = 1
) -> None:
    baseline_snapshot_failures = int_watcher(
        exe, args.machined, target, "reviver/cellappmgr/heartbeat_snapshot_failures"
    )

    def check():
        status = watcher_value(exe, args.machined, target, "reviver/cellappmgr/status")
        launch_pending = watcher_value(
            exe, args.machined, target, "reviver/cellappmgr/launch_pending"
        )
        checks = int_watcher(exe, args.machined, target, "reviver/cellappmgr/health_checks")
        failures = int_watcher(exe, args.machined, target, "reviver/cellappmgr/health_failures")
        restart_limit = watcher_value(
            exe, args.machined, target, "reviver/cellappmgr/restart_limit_reached"
        )
        manager_failures = int_watcher(
            exe, args.machined, target, "reviver/cellappmgr/manager_health_failures"
        )
        manager_timeouts = int_watcher(
            exe, args.machined, target, "reviver/cellappmgr/manager_health_timeouts"
        )
        launch_timeouts = int_watcher(
            exe, args.machined, target, "reviver/cellappmgr/launch_timeouts"
        )
        sent = int_watcher(exe, args.machined, target, "reviver/cellappmgr/heartbeat_sent")
        acks = int_watcher(exe, args.machined, target, "reviver/cellappmgr/heartbeat_acks")
        ack_age_ms = int_watcher(
            exe, args.machined, target, "reviver/cellappmgr/heartbeat_last_ack_age_ms"
        )
        snapshot_dirty = watcher_value(
            exe, args.machined, target, "reviver/cellappmgr/heartbeat_snapshot_dirty"
        )
        snapshot_stale = watcher_value(
            exe, args.machined, target, "reviver/cellappmgr/heartbeat_snapshot_save_stale"
        )
        snapshot_failures = int_watcher(
            exe, args.machined, target, "reviver/cellappmgr/heartbeat_snapshot_failures"
        )
        snapshot_status = watcher_value(
            exe, args.machined, target, "reviver/cellappmgr/heartbeat_snapshot_status"
        )
        heartbeat_failures = int_watcher(
            exe, args.machined, target, "reviver/cellappmgr/heartbeat_failures"
        )
        timeouts = int_watcher(
            exe, args.machined, target, "reviver/cellappmgr/heartbeat_timeouts"
        )
        if failures:
            raise RuntimeError(f"reviver health_failures={failures}")
        if status == "restart_limited":
            raise RuntimeError("reviver status=restart_limited")
        if restart_limit == "true":
            raise RuntimeError("reviver restart_limit_reached=true")
        if launch_pending == "true":
            return False, "waiting for Reviver launch_pending=false"
        if manager_failures:
            raise RuntimeError(f"reviver manager_health_failures={manager_failures}")
        if manager_timeouts:
            raise RuntimeError(f"reviver manager_health_timeouts={manager_timeouts}")
        if launch_timeouts:
            raise RuntimeError(f"reviver launch_timeouts={launch_timeouts}")
        if heartbeat_failures:
            raise RuntimeError(f"reviver heartbeat_failures={heartbeat_failures}")
        if timeouts:
            raise RuntimeError(f"reviver heartbeat_timeouts={timeouts}")
        if acks >= min_heartbeat_acks and ack_age_ms < 0:
            return False, "waiting for Reviver heartbeat last ack age"
        snapshot_ok, snapshot_detail, snapshot_hard_failure = (
            reviver_heartbeat_snapshot_health_detail(
                acks,
                min_heartbeat_acks,
                baseline_snapshot_failures,
                snapshot_dirty,
                snapshot_stale,
                snapshot_failures,
                snapshot_status,
            )
        )
        if snapshot_hard_failure:
            raise RuntimeError(f"reviver heartbeat snapshot unhealthy; {snapshot_detail}")
        return status == "active" and checks > 0 and snapshot_ok, (
            f"waiting for reviver health_checks and heartbeat_acks; "
            f"status={status} health_checks={checks} heartbeat_sent={sent} "
            f"heartbeat_last_ack_age_ms={ack_age_ms} {snapshot_detail}"
        )

    wait_until(args.timeout_sec, args.poll_sec, check)


def wait_for_reviver_retarget(
    args: argparse.Namespace, exe: Path, target: str, old_generation: int, new_pid: str
) -> int:
    def check():
        status = watcher_value(exe, args.machined, target, "reviver/cellappmgr/status")
        launch_pending = watcher_value(
            exe, args.machined, target, "reviver/cellappmgr/launch_pending"
        )
        active_pid = watcher_value(exe, args.machined, target, "reviver/cellappmgr/active_pid")
        generation = int_watcher(
            exe, args.machined, target, "reviver/cellappmgr/active_generation"
        )
        if status == "restart_limited":
            raise RuntimeError("reviver status=restart_limited")
        ok = (
            status == "active"
            and launch_pending == "false"
            and active_pid == new_pid
            and generation > old_generation
        )
        if ok:
            return True, generation
        return False, (
            f"waiting for Reviver retarget; status={status} active_pid={active_pid} "
            f"generation={generation}/{old_generation} launch_pending={launch_pending}"
        )

    return wait_until(args.timeout_sec, args.poll_sec, check)


def wait_for_restarted_mgr(
    args: argparse.Namespace, exe: Path, old_pid: str
) -> dict[str, str]:
    def check():
        managers = list_processes(exe, args.machined, "cellappmgr")
        matches = [proc for proc in managers if proc["name"] == args.cellappmgr_name]
        if not matches:
            return False, "waiting for CellAppMgr registration"
        manager = matches[0]
        if manager["pid"] == old_pid:
            return False, f"waiting for new CellAppMgr pid; still {old_pid}"
        return True, manager

    return wait_until(args.timeout_sec, args.poll_sec, check)


def wait_for_restore_convergence(
    args: argparse.Namespace, exe: Path, target: str, expected_topology: str | None = None
) -> str:
    snapshot_path = watcher_value(exe, args.machined, target, "cellappmgr/ha/snapshot_path")
    expected_backup_path = f"{snapshot_path}.bak"

    def check():
        restores = int_watcher(exe, args.machined, target, "cellappmgr/ha/snapshot_restores")
        fallbacks = int_watcher(
            exe, args.machined, target, "cellappmgr/ha/snapshot_fallback_restores"
        )
        save_failures, restore_failures, failures = snapshot_failure_counters(args, exe, target)
        restore_source = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_last_restore_source"
        )
        restore_path = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_last_restore_path"
        )
        restore_error = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_last_restore_error"
        )
        restore_primary_error = watcher_value(
            exe,
            args.machined,
            target,
            "cellappmgr/ha/snapshot_last_restore_primary_error",
        )
        restore_status = watcher_value(
            exe, args.machined, target, "cellappmgr/ha/snapshot_restore_status"
        )
        restore_status_fields = summary_fields(restore_status)
        restore_topology = None
        restore_topology_pending = 0
        restore_topology_pending_watcher = 0
        restore_topology_ok = (
            args.allow_empty_snapshot or args.allow_topology_change or expected_topology is None
        )
        restore_status_topology_ok = True
        if (
            expected_topology is not None
            and not args.allow_empty_snapshot
            and not args.allow_topology_change
        ):
            restore_topology, restore_topology_pending = read_restore_topology_fingerprint(
                args, exe, target
            )
            restore_topology_pending_watcher = int_watcher(
                exe,
                args.machined,
                target,
                "cellappmgr/ha/snapshot_last_restore_topology_pending_ack",
            )
            restore_topology_ok = (
                restore_topology == expected_topology
                and restore_topology_pending == 0
                and restore_topology_pending_watcher == 0
            )
            restore_status_topology_ok = (
                summary_has(restore_status_fields, "topology_present", "1")
                and summary_has(restore_status_fields, "topology_pending_ack", "0")
            )
        restore_attempt_age_ms = int_watcher(
            exe, args.machined, target, "cellappmgr/ha/snapshot_last_restore_attempt_age_ms"
        )
        restore_age_ms = int_watcher(
            exe, args.machined, target, "cellappmgr/ha/snapshot_last_restore_age_ms"
        )
        reattach_ok, reattach_status, has_stuck = read_reattach_health(
            args, exe, target, require_restored=not args.allow_empty_snapshot
        )
        mismatch = snapshot_failure_counter_mismatch(save_failures, restore_failures, failures)
        if mismatch:
            return False, f"{mismatch}; {reattach_status}"
        if failures:
            return False, (
                f"snapshot_failures={failures} save_failures={save_failures} "
                f"restore_failures={restore_failures} restore_source={restore_source} "
                f"restore_error={restore_error}; {reattach_status}"
            )
        if restore_error:
            return False, (
                f"snapshot restore_error={restore_error} restore_source={restore_source}; "
                f"{reattach_status}"
            )
        restore_source_ok = args.allow_empty_snapshot or restore_source in {"primary", "backup"}
        if restore_source == "backup" and fallbacks == 0:
            restore_source_ok = False
        restore_path_ok = args.allow_empty_snapshot or (
            (restore_source == "primary" and restore_path == snapshot_path)
            or (restore_source == "backup" and restore_path == expected_backup_path)
        )
        restore_status_ok = args.allow_empty_snapshot or (
            (
                restore_source == "primary"
                and summary_has(restore_status_fields, "state", "primary")
                and summary_has(restore_status_fields, "primary_error_present", "0")
                and summary_has(restore_status_fields, "primary_error_detail", "none")
                and summary_has(restore_status_fields, "error_detail", "none")
                and not restore_primary_error
            )
            or (
                restore_source == "backup"
                and summary_has(restore_status_fields, "state", "fallback")
                and summary_has(restore_status_fields, "error_detail", "none")
            )
        )
        if restore_source == "backup":
            restore_status_ok = (
                restore_status_ok
                and summary_has(restore_status_fields, "primary_error_present", "1")
                and restore_status_fields.get("primary_error_detail", "none") != "none"
                and bool(restore_primary_error)
            )
        restore_age_ok = args.allow_empty_snapshot or (
            restore_attempt_age_ms >= 0 and restore_age_ms >= 0
        )
        if has_stuck:
            raise RuntimeError(f"reattach stuck; {reattach_status}")
        ok = (
            (args.allow_empty_snapshot or restores > 0)
            and restore_source_ok
            and restore_path_ok
            and restore_status_ok
            and restore_age_ok
            and reattach_ok
            and restore_topology_ok
            and restore_status_topology_ok
        )
        topology_status = "skipped"
        if (
            expected_topology is not None
            and not args.allow_empty_snapshot
            and not args.allow_topology_change
        ):
            topology_status = "matched" if restore_topology_ok else "stale"
            if restore_topology_pending:
                topology_status = f"pending_ack={restore_topology_pending}"
            elif restore_topology_pending_watcher:
                topology_status = f"pending_ack={restore_topology_pending_watcher}"
        return ok, (
            f"restores={restores} fallback_restores={fallbacks} "
            f"restore_source={restore_source} restore_path={restore_path} "
            f"restore_primary_error={restore_primary_error} "
            f"restore_attempt_age_ms={restore_attempt_age_ms} "
            f"restore_age_ms={restore_age_ms} "
            f"restore_status={restore_status} "
            f"restore_topology={topology_status} "
            f"restore_status_topology={restore_status_topology_ok} "
            f"{reattach_status}"
        )

    return wait_until(args.timeout_sec, args.poll_sec, check)


def reviver_stability_snapshot(args: argparse.Namespace, exe: Path, target: str) -> dict[str, int]:
    paths = [
        "reviver/cellappmgr/active_generation",
        "reviver/cellappmgr/launch_count",
        "reviver/cellappmgr/launch_failures",
        "reviver/cellappmgr/launch_timeouts",
        "reviver/cellappmgr/restart_limit_hits",
        "reviver/cellappmgr/liveness_failures",
        "reviver/cellappmgr/health_failures",
        "reviver/cellappmgr/manager_health_failures",
        "reviver/cellappmgr/manager_health_timeouts",
        "reviver/cellappmgr/heartbeat_failures",
        "reviver/cellappmgr/heartbeat_timeouts",
        "reviver/cellappmgr/forced_terminations",
        "reviver/cellappmgr/registry_missing",
    ]
    return {path: int_watcher(exe, args.machined, target, path) for path in paths}


def wait_for_reviver_stability(
    args: argparse.Namespace,
    exe: Path,
    target: str,
    active_pid: str,
    active_generation: int,
    launch_count: int,
) -> str:
    if args.stability_sec <= 0:
        return "stability=skipped"

    baseline = reviver_stability_snapshot(args, exe, target)
    start_acks = int_watcher(exe, args.machined, target, "reviver/cellappmgr/heartbeat_acks")
    baseline_snapshot_failures = int_watcher(
        exe, args.machined, target, "reviver/cellappmgr/heartbeat_snapshot_failures"
    )
    deadline = time.monotonic() + args.stability_sec
    last_acks = start_acks
    while True:
        status = watcher_value(exe, args.machined, target, "reviver/cellappmgr/status")
        launch_pending = watcher_value(
            exe, args.machined, target, "reviver/cellappmgr/launch_pending"
        )
        restart_limit = watcher_value(
            exe, args.machined, target, "reviver/cellappmgr/restart_limit_reached"
        )
        current_pid = watcher_value(exe, args.machined, target, "reviver/cellappmgr/active_pid")
        current = reviver_stability_snapshot(args, exe, target)
        last_acks = int_watcher(exe, args.machined, target, "reviver/cellappmgr/heartbeat_acks")
        ack_age_ms = int_watcher(
            exe, args.machined, target, "reviver/cellappmgr/heartbeat_last_ack_age_ms"
        )
        snapshot_dirty = watcher_value(
            exe, args.machined, target, "reviver/cellappmgr/heartbeat_snapshot_dirty"
        )
        snapshot_stale = watcher_value(
            exe, args.machined, target, "reviver/cellappmgr/heartbeat_snapshot_save_stale"
        )
        snapshot_status = watcher_value(
            exe, args.machined, target, "reviver/cellappmgr/heartbeat_snapshot_status"
        )
        snapshot_ok, snapshot_detail, snapshot_hard_failure = (
            reviver_heartbeat_snapshot_health_detail(
                last_acks,
                start_acks,
                baseline_snapshot_failures,
                snapshot_dirty,
                snapshot_stale,
                int_watcher(
                    exe,
                    args.machined,
                    target,
                    "reviver/cellappmgr/heartbeat_snapshot_failures",
                ),
                snapshot_status,
            )
        )
        if status != "active":
            raise RuntimeError(f"reviver status changed during stability window: {status}")
        if launch_pending == "true":
            raise RuntimeError("reviver launch_pending=true during stability window")
        if restart_limit == "true":
            raise RuntimeError("reviver restart_limit_reached=true during stability window")
        if ack_age_ms < 0:
            raise RuntimeError("reviver heartbeat_last_ack_age_ms < 0 during stability window")
        if current_pid != active_pid:
            raise RuntimeError(
                f"reviver active_pid changed during stability window: {active_pid}->{current_pid}"
            )
        if current["reviver/cellappmgr/active_generation"] != active_generation:
            raise RuntimeError(
                "reviver active_generation changed during stability window: "
                f"{active_generation}->{current['reviver/cellappmgr/active_generation']}"
            )
        if current["reviver/cellappmgr/launch_count"] != launch_count:
            raise RuntimeError(
                "reviver launch_count changed during stability window: "
                f"{launch_count}->{current['reviver/cellappmgr/launch_count']}"
            )
        for path, value in baseline.items():
            if current[path] != value:
                raise RuntimeError(
                    f"{path} changed during stability window: {value}->{current[path]}"
                )
        if not snapshot_ok:
            reason = "unhealthy" if snapshot_hard_failure else "not ready"
            raise RuntimeError(
                f"reviver heartbeat snapshot {reason} during stability window; "
                f"{snapshot_detail}"
            )
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(args.poll_sec, remaining))

    return (
        f"stability={args.stability_sec:g}s heartbeat_acks={start_acks}->{last_acks} "
        f"heartbeat_last_ack_age_ms={ack_age_ms}"
    )


def wait_or_capture_reviver_stability(
    args: argparse.Namespace,
    exe: Path,
    target: str,
    active_pid: str,
    active_generation: int,
    launch_count: int,
    capture_unhealthy: bool = False,
) -> StabilityHealthReport:
    try:
        status = wait_for_reviver_stability(
            args, exe, target, active_pid, active_generation, launch_count
        )
        return StabilityHealthReport(True, status)
    except RuntimeError as ex:
        if not capture_unhealthy:
            raise
        return StabilityHealthReport(False, f"stability=failed detail={ex}")


def main() -> int:
    args = parse_args()
    exe = args.atlas_tool or default_atlas_tool(args.build)
    mode = "no-inject" if args.no_inject else "inject"
    results: list[HaCycleResult] = []
    reviver_failover_results: list[ReviverFailoverResult] = []
    summary_written = False
    failure_stage = "atlas_tool"
    if not exe.is_file():
        detail = f"atlas_tool not found: {exe}"
        if args.summary_json:
            try:
                write_summary_json(
                    args.summary_json,
                    mode,
                    results,
                    failed_run_current_payload(failure_stage, detail),
                    None,
                    summary_parameters(args),
                )
            except RuntimeError as ex:
                print(f"[verify_cellappmgr_ha] {ex}", file=sys.stderr)
        print(f"[verify_cellappmgr_ha] {detail}", file=sys.stderr)
        print("[verify_cellappmgr_ha] build it with tools/bin/build first", file=sys.stderr)
        return 1

    try:
        failure_stage = "list_cellapps"
        cellapps = list_processes(exe, args.machined, "cellapp")
        if len(cellapps) < args.min_cellapps:
            raise RuntimeError(
                f"need >= {args.min_cellapps} registered CellApps; got {len(cellapps)}"
            )
        failure_stage = "select_cellappmgr"
        manager = choose_process(
            list_processes(exe, args.machined, "cellappmgr"),
            args.cellappmgr_name,
            "cellappmgr",
        )
        target = f"cellappmgr:{manager['name']}"
        failure_stage = "list_revivers"
        revivers = list_processes(exe, args.machined, "reviver")
        if len(revivers) < args.min_revivers:
            raise RuntimeError(
                f"need >= {args.min_revivers} registered Revivers; got {len(revivers)}"
            )
        failure_stage = "select_reviver_leader"
        leadership = find_leader_reviver(exe, args.machined, revivers, args.reviver_name)
        failure_stage = "standby_reviver_health"
        standby_health_report = wait_or_capture_standby_reviver_health(
            args, exe, leadership, revivers, capture_unhealthy=args.no_inject
        )
        standby_health_status = standby_health_report.status
        standby_health_records = standby_health_report.records
        reviver = leadership.leader
        reviver_target = f"reviver:{reviver['name']}"
        launch_count = int_watcher(
            exe, args.machined, reviver_target, "reviver/cellappmgr/launch_count"
        )

        failure_stage = "reviver_health"
        wait_for_reviver_health(args, exe, reviver_target)
        leader_lock_summary: dict | None = None
        if args.check_leader_lock_mode:
            failure_stage = "leader_lock_mode"
            llh = read_cellappmgr_leader_lock_health(
                exe, args.machined, reviver_target, args.check_leader_lock_mode)
            if not llh.healthy:
                raise RuntimeError(f"leader lock mode check failed: {llh.detail}")
            leader_lock_summary = {
                "mode": llh.mode,
                "leader_active": llh.leader_active,
                "holder_id": llh.holder_id,
                "acquire_count": llh.acquire_count,
                "lease_renew_count": llh.lease_renew_count,
                "lease_failure_count": llh.lease_failure_count,
                "healthy": llh.healthy,
            }
        failure_stage = "reviver_baseline"
        active_generation = int_watcher(
            exe, args.machined, reviver_target, "reviver/cellappmgr/active_generation"
        )
        heartbeat_acks = int_watcher(
            exe, args.machined, reviver_target, "reviver/cellappmgr/heartbeat_acks"
        )
        failure_stage = "reviver_output_path"
        output_path = require_reviver_output_path(args, exe, reviver_target)
        failure_stage = "initial_snapshot_save"
        wait_for_snapshot_save(args, exe, target)
        failure_stage = "initial_topology"
        current_topology, current_topology_status = wait_for_topology_quiescence(
            args, exe, target
        )
        failure_stage = "initial_snapshot_topology"
        current_snapshot_status = wait_for_snapshot_save(
            args, exe, target, expected_topology=current_topology
        )
        print(
            f"[verify_cellappmgr_ha] target={manager['name']} pid={manager['pid']} "
            f"reviver={reviver['name']} launches={launch_count} "
            f"generation={active_generation} {leadership.status}; {standby_health_status}"
        )
        if args.verify_reviver_failover:
            for failover_cycle in range(1, args.reviver_failover_cycles + 1):
                failure_stage = "reviver_failover"
                revivers = list_processes(exe, args.machined, "reviver")
                requested_name = args.reviver_name if failover_cycle == 1 else ""
                leadership = find_leader_reviver(
                    exe, args.machined, revivers, requested_name
                )
                leadership, reviver_failover_result = wait_for_reviver_leader_failover(
                    args,
                    exe,
                    leadership,
                    manager,
                    revivers,
                    failover_cycle,
                    capture_unhealthy=True,
                )
                reviver_failover_results.append(reviver_failover_result)
                failover_result = "PASS" if reviver_failover_result.healthy else "FAIL"
                print(
                    f"[verify_cellappmgr_ha] {failover_result} reviver_failover "
                    f"cycle={failover_cycle}/{args.reviver_failover_cycles} "
                    f"{reviver_failover_result.old_leader}->"
                    f"{reviver_failover_result.new_leader} "
                    f"elapsed_ms={reviver_failover_result.failover_elapsed_ms} "
                    f"surviving_revivers={reviver_failover_result.surviving_revivers} "
                    f"standby_after={reviver_failover_result.standby_after}; "
                    f"{reviver_failover_result.status}"
                )
                if not reviver_failover_result.healthy:
                    break
            if reviver_failover_results and not reviver_failover_results[-1].healthy:
                parameters = summary_parameters(args)
                current = {
                    "reviver_leadership_status": leadership.status,
                    "reviver_failovers": [
                        result._asdict() for result in reviver_failover_results
                    ],
                }
                payload = build_summary_payload(
                    mode,
                    [],
                    current,
                    reviver_failover_results,
                    parameters,
                )
                if args.summary_json:
                    payload = write_summary_json(
                        args.summary_json,
                        mode,
                        [],
                        current,
                        reviver_failover_results,
                        parameters,
                    )
                    summary_written = True
                validate_summary_gates(payload)
                return 0
            reviver = leadership.leader
            reviver_target = f"reviver:{reviver['name']}"
            failure_stage = "post_failover_reviver_baseline"
            launch_count = int_watcher(
                exe, args.machined, reviver_target, "reviver/cellappmgr/launch_count"
            )
            active_generation = int_watcher(
                exe, args.machined, reviver_target, "reviver/cellappmgr/active_generation"
            )
            heartbeat_acks = int_watcher(
                exe, args.machined, reviver_target, "reviver/cellappmgr/heartbeat_acks"
            )
            output_path = require_reviver_output_path(args, exe, reviver_target)
            failure_stage = "post_failover_standby_health"
            revivers = list_processes(exe, args.machined, "reviver")
            standby_health_report = wait_or_capture_standby_reviver_health(
                args, exe, leadership, revivers, capture_unhealthy=args.no_inject
            )
            standby_health_status = standby_health_report.status
            standby_health_records = standby_health_report.records
        if args.no_inject:
            failure_stage = "no_inject_active_pid"
            active_pid = watcher_value(
                exe, args.machined, reviver_target, "reviver/cellappmgr/active_pid"
            )
            if active_pid != manager["pid"]:
                raise RuntimeError(
                    f"Reviver active_pid does not match CellAppMgr: "
                    f"{active_pid}!={manager['pid']}"
                )
            failure_stage = "no_inject_stability"
            stability_report = wait_or_capture_reviver_stability(
                args,
                exe,
                reviver_target,
                active_pid,
                active_generation,
                launch_count,
                capture_unhealthy=True,
            )
            stability_status = stability_report.status
            failure_stage = "no_inject_recovery"
            recovery_report = wait_or_capture_recovery_health(
                args, exe, target, require_restored=False, capture_unhealthy=True
            )
            reattach_status = recovery_report.status
            load_report = recovery_report.payload["load_report"]
            output_status = (
                "output_log=skipped" if output_path is None else f"output_log={output_path}"
            )
            current = {
                "pid": active_pid,
                "generation": active_generation,
                "launch_count": launch_count,
                "snapshot_status": current_snapshot_status,
                "topology_status": current_topology_status,
                "reattach_status": reattach_status,
                "recovery_status": recovery_report.status,
                "recovery": recovery_report.payload,
                "load_report_status": load_report["detail"],
                "load_report": load_report,
                "output_status": output_status,
                "stability_status": stability_status,
                "stability_healthy": stability_report.ok,
                "reviver_leadership_status": leadership.status,
                "reviver_standby_health_status": standby_health_status,
                "reviver_topology": build_reviver_topology_payload(
                    leadership,
                    standby_health_status,
                    standby_health_report.ok,
                    standby_health=standby_health_records,
                ),
            }
            if reviver_failover_results:
                current["reviver_failovers"] = [
                    result._asdict() for result in reviver_failover_results
                ]
            parameters = summary_parameters(args)
            payload = build_summary_payload(
                "no-inject",
                [],
                current,
                reviver_failover_results or None,
                parameters,
            )
            if args.summary_json:
                payload = write_summary_json(
                    args.summary_json,
                    "no-inject",
                    [],
                    current,
                    reviver_failover_results or None,
                    parameters,
                )
                summary_written = True
            validate_summary_gates(payload)
            print(
                f"[verify_cellappmgr_ha] PASS current HA watchers are reachable; "
                f"{leadership.status}; {current_snapshot_status}; {current_topology_status}; "
                f"{reattach_status}; {output_status}; {stability_status}"
            )
            return 0

        for cycle in range(1, args.cycles + 1):
            failure_stage = "cellappmgr_takeover_cycle"
            cycle_started_at = time.monotonic()
            old_pid = manager["pid"]
            try:
                stage = "pre_snapshot"
                wait_for_snapshot_save(args, exe, target)
                stage = "pre_topology"
                cycle_topology, pre_topology_status = wait_for_topology_quiescence(
                    args, exe, target
                )
                stage = "pre_snapshot_topology"
                pre_snapshot_status = wait_for_snapshot_save(
                    args, exe, target, expected_topology=cycle_topology
                )
                takeover_start = time.monotonic()
                stage = "shutdown"
                shutdown_process(exe, args.machined, target, args.shutdown_reason)
                stage = "restart"
                restarted = wait_for_restarted_mgr(args, exe, old_pid)
                manager_restart_ms = elapsed_ms(takeover_start)
                restored_target = f"cellappmgr:{restarted['name']}"
                stage = "reviver_retarget"
                new_generation = wait_for_reviver_retarget(
                    args, exe, reviver_target, active_generation, restarted["pid"]
                )
                reviver_retarget_ms = elapsed_ms(takeover_start)
                stage = "reviver_health"
                wait_for_reviver_health(args, exe, reviver_target, heartbeat_acks + 1)
                stage = "restore_convergence"
                status = wait_for_restore_convergence(
                    args, exe, restored_target, expected_topology=cycle_topology
                )
                restore_converged_ms = elapsed_ms(takeover_start)
                stage = "restored_topology"
                _, restored_topology_status = wait_for_topology_quiescence(
                    args, exe, restored_target, cycle_topology
                )
                stage = "post_restore_snapshot"
                post_restore_snapshot_status = wait_for_snapshot_save(
                    args,
                    exe,
                    restored_target,
                    require_after_restore=True,
                    expected_topology=cycle_topology,
                )
                takeover_elapsed_ms = elapsed_ms(takeover_start)
                stage = "output_log"
                output_status = wait_for_revived_output_log(
                    args, output_path, restarted["pid"]
                )
                stage = "launch_count"
                new_launch_count = int_watcher(
                    exe, args.machined, reviver_target, "reviver/cellappmgr/launch_count"
                )
                if new_launch_count <= launch_count:
                    raise RuntimeError(
                        f"Reviver launch_count did not increase: "
                        f"{launch_count}->{new_launch_count}"
                    )
                stability_report = wait_or_capture_reviver_stability(
                    args,
                    exe,
                    reviver_target,
                    restarted["pid"],
                    new_generation,
                    new_launch_count,
                    capture_unhealthy=True,
                )
                stability_status = stability_report.status
                recovery_report = wait_or_capture_recovery_health(
                    args,
                    exe,
                    restored_target,
                    require_restored=not args.allow_empty_snapshot,
                    capture_unhealthy=True,
                )
                stability_status = f"{stability_status}; {recovery_report.status}"
                results.append(
                    HaCycleResult(
                        cycle=cycle,
                        old_pid=old_pid,
                        new_pid=restarted["pid"],
                        generation_before=active_generation,
                        generation_after=new_generation,
                        launch_count_before=launch_count,
                        launch_count_after=new_launch_count,
                        restore_status=status,
                        pre_topology_status=pre_topology_status,
                        pre_snapshot_status=pre_snapshot_status,
                        restored_topology_status=restored_topology_status,
                        post_restore_snapshot_status=post_restore_snapshot_status,
                        output_status=output_status,
                        stability_status=stability_status,
                        manager_restart_ms=manager_restart_ms,
                        reviver_retarget_ms=reviver_retarget_ms,
                        restore_converged_ms=restore_converged_ms,
                        takeover_elapsed_ms=takeover_elapsed_ms,
                        recovery_status=recovery_report.status,
                        recovery=recovery_report.payload,
                        load_report_status=recovery_report.payload["load_report"]["detail"],
                        load_report=recovery_report.payload["load_report"],
                        stability_healthy=stability_report.ok,
                    )
                )
            except RuntimeError as ex:
                failed_cycle = failed_ha_cycle_result(
                    cycle,
                    old_pid,
                    active_generation,
                    launch_count,
                    cycle_started_at,
                    stage,
                    str(ex),
                )
                results.append(failed_cycle)
                parameters = summary_parameters(args)
                current = {"cycle_failure": failed_cycle._asdict()}
                if reviver_failover_results:
                    current["reviver_failovers"] = [
                        result._asdict() for result in reviver_failover_results
                    ]
                payload = build_summary_payload(
                    "inject",
                    results,
                    current,
                    reviver_failover_results or None,
                    parameters,
                )
                if args.summary_json:
                    payload = write_summary_json(
                        args.summary_json,
                        "inject",
                        results,
                        current,
                        reviver_failover_results or None,
                        parameters,
                    )
                    summary_written = True
                validate_summary_gates(payload)
                return 0
            cycle_result = "PASS" if recovery_report.ok and stability_report.ok else "FAIL"
            print(
                f"[verify_cellappmgr_ha] {cycle_result} cycle={cycle}/{args.cycles} "
                f"old_pid={old_pid} new_pid={restarted['pid']} "
                f"manager_restart_ms={manager_restart_ms} "
                f"reviver_retarget_ms={reviver_retarget_ms} "
                f"restore_converged_ms={restore_converged_ms} "
                f"takeover_elapsed_ms={takeover_elapsed_ms} "
                f"launches={new_launch_count} generation={active_generation}->{new_generation}; "
                f"{status}; {pre_topology_status}; {pre_snapshot_status}; "
                f"{restored_topology_status}; {post_restore_snapshot_status}; "
                f"{output_status}; {stability_status}"
            )
            manager = restarted
            target = restored_target
            active_generation = new_generation
            launch_count = new_launch_count
            if not stability_report.ok or not recovery_report.ok:
                break
            failure_stage = "heartbeat_baseline"
            heartbeat_acks = int_watcher(
                exe, args.machined, reviver_target, "reviver/cellappmgr/heartbeat_acks"
            )
        failure_stage = "final_recovery"
        recovery_report = wait_or_capture_recovery_health(
            args,
            exe,
            target,
            require_restored=not args.allow_empty_snapshot,
            capture_unhealthy=True,
        )
        load_report = recovery_report.payload["load_report"]
        requested_reviver_name = "" if reviver_failover_results else args.reviver_name
        failure_stage = "final_reviver_topology"
        leadership, standby_health_report, _ = refresh_reviver_topology(
            args, exe, requested_reviver_name, capture_unhealthy=True
        )
        standby_health_status = standby_health_report.status
        standby_health_records = standby_health_report.records
        current = {
            "recovery_status": recovery_report.status,
            "recovery": recovery_report.payload,
            "load_report_status": load_report["detail"],
            "load_report": load_report,
            "reviver_leadership_status": leadership.status,
            "reviver_standby_health_status": standby_health_status,
            "reviver_topology": build_reviver_topology_payload(
                leadership,
                standby_health_status,
                standby_health_report.ok,
                standby_health=standby_health_records,
            ),
        }
        if leader_lock_summary is not None:
            current["leader_lock"] = leader_lock_summary
        if reviver_failover_results:
            current["reviver_failovers"] = [
                result._asdict() for result in reviver_failover_results
            ]
        parameters = summary_parameters(args)
        payload = build_summary_payload(
            "inject",
            results,
            current,
            reviver_failover_results or None,
            parameters,
        )
        if args.summary_json:
            payload = write_summary_json(
                args.summary_json,
                "inject",
                results,
                current,
                reviver_failover_results or None,
                parameters,
            )
            summary_written = True
        validate_summary_gates(payload)
        print(f"[verify_cellappmgr_ha] PASS cycles={args.cycles} {leadership.status}")
        return 0
    except RuntimeError as ex:
        if args.summary_json and not summary_written:
            current = failed_run_current_payload(failure_stage, str(ex))
            if reviver_failover_results:
                current["reviver_failovers"] = [
                    result._asdict() for result in reviver_failover_results
                ]
            try:
                write_summary_json(
                    args.summary_json,
                    mode,
                    results,
                    current,
                    reviver_failover_results or None,
                    summary_parameters(args),
                )
            except RuntimeError as summary_ex:
                print(f"[verify_cellappmgr_ha] {summary_ex}", file=sys.stderr)
        print(f"[verify_cellappmgr_ha] {ex}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
