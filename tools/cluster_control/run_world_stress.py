#!/usr/bin/env python3
"""Bring up the full Atlas cluster + optionally drive it with world_stress."""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable, NoReturn

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common.paths import REPO_ROOT, dotnet_tfm_dir, resolve_repo_root  # noqa: F401


PROCESS_ROW_RE = re.compile(
    r"^(?P<type>\w+)\s+(?P<name>\S+)\s+(?P<addr>\S+)\s+(?P<pid>\d+)\s+"
    r"(?P<load>[\d.]+)%"
)

BASEAPP_MOVEMENT_WATCHERS: list[tuple[str, str]] = [
    ("in", "movement/input_packets_total"),
    ("fwd", "movement/input_forwarded_total"),
    ("drop", "movement/input_dropped_total"),
    ("rate", "movement/input_rate_limited_total"),
    ("invalid", "movement/input_invalid_dropped_total"),
    ("stale", "movement/input_stale_dropped_total"),
    ("seqgap", "movement/input_seq_gap_dropped_total"),
    ("ack", "movement/ack_sent_total"),
    ("ackstale", "movement/ack_stale_dropped_total"),
    ("c1", "movement/correction_tier1_total"),
    ("c2", "movement/correction_tier2_total"),
    ("snap", "movement/correction_snap_total"),
    ("sus", "movement/correction_suspicious_total"),
    ("rpt", "movement/correction_report_total"),
    ("rptdrop", "movement/correction_report_dropped_total"),
]

CELLAPP_MOVEMENT_WATCHERS: list[tuple[str, str]] = [
    ("in", "movement/input_packets_total"),
    ("frames", "movement/input_frames_enqueued_total"),
    ("drop", "movement/input_dropped_total"),
    ("rate", "movement/input_rate_limited_total"),
    ("invalid", "movement/input_invalid_dropped_total"),
    ("stale", "movement/input_stale_dropped_total"),
    ("seqgap", "movement/input_seq_gap_dropped_total"),
    ("overflow", "movement/input_overflow_dropped_total"),
    ("depth", "movement/input_queue_depth"),
    ("sim", "movement/frames_simulated_total"),
    ("hist", "movement/position_history_samples_recorded_total"),
    ("ack", "movement/ack_sent_total"),
    ("p95us", "movement/step_time_us_p95"),
]


