#!/usr/bin/env python3

# Bring up the full Atlas cluster (machined / loginapp / dbapp / baseappmgr /
# baseapp / cellappmgr / cellapp) and optionally drive it with `world_stress`.
#
# P1 milestone: allow --clients 0 to bring up the cluster only and verify
# that every process registers with machined. The world_stress binary is
# resolved lazily so P1 does not require it to exist yet.

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable, NoReturn


REPO_ROOT = Path(__file__).resolve().parents[2]


def resolve_repo_root() -> Path:
    return REPO_ROOT


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
    parser.add_argument("--cellapp-internal-port", type=int, default=26001)
    parser.add_argument("--cellapp-count", type=int, default=1)
    parser.add_argument("--cellapp-internal-port-stride", type=int, default=1)
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
    parser.add_argument("--verbose-failures", action="store_true")
    parser.add_argument(
        "--capture-dir", default=None,
        help="Save per-process Tracy captures (.tracy) to this directory. "
             "Filenames include git short hash and timestamp. "
             "Requires tracy-capture.exe in bin/<build>/tools/.",
    )
    parser.add_argument(
        "--capture-procs",
        default="loginapp,dbapp,baseappmgr,baseapp,cellappmgr,cellapp",
        help="Comma-separated server process names to capture (default: all six).",
    )

    # Phase C2/C3 — real atlas_client.exe subprocesses loaded with the
    # ClientSample assembly. world_stress orchestrates them alongside its
    # raw-protocol virtual clients and parses per-child stdout events.
    # See docs/script_client_smoke.md for the scenarios these flags enable.
    parser.add_argument("--script-clients", type=int, default=0,
                        help="Spawn N real atlas_client.exe subprocesses "
                             "alongside virtual clients (script_client_smoke.md)")
    parser.add_argument("--client-exe", default=None,
                        help="Path to atlas_client.exe. Defaults to "
                             "<build-dir>/src/client/<config>/atlas_client.exe")
    parser.add_argument("--client-assembly", default=None,
                        help="Path to Atlas.ClientSample.dll. Defaults to "
                             "samples/client/bin/<config>/net9.0/Atlas.ClientSample.dll")
    parser.add_argument("--client-runtime-config", default=None,
                        help="Optional hostfxr *.runtimeconfig.json forwarded to each child")
    parser.add_argument("--script-username-prefix", default="script_user_")
    parser.add_argument("--script-verify", action="store_true",
                        help="Fail the orchestrator run if any script child didn't observe OnInit")
    parser.add_argument("--client-drop-inbound-ms", nargs=2, type=int, metavar=("START", "DURATION"),
                        default=None,
                        help="Forward atlas_client --drop-inbound-ms (app-level drop of "
                             "state-channel messages; script_client_smoke.md 场景 2)")
    parser.add_argument("--client-drop-transport-ms", nargs=2, type=int,
                        metavar=("START", "DURATION"), default=None,
                        help="Forward atlas_client --drop-transport-ms (RUDP-layer drop; "
                             "reliable retransmit recovers; script_client_smoke.md 场景 3)")
    return parser.parse_args()


def log(message: str, *, stream: object = sys.stdout) -> None:
    print(message, file=stream, flush=True)


def fail(message: str) -> NoReturn:
    raise RuntimeError(message)


def assert_file_exists(path: Path, label: str) -> None:
    if not path.exists():
        fail(f"{label} not found: {path}")


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

    # P1: clients=0 means cluster-only smoke test; no stress workers. Phase
    # C2 relaxes this: if --script-clients > 0 we still need a single stress
    # worker shard to carry the script children even though there are zero
    # virtual clients.
    if args.clients <= 0 and args.script_clients <= 0:
        return []

    total_workers = args.worker_count * args.local_workers
    base_worker_index = args.worker_index * args.local_workers

    workers: list[dict[str, object]] = []
    for local_index in range(args.local_workers):
        global_worker_index = base_worker_index + local_index
        client_offset, client_count = split_range(args.clients, total_workers, global_worker_index)
        # The primary shard owns the script-client fleet and must be
        # scheduled even with zero virtual clients; other shards fall through
        # when their slice is empty.
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
    """Convert PascalCase config name to snake_case: Debug -> debug,
    RelWithDebInfo -> rel_with_deb_info."""
    import re
    return re.sub(r"([a-z])([A-Z])", r"\1_\2", config).lower()


