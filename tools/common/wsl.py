"""Helpers for re-invoking Linux tooling from Windows via WSL2."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


def is_available() -> bool:
    return shutil.which("wsl.exe") is not None


def to_wsl_path(win_path: Path) -> str:
    res = subprocess.run(
        ["wsl.exe", "wslpath", "-a", str(win_path)],
        capture_output=True, check=True, text=True,
    )
    return res.stdout.strip()


def reinvoke_python(script: Path, args: list[str] | None = None) -> int:
    """Run script under WSL's python3 with pass-through stdio; defaults to sys.argv[1:]."""
    if not is_available():
        raise RuntimeError(
            "wsl.exe not on PATH. Install WSL2: "
            "'wsl --install -d Ubuntu-24.04' (run from elevated PowerShell)."
        )
    if args is None:
        args = sys.argv[1:]
    cmd = ["wsl.exe", "-e", "python3", to_wsl_path(script.resolve()), *args]
    print(f"[wsl] -> {' '.join(cmd)}", flush=True)
    return subprocess.call(cmd)
