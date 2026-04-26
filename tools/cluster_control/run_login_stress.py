#!/usr/bin/env python3

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
    parser = argparse.ArgumentParser(description="Bring up a local Atlas cluster and run login_stress.")
    parser.add_argument("--build-dir", default="build/debug-windows")
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
    parser.add_argument("--clients", type=int, default=100)
    parser.add_argument("--account-pool", type=int, default=50)
    parser.add_argument("--account-index-base", type=int, default=0)
    parser.add_argument("--ramp-per-sec", type=int, default=100)
    parser.add_argument("--duration-sec", type=int, default=60)
    parser.add_argument("--shortline-pct", type=int, default=20)
    parser.add_argument("--shortline-min-ms", type=int, default=1000)
    parser.add_argument("--shortline-max-ms", type=int, default=5000)
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


def assign_worker_source_ips(source_ips: list[str], worker_index: int, worker_count: int) -> list[str]:
    if not source_ips:
        return []

    assigned = source_ips[worker_index::worker_count]
    if assigned:
        return assigned
    return [source_ips[worker_index % len(source_ips)]]


def build_worker_plan(args: argparse.Namespace, source_ips: list[str]) -> list[dict[str, object]]:
    if args.baseapp_count <= 0:
        fail("--baseapp-count must be >= 1")
    if args.baseapp_internal_port_stride <= 0:
        fail("--baseapp-internal-port-stride must be >= 1")
    if args.baseapp_external_port_stride <= 0:
        fail("--baseapp-external-port-stride must be >= 1")
    if args.local_workers <= 0:
        fail("--local-workers must be >= 1")
    if args.worker_count <= 0:
        fail("--worker-count must be >= 1")
    if args.worker_index < 0 or args.worker_index >= args.worker_count:
        fail("--worker-index must be in [0, worker-count)")

    total_workers = args.worker_count * args.local_workers
    base_worker_index = args.worker_index * args.local_workers

    workers: list[dict[str, object]] = []
    for local_index in range(args.local_workers):
        global_worker_index = base_worker_index + local_index
        client_offset, client_count = split_range(args.clients, total_workers, global_worker_index)
        if client_count <= 0:
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
                "source_ips": assign_worker_source_ips(source_ips, global_worker_index, total_workers),
            }
        )

    if not workers:
        fail("no stress workers were scheduled; increase --clients or lower worker count")
    return workers


def _config_to_snake(config: str) -> str:
    """Convert PascalCase config name to snake_case."""
    import re
    return re.sub(r"([a-z])([A-Z])", r"\1_\2", config).lower()


def resolve_program(repo_root: Path, bin_name: str, subdirs: Iterable[str], stem: str) -> Path:
    """Locate an executable under bin/<bin_name>/<subdir>/."""
    bin_base = repo_root / "bin" / bin_name
    suffixes = [".exe", ""] if os.name == "nt" else ["", ".exe"]
    for subdir in subdirs:
        for suffix in suffixes:
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
        **popen_kwargs,
    )

    # Close the parent's copy of the file handles.  The child already has
    # its own inherited copy and will keep writing to it.  Closing here
    # prevents handle leakage to subsequent Popen() calls on Windows
    # (bInheritHandles=TRUE inherits ALL open inheritable handles).
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
                    # CTRL_BREAK_EVENT triggers a graceful shutdown
                    # (fini → flush → exit) instead of hard-killing with
                    # TerminateProcess.  Works because the child was
                    # started with CREATE_NEW_PROCESS_GROUP.
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
    env = os.environ.copy()
    server_dir = str(atlas_tool.parent.parent / "server")
    path_sep = ";" if os.name == "nt" else ":"
    env["PATH"] = server_dir + path_sep + env.get("PATH", "")

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
    sqlite_path = db_dir / "atlas_login_stress.sqlite3"
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
    ]
    extend_repeated_flag(stress_args, "--source-ip", worker["source_ips"])
    if args.verbose_failures:
        stress_args.append("--verbose-failures")
    return stress_args


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


