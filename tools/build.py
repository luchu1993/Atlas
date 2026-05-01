#!/usr/bin/env python3
"""Atlas one-shot configure + build helper.

Wraps cmake configure/build with:
  - MSVC environment auto-load via vswhere + vcvars64.bat (Windows only).
  - Ninja auto-provisioning to .tmp/ninja/ when missing from PATH.

Run via the platform wrapper:
  Windows: tools\\build.bat <preset> [options]
  Linux:   tools/build.sh   <preset> [options]
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import stat
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common.paths import REPO_ROOT

PRESET_CONFIGS = {
    "debug": "Debug",
    "profile": "RelWithDebInfo",
    "release": "Release",
}

NINJA_VERSION = "1.12.1"
NINJA_URLS = {
    "Windows": f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-win.zip",
    "Linux":   f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-linux.zip",
    "Darwin":  f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-mac.zip",
}


def log(msg: str) -> None:
    print(f"[build] {msg}", flush=True)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Atlas one-shot configure + build.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Presets:\n"
            "  debug    Ninja Multi-Config, Debug    (fast dev iteration, /Z7 + PCH)\n"
            "  profile  RelWithDebInfo + Tracy + viewer (no tests)\n"
            "  release  Release (no tests)"
        ),
    )
    p.add_argument(
        "preset",
        choices=sorted(PRESET_CONFIGS),
        help="CMake preset to use.",
    )
    p.add_argument(
        "--clean",
        action="store_true",
        help="Wipe build/<preset> before configuring.",
    )
    g = p.add_mutually_exclusive_group()
    g.add_argument("--config-only", action="store_true",
                   help="Run cmake configure, skip build.")
    g.add_argument("--build-only",  action="store_true",
                   help="Skip configure, just build.")
    return p.parse_args()


def load_msvc_env() -> dict[str, str]:
    """Return env with MSVC tooling loaded; pass-through if cl.exe is on PATH."""
    if shutil.which("cl.exe"):
        return os.environ.copy()

    pf86 = os.environ.get("ProgramFiles(x86)") or r"C:\Program Files (x86)"
    vswhere = Path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        raise RuntimeError(f"vswhere.exe not found at {vswhere}")

    vs_path = subprocess.run(
        [str(vswhere), "-latest", "-property", "installationPath"],
        capture_output=True, check=True,
    ).stdout.decode(errors="replace").strip()
    if not vs_path:
        raise RuntimeError("No Visual Studio installation detected via vswhere.")

    vcvars = Path(vs_path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not vcvars.is_file():
        raise RuntimeError(f"vcvars64.bat not found at {vcvars}")

    log(f"Loading MSVC env via {vcvars.name}")
    # Run vcvars then dump env vars; "set" prints NAME=VALUE per line.
    res = subprocess.run(
        f'"{vcvars}" >nul && set',
        capture_output=True, shell=True, check=True,
    )
    env = os.environ.copy()
    for line in res.stdout.decode(errors="replace").splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            env[key] = value
    return env


def ensure_ninja(env: dict[str, str]) -> None:
    """Make sure 'ninja' resolves on env['PATH']; download to .tmp/ninja/ if not."""
    if shutil.which("ninja", path=env.get("PATH")):
        return

    system = platform.system()
    url = NINJA_URLS.get(system)
    if url is None:
        raise RuntimeError(f"No Ninja download URL for platform {system}")

    ninja_dir = REPO_ROOT / ".tmp" / "ninja"
    ninja_exe_name = "ninja.exe" if system == "Windows" else "ninja"
    ninja_exe = ninja_dir / ninja_exe_name

    if not ninja_exe.is_file():
        ninja_dir.mkdir(parents=True, exist_ok=True)
        zip_path = ninja_dir / "ninja.zip"
        log(f"Downloading Ninja v{NINJA_VERSION} ({system}) -> {ninja_dir}")
        urllib.request.urlretrieve(url, zip_path)
        with zipfile.ZipFile(zip_path) as zf:
            zf.extractall(ninja_dir)
        zip_path.unlink()
        if system != "Windows":
            ninja_exe.chmod(ninja_exe.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    env["PATH"] = f"{ninja_dir}{os.pathsep}{env.get('PATH', '')}"
    log(f"Ninja: {ninja_exe}")


def run_cmake(args: list[str], env: dict[str, str]) -> None:
    log(" ".join(args))
    subprocess.run(args, env=env, cwd=REPO_ROOT, check=True)


def main() -> int:
    args = parse_args()
    config = PRESET_CONFIGS[args.preset]

    if platform.system() == "Windows":
        env = load_msvc_env()
    else:
        env = os.environ.copy()

    # Only the debug preset uses Ninja Multi-Config; profile/release stay on
    # the default generator (VS solution on Windows, Make on Linux).
    if args.preset == "debug":
        ensure_ninja(env)

    build_dir = REPO_ROOT / "build" / args.preset

    if args.clean and build_dir.exists():
        log(f"Wiping build/{args.preset}")
        shutil.rmtree(build_dir)

    t_start = time.monotonic()

    if not args.build_only:
        run_cmake(["cmake", "--preset", args.preset], env)

    if not args.config_only:
        run_cmake(
            ["cmake", "--build", str(build_dir), "--config", config],
            env,
        )

    log(f"Done in {time.monotonic() - t_start:.1f}s")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        log(f"command failed (exit {e.returncode}): {' '.join(map(str, e.cmd))}")
        sys.exit(e.returncode)
    except Exception as e:
        log(f"FAILED: {e}")
        sys.exit(1)