def pct_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be an integer in [0, 100]") from exc
    if parsed < 0 or parsed > 100:
        raise argparse.ArgumentTypeError("must be an integer in [0, 100]")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Bring up a local Atlas cluster (incl. CellApp) and run world_stress."
    )
    parser.add_argument("--build-dir", default="build/debug")
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--machined-host", default="127.0.0.1")
    parser.add_argument("--machined-port", type=int, default=20018)
    parser.add_argument("--login-port", type=int, default=20013)
    parser.add_argument("--baseapp-internal-port", type=int, default=21001)
    parser.add_argument("--baseapp-external-port", type=int, default=22001)
    parser.add_argument("--baseapp-count", type=int, default=1)
    parser.add_argument("--baseapp-internal-port-stride", type=int, default=1)
    parser.add_argument("--baseapp-external-port-stride", type=int, default=1)
    parser.add_argument("--baseappmgr-port", type=int, default=23001)
    parser.add_argument("--dbapp-port", type=int, default=24001)
    parser.add_argument("--cellappmgr-port", type=int, default=25001)
    parser.add_argument("--reviver-port", type=int, default=27001)
    parser.add_argument("--cellapp-internal-port", type=int, default=26001)
    parser.add_argument("--cellapp-count", type=int, default=1)
    parser.add_argument("--cellapp-internal-port-stride", type=int, default=1)
    parser.add_argument(
        "--with-cellappmgr-reviver",
        action="store_true",
        help="Start atlas_reviver to supervise CellAppMgr.",
    )
    parser.add_argument(
        "--cellappmgr-reviver-count",
        type=int,
        default=1,
        help="Number of Reviver processes to start; >1 shares the leader lock as standby.",
    )
    parser.add_argument(
        "--reviver-port-stride",
        type=int,
        default=1,
        help="Internal port stride for additional Reviver processes.",
    )
    parser.add_argument(
        "--cellappmgr-snapshot-path",
        default=None,
        help="CellAppMgr HA snapshot path. Defaults under .tmp when Reviver is enabled.",
    )
    parser.add_argument("--cellappmgr-snapshot-interval-ms", type=int, default=250)
    parser.add_argument(
        "--reviver-leader-lock-path",
        default=None,
        help="Reviver leader lock path. Defaults under .tmp when Reviver is enabled.",
    )
    parser.add_argument("--reviver-restart-delay-ms", type=int, default=1000)
    parser.add_argument("--reviver-heartbeat-timeout-ms", type=int, default=4000)
    parser.add_argument("--reviver-max-restarts", type=int, default=3)
    parser.add_argument(
        "--reviver-leader-lock-mode",
        choices=("local", "machined"),
        default="local",
        help="Reviver leader lock backend: 'local' file lock (default, single host) or "
             "'machined' distributed lease (cross-host capable).",
    )
    parser.add_argument("--reviver-leader-lock-ttl-ms", type=int, default=8000)
    parser.add_argument("--reviver-leader-lock-renew-ms", type=int, default=3000)
    parser.add_argument("--clients", type=int, default=0)
    parser.add_argument("--account-pool", type=int, default=0)
    parser.add_argument("--account-index-base", type=int, default=0)
    parser.add_argument("--ramp-per-sec", type=int, default=100)
    parser.add_argument("--duration-sec", type=int, default=30)
    parser.add_argument("--shortline-pct", type=int, default=0)
    parser.add_argument("--shortline-min-ms", type=int, default=1000)
    parser.add_argument("--shortline-max-ms", type=int, default=5000)
    parser.add_argument("--rpc-rate-hz", type=int, default=2)
    parser.add_argument("--move-rate-hz", type=int, default=10)
    parser.add_argument("--move-mode", choices=("report-pos", "input"), default="report-pos")
    parser.add_argument("--movement-input-redundant-frames", type=int, choices=range(1, 4),
                        default=1)
    parser.add_argument("--movement-input-drop-pct", type=pct_int, default=0)
    parser.add_argument("--movement-input-reorder-pct", type=pct_int, default=0)
    parser.add_argument(
        "--movement-verify",
        action="store_true",
        help="Fail if BaseApp/CellApp movement watchers do not prove the input path.",
    )
    parser.add_argument("--spread-radius", type=float, default=0.0)
    parser.add_argument("--space-count", type=int, default=1)
    parser.add_argument("--hold-min-ms", type=int, default=30000)
    parser.add_argument("--hold-max-ms", type=int, default=60000)
    parser.add_argument("--retry-delay-ms", type=int, default=1000)
    parser.add_argument("--connect-timeout-ms", type=int, default=20000)
    parser.add_argument("--account-type-id", type=int, default=1)
    parser.add_argument("--source-ip", action="append", default=[])
    parser.add_argument("--source-ip-file")
    parser.add_argument("--local-workers", type=int, default=1)
    parser.add_argument("--worker-index", type=int, default=0)
    parser.add_argument("--worker-count", type=int, default=1)
    parser.add_argument("--login-rate-limit-per-ip", type=int, default=5)
    parser.add_argument("--login-rate-limit-global", type=int, default=1000)
    parser.add_argument("--login-rate-limit-window-sec", type=int, default=60)
    parser.add_argument("--login-rate-limit-trusted-cidr", action="append", default=[])
    parser.add_argument(
        "--password-hash",
        default="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    )
    parser.add_argument("--keep-cluster", action="store_true")
    parser.add_argument(
        "--load-refresh-sec",
        type=float,
        default=2.0,
        help="Seconds between keep-cluster process-list refreshes; 0 disables refresh.",
    )
    parser.add_argument("--base-assembly", default=None,
                        help="Override BaseApp script DLL (default: "
                             "bin/<build>/Atlas.StressTest.Base.dll)")
    parser.add_argument("--cell-assembly", default=None,
                        help="Override CellApp script DLL (default: "
                             "bin/<build>/Atlas.StressTest.Cell.dll)")
    parser.add_argument("--verbose-failures", action="store_true")
    parser.add_argument(
        "--allow-audit-violations",
        action="store_true",
        help="Don't fail when cellapp reports invariant violations (leave fan-out "
             "missed, stale enter-pending).",
    )
    parser.add_argument("--walk-step-meters", type=float, default=None,
                        help="Per-tick walk step magnitude.")
    parser.add_argument("--walk-range-meters", type=float, default=None,
                        help="Half-width of random-walk box around each session's spawn.")
    parser.add_argument("--teleport-pct", type=int, default=None,
                        help="Pct of ReportPos replaced by jumps; exercises RangeTrigger paths.")

    # cellapp is the authoritative spatial tick; baseapp 1.5-2x cellapp; managers 10 Hz.
    parser.add_argument("--cellapp-update-hertz", type=int, default=15)
    parser.add_argument("--baseapp-update-hertz", type=int, default=15)
    parser.add_argument("--loginapp-update-hertz", type=int, default=20)
    parser.add_argument("--baseappmgr-update-hertz", type=int, default=10)
    parser.add_argument("--cellappmgr-update-hertz", type=int, default=10)
    parser.add_argument("--reviver-update-hertz", type=int, default=10)
    parser.add_argument("--dbapp-update-hertz", type=int, default=10)
    parser.add_argument(
        "--capture-dir", default=None,
        help="Save per-process Tracy captures (.tracy) to this directory. "
             "Filenames include git short hash and timestamp. "
             "Requires tracy-capture.exe in bin/<build>/.",
    )
    parser.add_argument(
        "--capture-procs",
        default="loginapp,dbapp,baseappmgr,baseapp,cellappmgr,cellapp",
        help="Comma-separated server process names to capture (default: all six).",
    )

    # Real atlas_client.exe subprocesses; see docs/stress_test/script_client_smoke.md.
    parser.add_argument("--script-clients", type=int, default=0,
                        help="Spawn N real atlas_client.exe subprocesses "
                             "alongside virtual clients (script_client_smoke.md)")
    parser.add_argument("--client-exe", default=None,
                        help="Path to atlas_client.exe. Defaults to "
                             "bin/<build>/atlas_client.exe")
    parser.add_argument("--client-assembly", default=None,
                        help="Path to Atlas.ClientSample.dll. Defaults to "
                             "bin/<build>/Atlas.ClientSample.dll")
    parser.add_argument("--client-runtime-config", default=None,
                        help="Optional hostfxr *.runtimeconfig.json forwarded to each child")
    parser.add_argument("--script-username-prefix", default="script_user_")
    parser.add_argument("--script-verify", action="store_true",
                        help="Fail if a script child misses OnInit, movement input, "
                             "movement ack, or correction report")
    parser.add_argument("--client-drop-inbound-ms", nargs=2, type=int,
                        metavar=("START", "DURATION"), default=None,
                        help="Forward atlas_client --drop-inbound-ms (app-level drop of "
                             "state-channel messages; script_client_smoke.md scenario 2)")
    parser.add_argument("--client-drop-transport-ms", nargs=2, type=int,
                        metavar=("START", "DURATION"), default=None,
                        help="Forward atlas_client --drop-transport-ms (RUDP-layer drop; "
                             "reliable retransmit recovers; script_client_smoke.md scenario 3)")
    parser.add_argument("--client-transport-impairment-ms", nargs=2, type=int,
                        metavar=("LATENCY", "LOSS_PERMYRIAD"), default=None,
                        help="Forward atlas_client --impair-transport-ms. LATENCY is "
                             "one-way per direction; 200 loss-permyriad means 2%%")
    return parser.parse_args()


def log(message: str, *, stream: object = sys.stdout) -> None:
    print(message, file=stream, flush=True)


def fail(message: str) -> NoReturn:
    raise RuntimeError(message)


def print_registered_processes(atlas_tool: Path, machined_address: str, repo_root: Path) -> None:
    subprocess.run(
        [str(atlas_tool), "--machined", machined_address, "list"],
        cwd=repo_root,
        check=False,
        env=_dll_env(atlas_tool),
    )


def run_atlas_tool_query(
    atlas_tool: Path,
    machined_address: str,
    repo_root: Path,
    *args: str,
) -> subprocess.CompletedProcess[str] | None:
    try:
        return subprocess.run(
            [str(atlas_tool), "--machined", machined_address, *args],
            capture_output=True,
            text=True,
            timeout=10,
            cwd=repo_root,
            env=_dll_env(atlas_tool),
        )
    except (OSError, subprocess.TimeoutExpired):
        return None


def list_registered_process_names(
    atlas_tool: Path,
    machined_address: str,
    repo_root: Path,
    process_type: str,
) -> list[str]:
    result = run_atlas_tool_query(
        atlas_tool,
        machined_address,
        repo_root,
        "list",
        process_type,
    )
    if result is None or result.returncode != 0:
        return []
    names: list[str] = []
    for raw_line in result.stdout.splitlines():
        match = PROCESS_ROW_RE.match(raw_line.strip())
        if match and match.group("type") == process_type:
            names.append(match.group("name"))
    return names


def query_watcher_value(
    atlas_tool: Path,
    machined_address: str,
    repo_root: Path,
    target: str,
    path: str,
) -> str:
    result = run_atlas_tool_query(
        atlas_tool,
        machined_address,
        repo_root,
        "watch",
        target,
        path,
    )
    if result is None or result.returncode != 0:
        return "?"
    parts = result.stdout.strip().split(maxsplit=1)
    return parts[1] if len(parts) == 2 else "?"


def collect_movement_watcher_rows(
    *,
    atlas_tool: Path,
    machined_address: str,
    repo_root: Path,
    process_type: str,
    watchers: list[tuple[str, str]],
) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    names = list_registered_process_names(
        atlas_tool,
        machined_address,
        repo_root,
        process_type,
    )
    for name in names:
        row = {"name": name}
        target = f"{process_type}:{name}"
        for label, path in watchers:
            row[label] = query_watcher_value(
                atlas_tool,
                machined_address,
                repo_root,
                target,
                path,
            )
        rows.append(row)
    return rows


def print_watcher_rows(headers: list[str], rows: list[dict[str, str]], indent: str) -> None:
    widths = {h: max(len(h), *(len(row[h]) for row in rows)) for h in headers}
    log(indent + "  ".join(f"{h:<{widths[h]}}" for h in headers))
    log(indent + "  ".join("-" * widths[h] for h in headers))
    for row in rows:
        log(indent + "  ".join(f"{row[h]:<{widths[h]}}" for h in headers))


def print_movement_watcher_table(
    *,
    atlas_tool: Path,
    machined_address: str,
    repo_root: Path,
    title: str,
    process_type: str,
    watchers: list[tuple[str, str]],
) -> list[dict[str, str]]:
    rows = collect_movement_watcher_rows(
        atlas_tool=atlas_tool,
        machined_address=machined_address,
        repo_root=repo_root,
        process_type=process_type,
        watchers=watchers,
    )
    if not rows:
        log(f"  {title}: (none)")
        return rows

    log(f"  {title}:")
    print_watcher_rows(["name", *[label for label, _ in watchers]], rows, "    ")
    return rows


def print_movement_watcher_summary(
    atlas_tool: Path,
    machined_address: str,
    repo_root: Path,
) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    log("Movement watcher summary:")
    base_rows = print_movement_watcher_table(
        atlas_tool=atlas_tool,
        machined_address=machined_address,
        repo_root=repo_root,
        title="BaseApp",
        process_type="baseapp",
        watchers=BASEAPP_MOVEMENT_WATCHERS,
    )
    cell_rows = print_movement_watcher_table(
        atlas_tool=atlas_tool,
        machined_address=machined_address,
        repo_root=repo_root,
        title="CellApp",
        process_type="cellapp",
        watchers=CELLAPP_MOVEMENT_WATCHERS,
    )
    return base_rows, cell_rows


def watcher_total(rows: list[dict[str, str]], label: str) -> int | None:
    if not rows:
        return None
    total = 0
    for row in rows:
        try:
            total += int(row[label])
        except (KeyError, ValueError):
            return None
    return total


def verify_positive_watcher(
    errors: list[str],
    title: str,
    rows: list[dict[str, str]],
    label: str,
) -> None:
    total = watcher_total(rows, label)
    if total is None:
        errors.append(f"{title}.{label}: missing or unreadable")
    elif total <= 0:
        errors.append(f"{title}.{label}: expected > 0, got {total}")


def verify_zero_watcher(
    errors: list[str],
    title: str,
    rows: list[dict[str, str]],
    label: str,
) -> None:
    total = watcher_total(rows, label)
    if total is None:
        errors.append(f"{title}.{label}: missing or unreadable")
    elif total != 0:
        errors.append(f"{title}.{label}: expected 0, got {total}")


def verify_movement_watchers(
    base_rows: list[dict[str, str]],
    cell_rows: list[dict[str, str]],
    *,
    require_reports: bool,
) -> list[str]:
    errors: list[str] = []
    base_positive = ["in", "fwd", "ack"]
    if require_reports:
        base_positive.append("rpt")
    for label in base_positive:
        verify_positive_watcher(errors, "BaseApp", base_rows, label)
    for label in ("rate", "invalid", "seqgap", "ackstale", "rptdrop"):
        verify_zero_watcher(errors, "BaseApp", base_rows, label)
    for label in ("in", "frames", "sim", "hist", "ack"):
        verify_positive_watcher(errors, "CellApp", cell_rows, label)
    for label in ("rate", "invalid", "seqgap", "overflow"):
        verify_zero_watcher(errors, "CellApp", cell_rows, label)
    return errors


def scan_cellapp_audits(log_dir: Path) -> list[str]:
    """Grep cellapp logs for invariant-audit messages so a regression fails the run."""
    needles = ("leave fan-out missed", "stale enter-pending")
    found: list[str] = []
    candidates: list[Path] = []
    for ext in ("stderr.log", "stdout.log"):
        candidates.append(log_dir / f"cellapp.{ext}")
    for path in candidates:
        if not path.exists():
            continue
        try:
            for line in path.read_text(errors="replace").splitlines():
                if any(n in line for n in needles):
                    found.append(line.strip())
        except OSError:
            continue
    return found


def assert_file_exists(path: Path, label: str) -> None:
    if not path.exists():
        fail(f"{label} not found: {path}")


def resolve_optional_path(repo_root: Path, value: str | None) -> Path | None:
    if not value:
        return None
    path = Path(value)
    if not path.is_absolute():
        path = repo_root / path
    return path


def parse_source_ip_file(path: Path) -> list[str]:
    values: list[str] = []
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        values.append(line)
    return values


def collect_source_ips(args: argparse.Namespace) -> list[str]:
    source_ips = list(args.source_ip)
    if args.source_ip_file:
        source_ips.extend(parse_source_ip_file(Path(args.source_ip_file)))
    return source_ips


def split_range(total: int, parts: int, index: int) -> tuple[int, int]:
    base, remainder = divmod(total, parts)
    start = index * base + min(index, remainder)
    size = base + (1 if index < remainder else 0)
    return start, size


def assign_worker_source_ips(
    source_ips: list[str], worker_index: int, worker_count: int
) -> list[str]:
    if not source_ips:
        return []

    assigned = source_ips[worker_index::worker_count]
    if assigned:
        return assigned
    return [source_ips[worker_index % len(source_ips)]]


def build_worker_plan(args: argparse.Namespace, source_ips: list[str]) -> list[dict[str, object]]:
    if args.baseapp_count <= 0:
        fail("--baseapp-count must be >= 1")
    if args.cellapp_count <= 0:
        fail("--cellapp-count must be >= 1")
    if args.baseapp_internal_port_stride <= 0:
        fail("--baseapp-internal-port-stride must be >= 1")
    if args.baseapp_external_port_stride <= 0:
        fail("--baseapp-external-port-stride must be >= 1")
    if args.cellapp_internal_port_stride <= 0:
        fail("--cellapp-internal-port-stride must be >= 1")
    if args.local_workers <= 0:
        fail("--local-workers must be >= 1")
    if args.worker_count <= 0:
        fail("--worker-count must be >= 1")
    if args.worker_index < 0 or args.worker_index >= args.worker_count:
        fail("--worker-index must be in [0, worker-count)")

    # clients=0 = cluster-only smoke; --script-clients still keeps shard 0 alive.
    if args.clients <= 0 and args.script_clients <= 0:
        return []

    total_workers = args.worker_count * args.local_workers
    base_worker_index = args.worker_index * args.local_workers

    workers: list[dict[str, object]] = []
    for local_index in range(args.local_workers):
        global_worker_index = base_worker_index + local_index
        client_offset, client_count = split_range(args.clients, total_workers, global_worker_index)
        # Shard 0 owns the script-client fleet; other empty shards skip.
        if client_count <= 0 and not (args.script_clients > 0 and global_worker_index == 0):
            continue

        if args.account_pool >= total_workers:
            account_offset, account_count = split_range(
                args.account_pool, total_workers, global_worker_index
            )
            account_index_base = args.account_index_base + account_offset
        else:
            account_count = max(1, args.account_pool)
            account_index_base = args.account_index_base

        workers.append(
            {
                "global_worker_index": global_worker_index,
                "global_worker_count": total_workers,
                "client_offset": client_offset,
                "clients": client_count,
                "account_pool": account_count,
                "account_index_base": account_index_base,
                "source_ips": assign_worker_source_ips(
                    source_ips, global_worker_index, total_workers
                ),
            }
        )

    if not workers:
        fail("no stress workers were scheduled; increase --clients or lower worker count")
    return workers


def _config_to_snake(config: str) -> str:
    """Convert PascalCase config name to snake_case (RelWithDebInfo -> rel_with_deb_info)."""
    import re
    return re.sub(r"([a-z])([A-Z])", r"\1_\2", config).lower()


def _exe_suffixes() -> list[str]:
    return [".exe", ""] if os.name == "nt" else ["", ".exe"]


def _dll_env(exe_path: Path) -> dict[str, str]:
    """Return os.environ.copy(); flat bin/<build>/ layout means no PATH munging needed."""
    del exe_path
    return os.environ.copy()


def _git_short(repo_root: Path) -> str:
    try:
        r = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, cwd=repo_root,
        )
        return r.stdout.strip() if r.returncode == 0 else "unknown"
    except FileNotFoundError:
        return "unknown"


