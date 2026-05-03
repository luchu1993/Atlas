from __future__ import annotations

import os
import re
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from common.paths import resolve_repo_root


def _log(message: str, *, stream: object = sys.stdout) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", file=stream, flush=True)


def config_to_snake(config: str) -> str:
    return re.sub(r"([a-z])([A-Z])", r"\1_\2", config).lower()


def resolve_program(repo_root: Path, bin_name: str, subdirs: Iterable[str], stem: str) -> Path:
    bin_base = repo_root / "bin" / bin_name
    suffixes = [".exe", ""] if os.name == "nt" else ["", ".exe"]
    for subdir in (*subdirs, ""):
        for suffix in suffixes:
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
) -> LoggedProcess:
    stdout_path = log_directory / f"{name}.stdout.log"
    stderr_path = log_directory / f"{name}.stderr.log"

    _log(f"Starting {name}")
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

    # Parent's handles closed; child kept its own inherited copies.
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
            _log(f"Stopping {entry.name} (pid={proc.pid})")
            try:
                if os.name == "nt":
                    # CTRL_BREAK_EVENT graceful shutdown via CREATE_NEW_PROCESS_GROUP.
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
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        result = subprocess.run(
            [str(atlas_tool), "--machined", machined_address, "list", proc_type],
            capture_output=True,
            text=True,
            cwd=resolve_repo_root(),
            env=os.environ.copy(),
        )
        if result.returncode == 0 and name in result.stdout:
            return True
        time.sleep(0.5)
    return False
