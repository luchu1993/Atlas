#!/usr/bin/env python3
"""Build on Linux via WSL using the Windows-side source tree (no sync).
Re-invokes via WSL when run from Windows; build artefacts land in $HOME."""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from common.paths import REPO_ROOT
from common import wsl


PRESET_CONFIGS = {
    "debug":   "Debug",
    "release": "Release",
    "profile": "RelWithDebInfo",
}


def log(msg: str) -> None:
    print(f"[build-linux] {msg}", flush=True)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Build on Linux (via WSL on Windows) using the in-place source tree.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Build dir lives in WSL filesystem (default ~/atlas-builds/<preset>;\n"
            "override via $ATLAS_LINUX_BUILD_DIR). Source is read in place from\n"
            "/mnt/<drive>/... — no sync needed.\n\n"
            "Pass --with-tests to also run ctest after build (debug preset only;\n"
            "release/profile have ATLAS_BUILD_TESTS=OFF)."
        ),
    )
    p.add_argument(
        "preset", choices=sorted(PRESET_CONFIGS), nargs="?", default="debug",
        help="CMake preset (default: debug).",
    )
    p.add_argument(
        "--clean", action="store_true",
        help="Wipe the WSL build dir before configuring.",
    )
    p.add_argument(
        "--with-tests", action="store_true",
        help="Run ctest after build (debug preset only).",
    )
    p.add_argument(
        "--label", default="unit",
        help="ctest --label-regex when --with-tests is set (default: unit).",
    )
    return p.parse_args()


def cmake_configure_args(preset: str, src: Path, build: Path) -> list[str]:
    base = [
        "cmake", "-S", str(src), "-B", str(build),
        f"-DCMAKE_BUILD_TYPE={PRESET_CONFIGS[preset]}",
    ]
    if shutil.which("sccache"):
        base += ["-DCMAKE_C_COMPILER_LAUNCHER=sccache",
                 "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache"]
    if preset == "debug":
        return base + ["-G", "Ninja Multi-Config",
                       "-DCMAKE_DEFAULT_BUILD_TYPE=Debug"]
    if preset == "release":
        return base + ["-DATLAS_ENABLE_PROFILER=OFF",
                       "-DATLAS_BUILD_TESTS=OFF"]
    return base + ["-DATLAS_ENABLE_PROFILER=ON",
                   "-DATLAS_BUILD_TRACY_VIEWER=ON",
                   "-DATLAS_BUILD_TESTS=OFF"]


def run_on_linux(args: argparse.Namespace) -> int:
    config = PRESET_CONFIGS[args.preset]
    src = REPO_ROOT
    build_root = Path(os.environ.get(
        "ATLAS_LINUX_BUILD_DIR", str(Path.home() / "atlas-builds")))
    build = build_root / args.preset

    log(f"source: {src}")
    log(f"build:  {build}")

    if args.clean and build.exists():
        log(f"wiping {build}")
        shutil.rmtree(build)
    build.mkdir(parents=True, exist_ok=True)

    t0 = time.monotonic()

    configure = cmake_configure_args(args.preset, src, build)
    log(" ".join(configure))
    subprocess.run(configure, check=True)

    build_cmd = ["cmake", "--build", str(build), "--config", config]
    log(" ".join(build_cmd))
    subprocess.run(build_cmd, check=True)

    if shutil.which("sccache"):
        subprocess.run(["sccache", "--show-stats"], check=False)

    if not args.with_tests:
        log(f"done in {time.monotonic() - t0:.1f}s")
        return 0

    if args.preset != "debug":
        log(f"--with-tests ignored: {args.preset} preset has ATLAS_BUILD_TESTS=OFF")
        log(f"done in {time.monotonic() - t0:.1f}s")
        return 0

    test_cmd = ["ctest", "--build-config", config,
                "--label-regex", args.label, "--output-on-failure"]
    log(" ".join(test_cmd))
    subprocess.run(test_cmd, cwd=build, check=True)

    log(f"done in {time.monotonic() - t0:.1f}s")
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
