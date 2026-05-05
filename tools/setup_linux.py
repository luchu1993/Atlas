#!/usr/bin/env python3
"""Install the Linux toolchain Atlas needs (mirrors CI's build-linux.yml).
Re-invokes via WSL when run from Windows; idempotent — safe to re-run."""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import wsl


APT_BASE = [
    "build-essential",
    "g++-13",
    "gcc-13",
    "ninja-build",
    "cmake",
    "git",
    "python3",
    "curl",
    "ca-certificates",
    "pkg-config",
    "libssl-dev",
    "gdb",
    "rsync",
    "lsb-release",
    "sccache",
]

APT_DOTNET = ["dotnet-sdk-10.0"]
APT_CLANG_FORMAT = ["clang-format"]


def log(msg: str) -> None:
    print(f"[setup-linux] {msg}", flush=True)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Install the Linux toolchain Atlas needs (g++-13, Ninja, .NET 10, ...).",
        epilog="Idempotent — safe to re-run after upgrades. Re-invokes via WSL on Windows.",
    )
    p.add_argument("--no-clang-format", action="store_true",
                   help="Skip clang-format (otherwise needed to pass pre-commit lint).")
    p.add_argument("--no-dotnet", action="store_true",
                   help="Skip .NET 10 SDK (otherwise needed for C# scripting + tests).")
    return p.parse_args()


def sudo_prefix() -> list[str]:
    if os.geteuid() == 0:
        return []
    if shutil.which("sudo") is None:
        raise RuntimeError("not root and 'sudo' not found")
    return ["sudo"]


def run(cmd: list[str], **kw) -> None:
    log(" ".join(cmd))
    subprocess.run(cmd, check=True, **kw)


def check_distro() -> None:
    try:
        out = subprocess.run(
            ["lsb_release", "-rs"], capture_output=True, text=True, check=True,
        ).stdout.strip()
    except (FileNotFoundError, subprocess.CalledProcessError):
        log("warning: cannot detect distro version (lsb_release missing)")
        return
    if out != "26.04":
        log(f"warning: Ubuntu 26.04 expected, found {out} — package versions may differ from CI")


def ensure_inotify_limit(sudo: list[str]) -> None:
    conf = Path("/etc/sysctl.d/99-atlas-inotify.conf")
    if conf.exists():
        return
    log(f"writing {conf} (raises inotify watcher limit for VS Code Remote-WSL)")
    subprocess.run(
        sudo + ["tee", str(conf)],
        input="fs.inotify.max_user_watches=524288\n",
        text=True, capture_output=True, check=True,
    )
    run(sudo + ["sysctl", "-p", str(conf)])


def run_on_linux(args: argparse.Namespace) -> int:
    check_distro()

    sudo = sudo_prefix()

    pkgs = list(APT_BASE)
    if not args.no_dotnet:
        pkgs += APT_DOTNET
    if not args.no_clang_format:
        pkgs += APT_CLANG_FORMAT

    if sudo:
        run(sudo + ["-v"])
    run(sudo + ["apt-get", "update"])
    run(sudo + ["apt-get", "install", "-y"] + pkgs)

    for tool in ("g++", "gcc"):
        run(sudo + ["update-alternatives", "--install",
                    f"/usr/bin/{tool}", tool, f"/usr/bin/{tool}-13", "100"])

    ensure_inotify_limit(sudo)

    log("done. sanity-check: g++ --version && cmake --version && ninja --version"
        + (" && dotnet --info" if not args.no_dotnet else ""))
    return 0


def main() -> int:
    args = parse_args()
    if platform.system() == "Windows":
        return wsl.reinvoke_python(Path(__file__))
    return run_on_linux(args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        log(f"command failed (exit {e.returncode}): {' '.join(map(str, e.cmd))}")
        sys.exit(e.returncode)
    except Exception as e:
        log(f"FAILED: {e}")
        sys.exit(1)