def _tracy_port_for_pid(pid: int, timeout_sec: float = 8.0) -> int | None:
    """Return the Tracy TCP listener port for pid, or None if not found in time."""
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        if os.name == "nt":
            result = subprocess.run(
                [
                    "powershell", "-NoProfile", "-NonInteractive", "-Command",
                    f"Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue "
                    f"| Where-Object {{ $_.OwningProcess -eq {pid} "
                    f"  -and $_.LocalPort -ge 8086 -and $_.LocalPort -le 8200 }} "
                    f"| Select-Object -ExpandProperty LocalPort -First 1",
                ],
                capture_output=True, text=True,
            )
        else:
            result = subprocess.run(
                ["ss", "-tlnp"],
                capture_output=True, text=True,
            )
            # Crude grep for pid in ss output is good enough for dev use.
            port = None
            for line in result.stdout.splitlines():
                if f"pid={pid}" in line:
                    parts = line.split()
                    addr = parts[3] if len(parts) > 3 else ""
                    if ":" in addr:
                        try:
                            p = int(addr.rsplit(":", 1)[1])
                            if 8086 <= p <= 8200:
                                port = p
                                break
                        except ValueError:
                            pass
            return port

        port_str = result.stdout.strip()
        if port_str.isdigit():
            return int(port_str)
        time.sleep(0.5)
    return None