def main() -> int:
    args = parse_args()
    source_ips = collect_source_ips(args)
    worker_plan = build_worker_plan(args, source_ips)
    baseapp_specs = build_baseapp_specs(args)

    repo_root = resolve_repo_root()
    bin_name = Path(args.build_dir).name
    bin_base = repo_root / "bin" / bin_name
    runtime_config = repo_root / "runtime" / "atlas_server.runtimeconfig.json"
    runtime_assembly = bin_base / "server" / "Atlas.Runtime.dll"

    search_subdirs = ["server", "tools"]
    atlas_tool = resolve_program(repo_root, bin_name, search_subdirs, "atlas_tool")
    login_stress = resolve_program(repo_root, bin_name, search_subdirs, "login_stress")
    machined = resolve_program(repo_root, bin_name, search_subdirs, "machined")
    loginapp = resolve_program(repo_root, bin_name, search_subdirs, "atlas_loginapp")
    baseapp = resolve_program(repo_root, bin_name, search_subdirs, "atlas_baseapp")
    baseappmgr = resolve_program(repo_root, bin_name, search_subdirs, "atlas_baseappmgr")
    dbapp = resolve_program(repo_root, bin_name, search_subdirs, "atlas_dbapp")

    assert_file_exists(machined, machined.name)
    assert_file_exists(loginapp, loginapp.name)
    assert_file_exists(baseapp, baseapp.name)
    assert_file_exists(baseappmgr, baseappmgr.name)
    assert_file_exists(dbapp, dbapp.name)
    assert_file_exists(atlas_tool, atlas_tool.name)
    assert_file_exists(login_stress, login_stress.name)
    assert_file_exists(runtime_config, runtime_config.name)
    assert_file_exists(runtime_assembly, runtime_assembly.name)

    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    run_root = repo_root / ".tmp" / "login-stress" / timestamp
    log_dir = run_root / "logs"
    db_dir = run_root / "db"
    db_config_path = run_root / "dbapp.json"

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
        processes[-1].start_order = 5
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
                        str(runtime_assembly),
                        "--runtime-config",
                        str(runtime_config),
                        "--update-hertz",
                        "50",
                        "--log-level",
                        "info",
                    ],
                )
            )
            processes[-1].start_order = 4
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

        log("Waiting for processes to register with machined...")
        registrations = [
            wait_for_registration(
                atlas_tool=atlas_tool,
                machined_address=machined_address,
                proc_type="dbapp",
                name="dbapp",
            ),
            wait_for_registration(
                atlas_tool=atlas_tool,
                machined_address=machined_address,
                proc_type="baseappmgr",
                name="baseappmgr",
            ),
            *[
                wait_for_registration(
                    atlas_tool=atlas_tool,
                    machined_address=machined_address,
                    proc_type="baseapp",
                    name=str(baseapp_spec["name"]),
                )
                for baseapp_spec in baseapp_specs
            ],
            wait_for_registration(
                atlas_tool=atlas_tool,
                machined_address=machined_address,
                proc_type="loginapp",
                name="loginapp",
            ),
        ]

        if not all(registrations):
            log(
                "Warning: cluster did not fully register with machined within the timeout. "
                f"Continuing anyway; check logs under {log_dir} if login_stress fails.",
                stream=sys.stderr,
            )
        else:
            log("")
            log("Registered processes:")
            subprocess.run(
                [str(atlas_tool), "--machined", machined_address, "list"],
                cwd=repo_root,
                check=False,
            )
            log("")

        if args.local_workers == 1:
            worker = worker_plan[0]
            log(
                "Running login_stress..."
                f" worker={worker['global_worker_index']}/{worker['global_worker_count']}"
                f" clients={worker['clients']} account_pool={worker['account_pool']}"
                f" baseapps={len(baseapp_specs)}"
                f" source_ips={len(worker['source_ips'])}"
            )
            stress_result = subprocess.run(
                [str(login_stress), *build_stress_args(args, worker)], cwd=repo_root
            )
            if stress_result.returncode != 0:
                fail(f"login_stress exited with code {stress_result.returncode}")
        else:
            stress_workers: list[LoggedProcess] = []
            try:
                for ordinal, worker in enumerate(worker_plan):
                    name = f"login_stress_worker_{ordinal:02d}"
                    log(
                        f"Starting {name}: global_worker={worker['global_worker_index']}/"
                        f"{worker['global_worker_count']} clients={worker['clients']} "
                        f"account_pool={worker['account_pool']} baseapps={len(baseapp_specs)} "
                        f"source_ips={len(worker['source_ips'])}"
                    )
                    stress_workers.append(
                        start_logged_process(
                            name=name,
                            file_path=login_stress,
                            arguments=build_stress_args(args, worker),
                            working_directory=repo_root,
                            log_directory=log_dir,
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
                stop_logged_processes([worker for worker in stress_workers if worker.process.poll() is None])

        log("")
        log("Run artifacts:")
        log(f"  logs: {log_dir}")
        log(f"  db:   {db_dir}")

        if args.keep_cluster:
            log("Keeping cluster alive. Stop the spawned processes manually when done.")
            processes = []

        return 0
    finally:
        if processes:
            stop_logged_processes(processes)


if __name__ == "__main__":
    sys.exit(main())
