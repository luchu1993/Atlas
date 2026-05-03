#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common.cluster import (
    LoggedProcess,
    resolve_program,
    start_logged_process,
    stop_logged_processes,
    wait_for_registration,
)
from common.paths import resolve_repo_root


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Spawn a minimal Atlas cluster (machined + dbapp + baseappmgr + "
        "loginapp + baseapp) on 127.0.0.1 for integration testing. Outputs a "
        "READY line to stdout once every process has registered, then blocks "
        "until stdin is closed before tearing the cluster down.",
    )
    parser.add_argument("--build-dir", default="debug",
                        help="Subdirectory of bin/ where binaries live (default: debug).")
    parser.add_argument("--port-base", type=int, default=0,
                        help="Lowest port to use; 0 = auto-pick a free range.")
    parser.add_argument("--registration-timeout-sec", type=int, default=20)
    return parser.parse_args()


# Cluster needs five contiguous ports (machined, dbapp, baseappmgr, baseapp,
# loginapp). Reserve a 16-slot block to give downstream tools elbow room.
_PORT_BLOCK_SIZE = 16


def _pick_free_port_base() -> int:
    # Bind a UDP socket to port 0; OS hands back an unused port; release it.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


def main() -> int:
    args = parse_args()
    port_base = args.port_base if args.port_base > 0 else _pick_free_port_base()

    machined_port = port_base
    dbapp_port = port_base + 1
    baseappmgr_port = port_base + 2
    baseapp_internal_port = port_base + 3
    baseapp_external_port = port_base + 4
    loginapp_port = port_base + 5

    repo_root = resolve_repo_root()
    bin_name = args.build_dir
    runtime_config = repo_root / "runtime" / "atlas_server.runtimeconfig.json"
    runtime_assembly = repo_root / "bin" / bin_name / "Atlas.Runtime.dll"

    machined = resolve_program(repo_root, bin_name, [], "machined")
    loginapp = resolve_program(repo_root, bin_name, [], "atlas_loginapp")
    baseapp = resolve_program(repo_root, bin_name, [], "atlas_baseapp")
    baseappmgr = resolve_program(repo_root, bin_name, [], "atlas_baseappmgr")
    dbapp = resolve_program(repo_root, bin_name, [], "atlas_dbapp")
    atlas_tool = resolve_program(repo_root, bin_name, [], "atlas_tool")

    for prog in (machined, loginapp, baseapp, baseappmgr, dbapp, atlas_tool,
                 runtime_config, runtime_assembly):
        if not prog.exists():
            print(f"missing: {prog}", file=sys.stderr, flush=True)
            return 1

    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    run_root = repo_root / ".tmp" / "test_cluster" / timestamp
    log_dir = run_root / "logs"
    db_dir = run_root / "db"
    db_config_path = run_root / "dbapp.json"

    log_dir.mkdir(parents=True, exist_ok=True)
    db_dir.mkdir(parents=True, exist_ok=True)

    machined_address = f"127.0.0.1:{machined_port}"
    db_config_path.write_text(
        json.dumps({
            "machined_address": machined_address,
            "auto_create_accounts": True,
            "account_type_id": 1,
            "database": {
                "type": "sqlite",
                "sqlite_path": str(db_dir / "test_cluster.sqlite3"),
                "sqlite_foreign_keys": True,
            },
        }, indent=2) + "\n",
        encoding="utf-8",
    )

    processes: list[LoggedProcess] = []

    try:
        processes.append(start_logged_process(
            name="machined", file_path=machined,
            working_directory=repo_root, log_directory=log_dir,
            arguments=["--type", "machined", "--name", "machined",
                       "--internal-port", str(machined_port), "--log-level", "info"]))
        processes[-1].start_order = 1
        time.sleep(1)

        processes.append(start_logged_process(
            name="dbapp", file_path=dbapp,
            working_directory=repo_root, log_directory=log_dir,
            arguments=["--type", "dbapp", "--name", "dbapp",
                       "--machined", machined_address,
                       "--internal-port", str(dbapp_port),
                       "--config", str(db_config_path),
                       "--update-hertz", "50", "--log-level", "info"]))
        processes[-1].start_order = 2

        processes.append(start_logged_process(
            name="baseappmgr", file_path=baseappmgr,
            working_directory=repo_root, log_directory=log_dir,
            arguments=["--type", "baseappmgr", "--name", "baseappmgr",
                       "--machined", machined_address,
                       "--internal-port", str(baseappmgr_port),
                       "--update-hertz", "50", "--log-level", "info"]))
        processes[-1].start_order = 3

        processes.append(start_logged_process(
            name="baseapp", file_path=baseapp,
            working_directory=repo_root, log_directory=log_dir,
            arguments=["--type", "baseapp", "--name", "baseapp",
                       "--machined", machined_address,
                       "--internal-port", str(baseapp_internal_port),
                       "--external-port", str(baseapp_external_port),
                       "--assembly", str(runtime_assembly),
                       "--runtime-config", str(runtime_config),
                       "--update-hertz", "50", "--log-level", "info"]))
        processes[-1].start_order = 4

        processes.append(start_logged_process(
            name="loginapp", file_path=loginapp,
            working_directory=repo_root, log_directory=log_dir,
            arguments=["--type", "loginapp", "--name", "loginapp",
                       "--machined", machined_address,
                       "--external-port", str(loginapp_port),
                       "--auto-create-accounts", "true",
                       "--update-hertz", "50", "--log-level", "info"]))
        processes[-1].start_order = 5

        ready_targets = [
            ("dbapp", "dbapp"),
            ("baseappmgr", "baseappmgr"),
            ("baseapp", "baseapp"),
            ("loginapp", "loginapp"),
        ]
        for proc_type, name in ready_targets:
            if not wait_for_registration(
                atlas_tool=atlas_tool, machined_address=machined_address,
                proc_type=proc_type, name=name,
                timeout_sec=args.registration_timeout_sec,
            ):
                print(f"timeout waiting for {name}", file=sys.stderr, flush=True)
                return 2

        # Single-line READY marker; consumers parse `key=value` pairs.
        print(
            f"READY loginapp_port={loginapp_port} baseapp_external_port={baseapp_external_port} "
            f"machined_port={machined_port} log_dir={log_dir}",
            flush=True,
        )

        # Block until stdin closes (parent disposes / CTRL-C). readline returns
        # '' on EOF; CTRL-C raises KeyboardInterrupt which falls through to finally.
        for _ in iter(sys.stdin.readline, ""):
            pass
        return 0
    except KeyboardInterrupt:
        return 0
    finally:
        stop_logged_processes(processes)


if __name__ == "__main__":
    sys.exit(main())