def start_tracy_captures(
    *,
    capture_exe: Path,
    processes: list["LoggedProcess"],
    wanted_names: set[str],
    capture_dir: Path,
    git_hash: str,
    timestamp: str,
    duration_sec: int,
) -> list[subprocess.Popen[str]]:
    """Start tracy-capture for each wanted process; return launched Popen list."""
    capture_dir = capture_dir.resolve()
    capture_dir.mkdir(parents=True, exist_ok=True)
    env = _dll_env(capture_exe)
    captures: list[subprocess.Popen[str]] = []
    for proc_entry in processes:
        if proc_entry.name not in wanted_names:
            continue
        pid = proc_entry.process.pid
        port = _tracy_port_for_pid(pid)
        if port is None:
            log(f"[capture] {proc_entry.name}: Tracy port not found for pid={pid}, skipping")
            continue
        out_file = capture_dir / f"{proc_entry.name}_{git_hash}_{timestamp}.tracy"
        log(f"[capture] {proc_entry.name} pid={pid} port={port} -> {out_file.name}")
        captures.append(
            subprocess.Popen(
                [str(capture_exe), "-a", "127.0.0.1", "-p", str(port),
                 "-s", str(duration_sec + 10), "-o", str(out_file)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env=env,
            )
        )
    return captures


def stop_tracy_captures(captures: list[subprocess.Popen[str]]) -> None:
    """Let tracy-capture flush (it writes .tracy only on clean exit); kill after 20s."""
    for p in captures:
        try:
            p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            p.kill()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass


def resolve_program(
    build_root: Path, bin_name: str, subdirs: Iterable[str], stem: str
) -> Path:
    """Locate an executable under bin/<bin_name>/; legacy nested subdirs are also searched."""
    bin_base = build_root / "bin" / bin_name
    for subdir in (*subdirs, ""):
        for suffix in _exe_suffixes():
            candidate = bin_base / subdir / f"{stem}{suffix}"
            if candidate.exists():
                return candidate
    return bin_base / f"{stem}{'.exe' if os.name == 'nt' else ''}"


@dataclass
class LoggedProcess:
    name: str
    start_order: int
    process: subprocess.Popen[str]
    stdout_handle: object
    stderr_handle: object


def start_logged_process(
    *,
    name: str,
    file_path: Path,
    arguments: Iterable[str],
    working_directory: Path,
    log_directory: Path,
    env: dict[str, str] | None = None,
) -> LoggedProcess:
    stdout_path = log_directory / f"{name}.stdout.log"
    stderr_path = log_directory / f"{name}.stderr.log"

    log(f"Starting {name}")
    stdout_handle = stdout_path.open("w", encoding="utf-8", newline="")
    stderr_handle = stderr_path.open("w", encoding="utf-8", newline="")

    creationflags = 0
    popen_kwargs: dict[str, object] = {}
    if os.name == "nt":
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP  # type: ignore[attr-defined]
    else:
        popen_kwargs["start_new_session"] = True

    process = subprocess.Popen(
        [str(file_path), *list(arguments)],
        cwd=working_directory,
        stdout=stdout_handle,
        stderr=stderr_handle,
        text=True,
        creationflags=creationflags,
        env=env,
        **popen_kwargs,
    )

    stdout_handle.close()
    stderr_handle.close()

    return LoggedProcess(
        name=name,
        start_order=0,
        process=process,
        stdout_handle=None,
        stderr_handle=None,
    )


def stop_logged_processes(processes: list[LoggedProcess]) -> None:
    for order in sorted({entry.start_order for entry in processes}, reverse=True):
        group = [entry for entry in processes if entry.start_order == order]
        for entry in group:
            proc = entry.process
            if proc.poll() is None:
                log(f"Stopping {entry.name} (pid={proc.pid})")
                try:
                    if os.name == "nt":
                        os.kill(proc.pid, signal.CTRL_BREAK_EVENT)
                    else:
                        os.killpg(proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass

        for entry in group:
            proc = entry.process
            if proc.poll() is None:
                try:
                    proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    try:
                        if os.name == "nt":
                            proc.kill()
                        else:
                            os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        pass

            if entry.stdout_handle:
                entry.stdout_handle.close()
            if entry.stderr_handle:
                entry.stderr_handle.close()


def request_shutdown_target(
    atlas_tool: Path, machined_address: str, repo_root: Path, target: str, reason: int
) -> None:
    subprocess.run(
        [
            str(atlas_tool),
            "--machined",
            machined_address,
            "shutdown",
            target,
            str(reason),
        ],
        cwd=repo_root,
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=_dll_env(atlas_tool),
    )


def wait_for_registration(
    *,
    atlas_tool: Path,
    machined_address: str,
    proc_type: str,
    name: str,
    timeout_sec: int = 15,
) -> bool:
    env = _dll_env(atlas_tool)
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        result = subprocess.run(
            [str(atlas_tool), "--machined", machined_address, "list", proc_type],
            capture_output=True,
            text=True,
            cwd=resolve_repo_root(),
            env=env,
        )
        if result.returncode == 0 and name in result.stdout:
            return True
        time.sleep(0.5)
    return False


def build_runtime_config(
    *,
    machined_address: str,
    account_type_id: int,
    db_dir: Path,
) -> dict[str, object]:
    sqlite_path = db_dir / "atlas_world_stress.sqlite3"
    return {
        "machined_address": machined_address,
        "auto_create_accounts": True,
        "account_type_id": account_type_id,
        "database": {
            "type": "sqlite",
            "sqlite_path": str(sqlite_path),
            "sqlite_foreign_keys": True,
        },
    }


def extend_repeated_flag(arguments: list[str], flag: str, values: Iterable[str]) -> None:
    for value in values:
        arguments.extend([flag, value])


def build_loginapp_args(args: argparse.Namespace, machined_address: str) -> list[str]:
    loginapp_args = [
        "--type",
        "loginapp",
        "--name",
        "loginapp",
        "--machined",
        machined_address,
        "--external-port",
        str(args.login_port),
        "--auto-create-accounts",
        "true",
        "--login-rate-limit-per-ip",
        str(args.login_rate_limit_per_ip),
        "--login-rate-limit-global",
        str(args.login_rate_limit_global),
        "--login-rate-limit-window-sec",
        str(args.login_rate_limit_window_sec),
        "--update-hertz",
        str(args.loginapp_update_hertz),
        "--log-level",
        "info",
    ]
    extend_repeated_flag(
        loginapp_args, "--login-rate-limit-trusted-cidr", args.login_rate_limit_trusted_cidr
    )
    return loginapp_args


def build_stress_args(
    args: argparse.Namespace, worker: dict[str, object], entity_def_digest: str
) -> list[str]:
    stress_args = [
        "--login",
        f"{args.machined_host}:{args.login_port}",
        "--password-hash",
        args.password_hash,
        "--entity-def-digest",
        entity_def_digest,
        "--clients",
        str(worker["clients"]),
        "--account-pool",
        str(worker["account_pool"]),
        "--account-index-base",
        str(worker["account_index_base"]),
        "--worker-index",
        str(worker["global_worker_index"]),
        "--worker-count",
        str(worker["global_worker_count"]),
        "--ramp-per-sec",
        str(args.ramp_per_sec),
        "--duration-sec",
        str(args.duration_sec),
        "--retry-delay-ms",
        str(args.retry_delay_ms),
        "--connect-timeout-ms",
        str(args.connect_timeout_ms),
        "--hold-min-ms",
        str(args.hold_min_ms),
        "--hold-max-ms",
        str(args.hold_max_ms),
        "--shortline-pct",
        str(args.shortline_pct),
        "--shortline-min-ms",
        str(args.shortline_min_ms),
        "--shortline-max-ms",
        str(args.shortline_max_ms),
        "--rpc-rate-hz",
        str(args.rpc_rate_hz),
        "--move-rate-hz",
        str(args.move_rate_hz),
        "--move-mode",
        args.move_mode,
        "--movement-input-redundant-frames",
        str(args.movement_input_redundant_frames),
        "--movement-input-drop-pct",
        str(args.movement_input_drop_pct),
        "--movement-input-reorder-pct",
        str(args.movement_input_reorder_pct),
        "--spread-radius",
        str(args.spread_radius),
        "--space-count",
        str(args.space_count),
    ]
    if args.walk_step_meters is not None:
        stress_args.extend(["--walk-step-meters", str(args.walk_step_meters)])
    if args.walk_range_meters is not None:
        stress_args.extend(["--walk-range-meters", str(args.walk_range_meters)])
    if args.teleport_pct is not None:
        stress_args.extend(["--teleport-pct", str(args.teleport_pct)])
    extend_repeated_flag(stress_args, "--source-ip", worker["source_ips"])
    if args.verbose_failures:
        stress_args.append("--verbose-failures")

    # Only shard 0 spawns script children; others would race for the same usernames.
    is_primary_worker = int(worker["global_worker_index"]) == 0
    if args.script_clients > 0 and is_primary_worker:
        stress_args.extend(["--script-clients", str(args.script_clients)])
        client_exe = args.client_exe or default_client_exe(args)
        client_assembly = args.client_assembly or default_client_assembly(args)
        client_runtime_config = args.client_runtime_config or default_client_runtime_config(
            args, client_assembly
        )
        stress_args.extend(["--client-exe", str(client_exe)])
        stress_args.extend(["--client-assembly", str(client_assembly)])
        if client_runtime_config:
            stress_args.extend(["--client-runtime-config", str(client_runtime_config)])
        if args.script_username_prefix != "script_user_":
            stress_args.extend(["--script-username-prefix", args.script_username_prefix])
        if args.script_verify:
            stress_args.append("--script-verify")
        if args.client_drop_inbound_ms:
            start_ms, duration_ms = args.client_drop_inbound_ms
            stress_args.extend([
                "--client-drop-inbound-ms", str(start_ms), str(duration_ms),
            ])
        if args.client_drop_transport_ms:
            start_ms, duration_ms = args.client_drop_transport_ms
            stress_args.extend([
                "--client-drop-transport-ms", str(start_ms), str(duration_ms),
            ])
        if args.client_transport_impairment_ms:
            latency_ms, loss_permyriad = args.client_transport_impairment_ms
            stress_args.extend([
                "--client-transport-impairment-ms", str(latency_ms), str(loss_permyriad),
            ])
    return stress_args


_DIGEST_FILE = (
    Path("samples") / "stress" / "Atlas.StressTest.Base" / "obj" / "generated"
    / "Atlas.Generators.Def" / "Atlas.Generators.Def.DefGenerator"
    / "EntityDefDigest.g.cs"
)
_DIGEST_RE = re.compile(r'Sha256Hex\s*=\s*"([0-9A-Fa-f]{64})"')
_DEFDUMP_RE = re.compile(r"DefDump:\s+digest=([0-9A-Fa-f]{64})")


def find_defdump(repo_root: Path, build_dir: str) -> Path | None:
    build_path = repo_root / build_dir
    build_name = Path(build_dir).name
    candidates = [
        build_path / "csharp" / "Atlas.Tools.DefDump.dll",
        repo_root / "build" / build_name / "csharp" / "Atlas.Tools.DefDump.dll",
        repo_root / "bin" / build_name / "Atlas.Tools.DefDump.dll",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def load_entity_def_digest(repo_root: Path, build_dir: str, assembly_path: Path) -> str:
    defdump = find_defdump(repo_root, build_dir)
    if defdump is not None:
        try:
            proc = subprocess.run(
                ["dotnet", str(defdump), "--digest-only", str(assembly_path)],
                capture_output=True,
                text=True,
                cwd=repo_root,
                timeout=30,
            )
        except FileNotFoundError:
            fail("dotnet not found; install the .NET SDK or run from a developer shell")
        if proc.returncode != 0:
            detail = proc.stderr.strip() or proc.stdout.strip()
            fail(f"DefDump failed for {assembly_path}: {detail}")
        match = _DEFDUMP_RE.search(proc.stdout)
        if not match:
            fail(f"DefDump output did not contain a digest for {assembly_path}")
        return match.group(1)

    path = repo_root / _DIGEST_FILE
    if not path.is_file():
        fail(f"EntityDefDigest.g.cs not found at {path}; build the C# stress projects first.")
    match = _DIGEST_RE.search(path.read_text(encoding="utf-8"))
    if not match:
        fail(f"Could not extract Sha256Hex from {path}.")
    return match.group(1)


def default_client_exe(args: argparse.Namespace) -> Path:
    bin_name = Path(args.build_dir).name
    return resolve_repo_root() / "bin" / bin_name / "atlas_client.exe"


def default_client_assembly(args: argparse.Namespace) -> Path:
    bin_name = Path(args.build_dir).name
    deployed = resolve_repo_root() / "bin" / bin_name / "Atlas.ClientSample.dll"
    if deployed.exists():
        return deployed
    config_dir = resolve_repo_root() / "samples" / "client" / "bin" / args.config
    return dotnet_tfm_dir(config_dir) / "Atlas.ClientSample.dll"


def default_client_runtime_config(
    args: argparse.Namespace, client_assembly: Path | str
) -> Path | None:
    assembly = Path(client_assembly)
    sibling = assembly.parent / f"{assembly.stem}.runtimeconfig.json"
    if sibling.exists():
        return sibling
    config_dir = resolve_repo_root() / "samples" / "client" / "bin" / args.config
    sample = dotnet_tfm_dir(config_dir) / "Atlas.ClientSample.runtimeconfig.json"
    return sample if sample.exists() else None


def build_baseapp_specs(args: argparse.Namespace) -> list[dict[str, object]]:
    specs: list[dict[str, object]] = []
    for index in range(args.baseapp_count):
        specs.append(
            {
                "index": index,
                "name": "baseapp" if index == 0 else f"baseapp_{index:02d}",
                "log_name": "baseapp" if index == 0 else f"baseapp_{index:02d}",
                "internal_port": args.baseapp_internal_port
                + index * args.baseapp_internal_port_stride,
                "external_port": args.baseapp_external_port
                + index * args.baseapp_external_port_stride,
            }
        )
    return specs


def build_cellapp_specs(args: argparse.Namespace) -> list[dict[str, object]]:
    specs: list[dict[str, object]] = []
    for index in range(args.cellapp_count):
        specs.append(
            {
                "index": index,
                "name": "cellapp" if index == 0 else f"cellapp_{index:02d}",
                "log_name": "cellapp" if index == 0 else f"cellapp_{index:02d}",
                "internal_port": args.cellapp_internal_port
                + index * args.cellapp_internal_port_stride,
            }
        )
    return specs


def build_reviver_specs(args: argparse.Namespace) -> list[dict[str, object]]:
    specs: list[dict[str, object]] = []
    for index in range(args.cellappmgr_reviver_count):
        specs.append(
            {
                "index": index,
                "name": "reviver" if index == 0 else f"reviver_{index:02d}",
                "log_name": "reviver" if index == 0 else f"reviver_{index:02d}",
                "internal_port": args.reviver_port + index * args.reviver_port_stride,
            }
        )
    return specs


def build_reviver_args(
    args: argparse.Namespace,
    spec: dict[str, object],
    machined_address: str,
    cellappmgr: Path,
    cellappmgr_snapshot_path: Path | None,
    reviver_leader_lock_path: Path | None,
    revived_cellappmgr_output_path: Path | None,
) -> list[str]:
    reviver_args = [
        "--type",
        "reviver",
        "--name",
        str(spec["name"]),
        "--machined",
        machined_address,
        "--internal-port",
        str(spec["internal_port"]),
        "--update-hertz",
        str(args.reviver_update_hertz),
        "--revive-cellappmgr-exe",
        str(cellappmgr),
        "--revive-cellappmgr-name",
        "cellappmgr",
        "--revive-cellappmgr-port",
        str(args.cellappmgr_port),
        "--revive-cellappmgr-on-start",
        "true",
        "--revive-cellappmgr-update-hertz",
        str(args.cellappmgr_update_hertz),
        "--revive-cellappmgr-output-path",
        str(revived_cellappmgr_output_path),
        "--revive-cellappmgr-heartbeat-timeout-ms",
        str(args.reviver_heartbeat_timeout_ms),
        "--revive-restart-delay-ms",
        str(args.reviver_restart_delay_ms),
        "--revive-max-restarts",
        str(args.reviver_max_restarts),
        "--log-level",
        "info",
    ]
    if cellappmgr_snapshot_path is not None:
        reviver_args.extend(
            [
                "--revive-cellappmgr-snapshot-path",
                str(cellappmgr_snapshot_path),
                "--revive-cellappmgr-snapshot-interval-ms",
                str(args.cellappmgr_snapshot_interval_ms),
            ]
        )
    if reviver_leader_lock_path is not None:
        reviver_args.extend(["--revive-leader-lock-path", str(reviver_leader_lock_path)])
    reviver_args.extend([
        "--revive-leader-lock-mode", args.reviver_leader_lock_mode,
        "--revive-leader-lock-ttl-ms", str(args.reviver_leader_lock_ttl_ms),
        "--revive-leader-lock-renew-ms", str(args.reviver_leader_lock_renew_ms),
    ])
    return reviver_args


def main() -> int:
    args = parse_args()
    if args.cellappmgr_reviver_count < 1:
        fail("--cellappmgr-reviver-count must be >= 1")
    if args.reviver_port_stride <= 0:
        fail("--reviver-port-stride must be >= 1")
    if not args.with_cellappmgr_reviver and args.cellappmgr_reviver_count != 1:
        fail("--cellappmgr-reviver-count requires --with-cellappmgr-reviver")
    source_ips = collect_source_ips(args)
    worker_plan = build_worker_plan(args, source_ips)
    baseapp_specs = build_baseapp_specs(args)
    cellapp_specs = build_cellapp_specs(args)
    reviver_specs = build_reviver_specs(args)

    repo_root = resolve_repo_root()
    runtime_config = repo_root / "runtime" / "atlas_server.runtimeconfig.json"

    # bin/ path is keyed on the build directory name, not the CMake config name.
    # AtlasOutputDirectory.cmake routes all artifacts into bin/<build_dir_name>/.
    bin_name = Path(args.build_dir).name
    bin_base = repo_root / "bin" / bin_name

    # C# assemblies deployed by CMake into bin/<bin_name>/ (flat layout).
    base_assembly = Path(args.base_assembly) if args.base_assembly \
                    else bin_base / "Atlas.StressTest.Base.dll"
    cell_assembly = Path(args.cell_assembly) if args.cell_assembly \
                    else bin_base / "Atlas.StressTest.Cell.dll"

    # Legacy subdirectory hints kept for transitional builds; resolve_program
    # also falls back to the flat bin/<bin_name>/ root.
    search_subdirs = []

    atlas_tool = resolve_program(repo_root, bin_name, search_subdirs, "atlas_tool")
    machined = resolve_program(repo_root, bin_name, search_subdirs, "machined")
    loginapp = resolve_program(repo_root, bin_name, search_subdirs, "atlas_loginapp")
    baseapp = resolve_program(repo_root, bin_name, search_subdirs, "atlas_baseapp")
    baseappmgr = resolve_program(repo_root, bin_name, search_subdirs, "atlas_baseappmgr")
    dbapp = resolve_program(repo_root, bin_name, search_subdirs, "atlas_dbapp")
    cellapp = resolve_program(repo_root, bin_name, search_subdirs, "atlas_cellapp")
    cellappmgr = resolve_program(repo_root, bin_name, search_subdirs, "atlas_cellappmgr")
    reviver = resolve_program(repo_root, bin_name, search_subdirs, "atlas_reviver")

    assert_file_exists(machined, machined.name)
    assert_file_exists(loginapp, loginapp.name)
    assert_file_exists(baseapp, baseapp.name)
    assert_file_exists(baseappmgr, baseappmgr.name)
    assert_file_exists(dbapp, dbapp.name)
    assert_file_exists(cellapp, cellapp.name)
    assert_file_exists(cellappmgr, cellappmgr.name)
    if args.with_cellappmgr_reviver:
        assert_file_exists(reviver, reviver.name)
    assert_file_exists(atlas_tool, atlas_tool.name)
    assert_file_exists(runtime_config, runtime_config.name)
    assert_file_exists(base_assembly, base_assembly.name)
    assert_file_exists(cell_assembly, cell_assembly.name)

    # world_stress is only required when clients > 0.
    world_stress: Path | None = None
    entity_def_digest: str | None = None
    if worker_plan:
        world_stress = resolve_program(
            repo_root, bin_name, search_subdirs, "world_stress"
        )
        assert_file_exists(world_stress, world_stress.name)
        entity_def_digest = load_entity_def_digest(repo_root, args.build_dir, base_assembly)

    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    git_hash = _git_short(repo_root)
    run_root = repo_root / ".tmp" / "world-stress" / timestamp
    log_dir = run_root / "logs"
    db_dir = run_root / "db"
    ha_dir = run_root / "ha"
    db_config_path = run_root / "dbapp.json"
    revived_cellappmgr_output_path = (
        log_dir / "cellappmgr_revived.log" if args.with_cellappmgr_reviver else None
    )
    cellappmgr_snapshot_path = resolve_optional_path(
        repo_root, args.cellappmgr_snapshot_path
    )
    reviver_leader_lock_path = resolve_optional_path(
        repo_root, args.reviver_leader_lock_path
    )
    if args.with_cellappmgr_reviver:
        if cellappmgr_snapshot_path is None:
            cellappmgr_snapshot_path = ha_dir / "cellappmgr.bin"
        if reviver_leader_lock_path is None:
            reviver_leader_lock_path = ha_dir / "reviver_cellappmgr.lock"

    capture_exe: Path | None = None
    capture_dir: Path | None = None
    if args.capture_dir:
        capture_exe = resolve_program(repo_root, bin_name, search_subdirs, "tracy-capture")
        if not capture_exe.exists():
            fail(
                f"--capture-dir set but tracy-capture not found at {capture_exe}. "
                f"Build with -DATLAS_BUILD_TRACY_VIEWER=ON."
            )
        capture_dir = Path(args.capture_dir)

    log_dir.mkdir(parents=True, exist_ok=True)
    db_dir.mkdir(parents=True, exist_ok=True)
    if cellappmgr_snapshot_path is not None:
        cellappmgr_snapshot_path.parent.mkdir(parents=True, exist_ok=True)
    if reviver_leader_lock_path is not None:
        reviver_leader_lock_path.parent.mkdir(parents=True, exist_ok=True)

    machined_address = f"{args.machined_host}:{args.machined_port}"
    db_config_path.write_text(
        json.dumps(
            build_runtime_config(
                machined_address=machined_address,
                account_type_id=args.account_type_id,
                db_dir=db_dir,
            ),
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    processes: list[LoggedProcess] = []

    try:
        processes.append(
            start_logged_process(
                name="machined",
                file_path=machined,
                working_directory=repo_root,
                log_directory=log_dir,
                arguments=[
                    "--type",
                    "machined",
                    "--name",
                    "machined",
                    "--internal-port",
                    str(args.machined_port),
                    "--log-level",
                    "info",
                ],
            )
        )
        processes[-1].start_order = 1
        time.sleep(1)

        processes.append(
            start_logged_process(
                name="loginapp",
                file_path=loginapp,
                working_directory=repo_root,
                log_directory=log_dir,
                arguments=build_loginapp_args(args, machined_address),
            )
        )
        processes[-1].start_order = 7
        time.sleep(1)

        processes.append(
            start_logged_process(
                name="dbapp",
                file_path=dbapp,
                working_directory=repo_root,
                log_directory=log_dir,
                arguments=[
                    "--type",
                    "dbapp",
                    "--name",
                    "dbapp",
                    "--machined",
                    machined_address,
                    "--internal-port",
                    str(args.dbapp_port),
                    "--config",
                    str(db_config_path),
                    "--update-hertz",
                    str(args.dbapp_update_hertz),
                    "--log-level",
                    "info",
                ],
            )
        )
        processes[-1].start_order = 2
        time.sleep(1)

        processes.append(
            start_logged_process(
                name="baseappmgr",
                file_path=baseappmgr,
                working_directory=repo_root,
                log_directory=log_dir,
                arguments=[
                    "--type",
                    "baseappmgr",
                    "--name",
                    "baseappmgr",
                    "--machined",
                    machined_address,
                    "--internal-port",
                    str(args.baseappmgr_port),
                    "--update-hertz",
                    str(args.baseappmgr_update_hertz),
                    "--log-level",
                    "info",
                ],
            )
        )
        processes[-1].start_order = 5
        time.sleep(1)

        cellappmgr_args = [
            "--type",
            "cellappmgr",
            "--name",
            "cellappmgr",
            "--machined",
            machined_address,
            "--internal-port",
            str(args.cellappmgr_port),
            "--update-hertz",
            str(args.cellappmgr_update_hertz),
            "--log-level",
            "info",
        ]
        if cellappmgr_snapshot_path is not None:
            cellappmgr_args.extend(
                [
                    "--snapshot-path",
                    str(cellappmgr_snapshot_path),
                    "--snapshot-interval-ms",
                    str(args.cellappmgr_snapshot_interval_ms),
                ]
            )
        processes.append(
            start_logged_process(
                name="cellappmgr",
                file_path=cellappmgr,
                working_directory=repo_root,
                log_directory=log_dir,
                arguments=cellappmgr_args,
            )
        )
        processes[-1].start_order = 6
        time.sleep(1)

        if args.with_cellappmgr_reviver:
            for reviver_spec in reviver_specs:
                processes.append(
                    start_logged_process(
                        name=str(reviver_spec["log_name"]),
                        file_path=reviver,
                        working_directory=repo_root,
                        log_directory=log_dir,
                        arguments=build_reviver_args(
                            args,
                            reviver_spec,
                            machined_address,
                            cellappmgr,
                            cellappmgr_snapshot_path,
                            reviver_leader_lock_path,
                            revived_cellappmgr_output_path,
                        ),
                    )
                )
                processes[-1].start_order = 8
                time.sleep(1)

        # Launch CellApps before BaseApps so CreateSpace sees the full host pool.
        # This avoids elastic-grow drip-feeding cells one registration at a time.
        for cellapp_spec in cellapp_specs:
            processes.append(
                start_logged_process(
                    name=str(cellapp_spec["log_name"]),
                    file_path=cellapp,
                    working_directory=repo_root,
                    log_directory=log_dir,
                    arguments=[
                        "--type",
                        "cellapp",
                        "--name",
                        str(cellapp_spec["name"]),
                        "--machined",
                        machined_address,
                        "--internal-port",
                        str(cellapp_spec["internal_port"]),
                        "--assembly",
                        str(cell_assembly),
                        "--runtime-config",
                        str(runtime_config),
                        "--update-hertz",
                        str(args.cellapp_update_hertz),
                        "--log-level",
                        "info",
                    ],
                )
            )
            processes[-1].start_order = 3
        time.sleep(1)

        for baseapp_spec in baseapp_specs:
            processes.append(
                start_logged_process(
                    name=str(baseapp_spec["log_name"]),
                    file_path=baseapp,
                    working_directory=repo_root,
                    log_directory=log_dir,
                    arguments=[
                        "--type",
                        "baseapp",
                        "--name",
                        str(baseapp_spec["name"]),
                        "--machined",
                        machined_address,
                        "--internal-port",
                        str(baseapp_spec["internal_port"]),
                        "--external-port",
                        str(baseapp_spec["external_port"]),
                        "--assembly",
                        str(base_assembly),
                        "--runtime-config",
                        str(runtime_config),
                        "--update-hertz",
                        str(args.baseapp_update_hertz),
                        "--log-level",
                        "info",
                    ],
                )
            )
            processes[-1].start_order = 4
        time.sleep(1)

        log("Waiting for processes to register with machined...")
        registrations: list[tuple[str, bool]] = []
        registrations.append(
            (
                "dbapp",
                wait_for_registration(
                    atlas_tool=atlas_tool,
                    machined_address=machined_address,
                    proc_type="dbapp",
                    name="dbapp",
                ),
            )
        )
        registrations.append(
            (
                "baseappmgr",
                wait_for_registration(
                    atlas_tool=atlas_tool,
                    machined_address=machined_address,
                    proc_type="baseappmgr",
                    name="baseappmgr",
                ),
            )
        )
        registrations.append(
            (
                "cellappmgr",
                wait_for_registration(
                    atlas_tool=atlas_tool,
                    machined_address=machined_address,
                    proc_type="cellappmgr",
                    name="cellappmgr",
                ),
            )
        )
        if args.with_cellappmgr_reviver:
            for reviver_spec in reviver_specs:
                registrations.append(
                    (
                        str(reviver_spec["name"]),
                        wait_for_registration(
                            atlas_tool=atlas_tool,
                            machined_address=machined_address,
                            proc_type="reviver",
                            name=str(reviver_spec["name"]),
                        ),
                    )
                )
        for baseapp_spec in baseapp_specs:
            registrations.append(
                (
                    str(baseapp_spec["name"]),
                    wait_for_registration(
                        atlas_tool=atlas_tool,
                        machined_address=machined_address,
                        proc_type="baseapp",
                        name=str(baseapp_spec["name"]),
                    ),
                )
            )
        for cellapp_spec in cellapp_specs:
            registrations.append(
                (
                    str(cellapp_spec["name"]),
                    wait_for_registration(
                        atlas_tool=atlas_tool,
                        machined_address=machined_address,
                        proc_type="cellapp",
                        name=str(cellapp_spec["name"]),
                    ),
                )
            )
        registrations.append(
            (
                "loginapp",
                wait_for_registration(
                    atlas_tool=atlas_tool,
                    machined_address=machined_address,
                    proc_type="loginapp",
                    name="loginapp",
                ),
            )
        )

        missing = [name for name, ok in registrations if not ok]
        if missing:
            log(
                "Warning: the following processes did not register within timeout: "
                + ", ".join(missing)
                + f". Check logs under {log_dir}.",
                stream=sys.stderr,
            )
        else:
            log("")
            log("Registered processes:")
            print_registered_processes(atlas_tool, machined_address, repo_root)
            log("")

        # Start Tracy captures for requested processes (after all are registered).
        active_captures: list[subprocess.Popen[str]] = []
        if capture_exe and capture_dir:
            wanted = {n.strip() for n in args.capture_procs.split(",") if n.strip()}
            active_captures = start_tracy_captures(
                capture_exe=capture_exe,
                processes=processes,
                wanted_names=wanted,
                capture_dir=capture_dir,
                git_hash=git_hash,
                timestamp=timestamp,
                duration_sec=args.duration_sec,
            )

        try:
            if not worker_plan:
                if args.keep_cluster:
                    refresh_sec = args.load_refresh_sec
                    if refresh_sec > 0:
                        log(
                            "No stress workers scheduled; cluster running. "
                            f"Refreshing LOAD every {max(0.5, refresh_sec):g}s. Ctrl+C to stop."
                        )
                    else:
                        log("No stress workers scheduled; cluster running. Ctrl+C to stop.")
                    try:
                        while True:
                            if refresh_sec <= 0:
                                time.sleep(3600)
                                continue
                            time.sleep(max(0.5, refresh_sec))
                            log("")
                            log("Registered processes:")
                            print_registered_processes(atlas_tool, machined_address, repo_root)
                    except KeyboardInterrupt:
                        log("Interrupted; shutting cluster down...")
                        args.keep_cluster = False
                else:
                    log(
                        f"No stress workers scheduled (clients={args.clients}); "
                        f"holding cluster for {args.duration_sec}s to verify stability..."
                    )
                    time.sleep(max(1, args.duration_sec))
            elif args.local_workers == 1:
                assert world_stress is not None
                worker = worker_plan[0]
                log(
                    "Running world_stress..."
                    f" worker={worker['global_worker_index']}/{worker['global_worker_count']}"
                    f" clients={worker['clients']} account_pool={worker['account_pool']}"
                    f" baseapps={len(baseapp_specs)} cellapps={len(cellapp_specs)}"
                    f" source_ips={len(worker['source_ips'])}"
                )
                assert entity_def_digest is not None
                stress_result = subprocess.run(
                    [str(world_stress), *build_stress_args(args, worker, entity_def_digest)],
                    cwd=repo_root,
                    env=_dll_env(world_stress),
                )
                if stress_result.returncode != 0:
                    fail(f"world_stress exited with code {stress_result.returncode}")
            else:
                assert world_stress is not None
                stress_workers: list[LoggedProcess] = []
                try:
                    for ordinal, worker in enumerate(worker_plan):
                        name = f"world_stress_worker_{ordinal:02d}"
                        log(
                            f"Starting {name}: global_worker={worker['global_worker_index']}/"
                            f"{worker['global_worker_count']} clients={worker['clients']} "
                            f"account_pool={worker['account_pool']} baseapps={len(baseapp_specs)} "
                            f"cellapps={len(cellapp_specs)} "
                            f"source_ips={len(worker['source_ips'])}"
                        )
                        assert entity_def_digest is not None
                        stress_workers.append(
                            start_logged_process(
                                name=name,
                                file_path=world_stress,
                                arguments=build_stress_args(args, worker, entity_def_digest),
                                working_directory=repo_root,
                                log_directory=log_dir,
                                env=_dll_env(world_stress),
                            )
                        )

                    for worker_proc in stress_workers:
                        return_code = worker_proc.process.wait()
                        if worker_proc.stdout_handle:
                            worker_proc.stdout_handle.close()
                        if worker_proc.stderr_handle:
                            worker_proc.stderr_handle.close()
                        worker_proc.stdout_handle = None
                        worker_proc.stderr_handle = None
                        if return_code != 0:
                            fail(f"{worker_proc.name} exited with code {return_code}")
                finally:
                    stop_logged_processes(
                        [w for w in stress_workers if w.process.poll() is None]
                    )
        finally:
            stop_tracy_captures(active_captures)
            if active_captures and capture_dir:
                log(f"[capture] Tracy traces saved to {capture_dir}")

        movement_verify_errors: list[str] = []
        movement_summary = (
            args.script_clients > 0 or args.move_mode == "input" or args.movement_verify
        )
        if movement_summary:
            log("")
            base_rows, cell_rows = print_movement_watcher_summary(
                atlas_tool,
                machined_address,
                repo_root,
            )
            if args.script_verify or args.movement_verify:
                movement_verify_errors = verify_movement_watchers(
                    base_rows,
                    cell_rows,
                    require_reports=args.script_clients > 0,
                )

        log("")
        log("Run artifacts:")
        log(f"  logs: {log_dir}")
        log(f"  db:   {db_dir}")

        if movement_verify_errors:
            gate = "script-verify" if args.script_verify else "movement-verify"
            log(f"[{gate}] movement watcher gate failed:")
            for error in movement_verify_errors:
                log(f"  {error}")
            return 4

        if args.keep_cluster:
            log("Keeping cluster alive. Stop the spawned processes manually when done.")
            processes = []

        # Cellapp invariant violations: usually a regression in destruction-leave fan-out.
        audit_lines = scan_cellapp_audits(log_dir)
        if audit_lines and not args.allow_audit_violations:
            log(f"[audit] cellapp reported {len(audit_lines)} invariant violation(s):")
            for line in audit_lines[:20]:
                log(f"  {line}")
            if len(audit_lines) > 20:
                log(f"  ... and {len(audit_lines) - 20} more (truncated)")
            return 3

        return 0 if not missing else 2
    finally:
        if processes:
            if args.with_cellappmgr_reviver:
                request_shutdown_target(
                    atlas_tool, machined_address, repo_root, "cellappmgr:cellappmgr", 0
                )
                time.sleep(1)
            stop_logged_processes(processes)


if __name__ == "__main__":
    sys.exit(main())