def _exe_suffixes() -> list[str]:
    return [".exe", ""] if os.name == "nt" else ["", ".exe"]


def _dll_env(exe_path: Path) -> dict[str, str]:
    """Return env dict with the sibling server/ directory prepended to PATH.

    On Windows, DLL resolution starts from the executable's own directory.
    Executables in tools/ share DLL dependencies with server/ (mimalloc,
    TracyClient, atlas_engine), so we prepend server/ to PATH for any
    process that lives outside that directory.
    """
    env = os.environ.copy()
    server_dir = str(exe_path.parent.parent / "server")
    path_sep = ";" if os.name == "nt" else ":"
    env["PATH"] = server_dir + path_sep + env.get("PATH", "")
    return env


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
            # crude grep for pid in ss output — good enough for dev use
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
        log(f"[capture] {proc_entry.name} pid={pid} port={port} → {out_file.name}")
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
    """Wait for tracy-capture processes to finish writing, then force-kill stragglers.

    tracy-capture flushes the .tracy file only on clean exit; hard-killing it
    loses the capture.  We give it 20 s to finish (it should be nearly done
    since -s was set to duration+10), then force-kill anything still running.
    """
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
    """Locate an executable under bin/<bin_name>/<subdir>/.

    bin_name is the last path component of the CMake binary directory
    (e.g. "profile-release", "debug") — Atlas's AtlasOutputDirectory.cmake
    routes all artifacts into bin/<build_dir_name>/, not bin/<config_snake>/.
    """
    bin_base = build_root / "bin" / bin_name
    for subdir in subdirs:
        for suffix in _exe_suffixes():
            candidate = bin_base / subdir / f"{stem}{suffix}"
            if candidate.exists():
                return candidate
    return bin_base / "server" / f"{stem}{'.exe' if os.name == 'nt' else ''}"


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
    for entry in sorted(processes, key=lambda item: item.start_order, reverse=True):
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
        "50",
        "--log-level",
        "info",
    ]
    extend_repeated_flag(
        loginapp_args, "--login-rate-limit-trusted-cidr", args.login_rate_limit_trusted_cidr
    )
    return loginapp_args


def build_stress_args(args: argparse.Namespace, worker: dict[str, object]) -> list[str]:
    stress_args = [
        "--login",
        f"{args.machined_host}:{args.login_port}",
        "--password-hash",
        args.password_hash,
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
        "--space-count",
        str(args.space_count),
    ]
    extend_repeated_flag(stress_args, "--source-ip", worker["source_ips"])
    if args.verbose_failures:
        stress_args.append("--verbose-failures")

    # Phase C2/C3 pass-through. Only the first worker shard carries the
    # script-client fleet: launching the same children from every shard would
    # multiply the subprocess count and all children would race for the same
    # username pool. Downstream shards fall back to zero script-clients even
    # when --script-clients is set on the command line.
    is_primary_worker = int(worker["global_worker_index"]) == 0
    if args.script_clients > 0 and is_primary_worker:
        stress_args.extend(["--script-clients", str(args.script_clients)])
        client_exe = args.client_exe or default_client_exe(args)
        client_assembly = args.client_assembly or default_client_assembly(args)
        stress_args.extend(["--client-exe", str(client_exe)])
        stress_args.extend(["--client-assembly", str(client_assembly)])
        if args.client_runtime_config:
            stress_args.extend(["--client-runtime-config", str(args.client_runtime_config)])
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
    return stress_args


def default_client_exe(args: argparse.Namespace) -> Path:
    bin_name = Path(args.build_dir).name
    return resolve_repo_root() / "bin" / bin_name / "client" / "atlas_client.exe"


def default_client_assembly(args: argparse.Namespace) -> Path:
    # The ClientSample csproj does not set AppendPlatformToOutputPath, so
    # `dotnet build` drops the assembly under bin/<Config>/net9.0/ (no x64
    # segment). Keep the default aligned with reality.
    return (resolve_repo_root() / "samples" / "client" / "bin" / args.config
            / "net9.0" / "Atlas.ClientSample.dll")


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


