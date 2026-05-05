"""Helpers for re-invoking Linux tooling from Windows via WSL2."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


_SKIP_DISTROS = {
    "docker-desktop",
    "docker-desktop-data",
    "rancher-desktop",
    "rancher-desktop-data",
}


def is_available() -> bool:
    return shutil.which("wsl.exe") is not None


def is_running_inside() -> bool:
    """True when this process is running inside a WSL kernel."""
    try:
        return "microsoft" in Path("/proc/sys/kernel/osrelease").read_text().lower()
    except OSError:
        return False


def _list_distros() -> list[str]:
    res = subprocess.run(
        ["wsl.exe", "--list", "--quiet"],
        capture_output=True, check=True,
    )
    text = res.stdout.decode("utf-16-le", errors="ignore").lstrip("﻿")
    return [line.strip() for line in text.splitlines() if line.strip()]


def pick_distro() -> str:
    """Linux distro for Atlas builds; honors $ATLAS_WSL_DISTRO.
    Prefers Ubuntu/Debian (apt-based — the toolchain installer needs it).
    Skips Docker/Rancher Desktop distros: no apt, /mnt/host/<drive> mounts."""
    override = os.environ.get("ATLAS_WSL_DISTRO")
    if override:
        return override
    candidates = [n for n in _list_distros() if n.lower() not in _SKIP_DISTROS]
    if not candidates:
        raise RuntimeError(
            "no usable WSL distro found. Install one with "
            "'wsl --install -d Ubuntu-26.04' (elevated PowerShell), "
            "or override with $ATLAS_WSL_DISTRO."
        )
    for name in candidates:
        if "ubuntu" in name.lower() or "debian" in name.lower():
            return name
    return candidates[0]


def to_wsl_path(win_path: Path, distro: str | None = None) -> str:
    distro = distro or pick_distro()
    arg = win_path.as_posix() if isinstance(win_path, Path) else str(win_path).replace("\\", "/")
    res = subprocess.run(
        ["wsl.exe", "-d", distro, "wslpath", "-a", arg],
        capture_output=True, check=True, text=True,
    )
    return res.stdout.strip()


def reinvoke_python(script: Path, args: list[str] | None = None) -> int:
    """Run script under WSL's python3 with pass-through stdio; defaults to sys.argv[1:]."""
    if not is_available():
        raise RuntimeError(
            "wsl.exe not on PATH. Install WSL2: "
            "'wsl --install -d Ubuntu-26.04' (run from elevated PowerShell)."
        )
    if args is None:
        args = sys.argv[1:]
    distro = pick_distro()
    cmd = ["wsl.exe", "-d", distro, "-e", "python3",
           to_wsl_path(script.resolve(), distro), *args]
    print(f"[wsl] -> {' '.join(cmd)}", flush=True)
    return subprocess.call(cmd)
