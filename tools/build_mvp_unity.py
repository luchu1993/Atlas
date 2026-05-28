#!/usr/bin/env python3
"""Build the MVP Unity client from the command line."""

from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common.paths import REPO_ROOT  # noqa: E402
from common import unity as unity_helpers  # noqa: E402

TAG = "build_mvp_unity"
DEFAULT_UNITY_PROJECT = REPO_ROOT / "samples" / "mvp" / "UnityClient"
DEFAULT_LOG = REPO_ROOT / "out" / "mvp-unity" / "unity-build.log"
BUILD_METHOD = "Atlas.Mvp.Editor.MvpUnityBuild.BuildFromCommandLine"
TARGETS = ("StandaloneWindows64", "StandaloneLinux64", "StandaloneOSX")
HOST = platform.system()


def info(msg: str) -> None:
    unity_helpers.info(TAG, msg)


def fail(msg: str, code: int = 1) -> None:
    unity_helpers.fail(TAG, msg, code)


def run(cmd: list[str]) -> None:
    info(" ".join(cmd))
    subprocess.run(cmd, check=True)


def default_target() -> str:
    if HOST == "Windows":
        return "StandaloneWindows64"
    if HOST == "Darwin":
        return "StandaloneOSX"
    return "StandaloneLinux64"


def default_output(target: str) -> Path:
    root = REPO_ROOT / "out" / "mvp-unity"
    if target == "StandaloneWindows64":
        return root / "windows" / "AtlasMvp.exe"
    if target == "StandaloneLinux64":
        return root / "linux" / "AtlasMvp.x86_64"
    if target == "StandaloneOSX":
        return root / "macos" / "AtlasMvp.app"
    fail(f"unsupported target: {target}")


def clean_output(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def parse_args() -> argparse.Namespace:
    target = default_target()
    p = argparse.ArgumentParser(description="Build samples/mvp/UnityClient in Unity batchmode")
    p.add_argument("--unity-project", default=str(DEFAULT_UNITY_PROJECT),
                   help=f"Unity project root (default: {DEFAULT_UNITY_PROJECT})")
    p.add_argument("--unity", help="Unity executable path; otherwise UNITY_EXE/UNITY_PATH or Unity Hub path is used")
    p.add_argument("--target", default=target, choices=TARGETS,
                   help=f"Unity build target (default: {target})")
    p.add_argument("--output", help="Player output path; defaults under out/mvp-unity/")
    p.add_argument("--log", default=str(DEFAULT_LOG), help=f"Unity log path (default: {DEFAULT_LOG})")
    p.add_argument("--config", default="Release", choices=["Debug", "Release"],
                   help="Atlas.Mvp.Client.dll config passed to setup_mvp_unity (default: Release)")
    p.add_argument("--client-config", default="Release", choices=["Debug", "Release"],
                   help="Native plugin + SDK config passed to setup_mvp_unity (default: Release)")
    p.add_argument("--skip-setup", action="store_true", help="Skip tools/setup_mvp_unity.py")
    p.add_argument("--skip-build", action="store_true",
                   help="Pass --skip-build to setup_mvp_unity.py when setup runs")
    p.add_argument("--development", action="store_true", help="Build a Unity development player")
    p.add_argument("--clean-output", action="store_true", help="Delete previous output before building")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    unity_project = Path(args.unity_project).expanduser().resolve()
    unity_helpers.validate_project(TAG, unity_project)
    unity = unity_helpers.resolve_unity(TAG, args.unity, unity_project)
    output = Path(args.output).expanduser().resolve() if args.output else default_output(args.target)
    log = Path(args.log).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    log.parent.mkdir(parents=True, exist_ok=True)

    if args.clean_output:
        clean_output(output)

    if not args.skip_setup:
        setup_cmd = [sys.executable, str(REPO_ROOT / "tools" / "setup_mvp_unity.py"),
                     "--unity-project", str(unity_project),
                     "--config", args.config,
                     "--client-config", args.client_config]
        if args.skip_build:
            setup_cmd.append("--skip-build")
        run(setup_cmd)

    cmd = [
        str(unity),
        "-batchmode",
        "-quit",
        "-nographics",
        "-projectPath", str(unity_project),
        "-executeMethod", BUILD_METHOD,
        "-logFile", str(log),
        "-atlasBuildTarget", args.target,
        "-atlasBuildOutput", str(output),
    ]
    if args.development:
        cmd.append("-atlasDevelopment")

    info(" ".join(cmd))
    result = subprocess.run(cmd, check=False)
    if result.returncode != 0:
        info(f"Unity failed with exit code {result.returncode}; log: {log}")
        unity_helpers.tail_log(TAG, log)
        return result.returncode

    if not output.exists():
        info(f"Unity exited successfully but output was not found: {output}")
        info(f"log: {log}")
        unity_helpers.tail_log(TAG, log)
        return 1

    info(f"built {output}")
    info(f"log: {log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