def main() -> int:
    args = parse_args()
    source_ips = collect_source_ips(args)
    worker_plan = build_worker_plan(args, source_ips)
    baseapp_specs = build_baseapp_specs(args)
    cellapp_specs = build_cellapp_specs(args)

    repo_root = resolve_repo_root()
    runtime_config = repo_root / "runtime" / "atlas_server.runtimeconfig.json"

    # bin/ path is keyed on the build directory name, not the CMake config name.
    # AtlasOutputDirectory.cmake routes all artifacts into bin/<build_dir_name>/.
    bin_name = Path(args.build_dir).name
    bin_base = repo_root / "bin" / bin_name

    # C# assemblies deployed by CMake into bin/<bin_name>/tools/.
    base_assembly = bin_base / "tools" / "Atlas.StressTest.Base.dll"
    cell_assembly = bin_base / "tools" / "Atlas.StressTest.Cell.dll"

    # Subdirectories to search for executables.
    search_subdirs = ["server", "tools"]

    atlas_tool = resolve_program(repo_root, bin_name, search_subdirs, "atlas_tool")
    machined = resolve_program(repo_root, bin_name, search_subdirs, "machined")
    loginapp = resolve_program(repo_root, bin_name, search_subdirs, "atlas_loginapp")
    baseapp = resolve_program(repo_root, bin_name, search_subdirs, "atlas_baseapp")
    baseappmgr = resolve_program(repo_root, bin_name, search_subdirs, "atlas_baseappmgr")
    dbapp = resolve_program(repo_root, bin_name, search_subdirs, "atlas_dbapp")
    cellapp = resolve_program(repo_root, bin_name, search_subdirs, "atlas_cellapp")
    cellappmgr = resolve_program(repo_root, bin_name, search_subdirs, "atlas_cellappmgr")

    assert_file_exists(machined, machined.name)
    assert_file_exists(loginapp, loginapp.name)
    assert_file_exists(baseapp, baseapp.name)
    assert_file_exists(baseappmgr, baseappmgr.name)
    assert_file_exists(dbapp, dbapp.name)
    assert_file_exists(cellapp, cellapp.name)
    assert_file_exists(cellappmgr, cellappmgr.name)
    assert_file_exists(atlas_tool, atlas_tool.name)
    assert_file_exists(runtime_config, runtime_config.name)
    assert_file_exists(base_assembly, base_assembly.name)
    assert_file_exists(cell_assembly, cell_assembly.name)

    # world_stress is only required when clients > 0.
    world_stress: Path | None = None
    if worker_plan:
        world_stress = resolve_program(
            repo_root, bin_name, search_subdirs, "world_stress"
        )
        assert_file_exists(world_stress, world_stress.name)

    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    git_hash = _git_short(repo_root)
    run_root = repo_root / ".tmp" / "world-stress" / timestamp
    log_dir = run_root / "logs"
    db_dir = run_root / "db"
    db_config_path = run_root / "dbapp.json"

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
                    "50",
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
                    "50",
                    "--log-level",
                    "info",
                ],
            )
        )
        processes[-1].start_order = 3
        time.sleep(1)

        processes.append(
            start_logged_process(
                name="cellappmgr",
                file_path=cellappmgr,
                working_directory=repo_root,
                log_directory=log_dir,
                arguments=[
                    "--type",
                    "cellappmgr",
                    "--name",
                    "cellappmgr",
                    "--machined",
                    machined_address,
                    "--internal-port",
                    str(args.cellappmgr_port),
                    "--update-hertz",
                    "50",
                    "--log-level",
                    "info",
                ],
            )
        )
        processes[-1].start_order = 4
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
                        "50",
                        "--log-level",
                        "info",
                    ],
                )
            )
            processes[-1].start_order = 5
        time.sleep(1)

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
                        "10",
                        "--log-level",
                        "info",
                    ],
                )
            )
            processes[-1].start_order = 6
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
            subprocess.run(
                [str(atlas_tool), "--machined", machined_address, "list"],
                cwd=repo_root,
                check=False,
                env=_dll_env(atlas_tool),
            )
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
                stress_result = subprocess.run(
                    [str(world_stress), *build_stress_args(args, worker)],
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
                        stress_workers.append(
                            start_logged_process(
                                name=name,
                                file_path=world_stress,
                                arguments=build_stress_args(args, worker),
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

        log("")
        log("Run artifacts:")
        log(f"  logs: {log_dir}")
        log(f"  db:   {db_dir}")

        if args.keep_cluster:
            log("Keeping cluster alive. Stop the spawned processes manually when done.")
            processes = []

        return 0 if not missing else 2
    finally:
        if processes:
            stop_logged_processes(processes)


if __name__ == "__main__":
    sys.exit(main())
