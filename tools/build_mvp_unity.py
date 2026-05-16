#!/usr/bin/env python3
"""Build the MVP Unity client from the command line."""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common.paths import REPO_ROOT  # noqa: E402

DEFAULT_UNITY_PROJECT = REPO_ROOT / "samples" / "mvp" / "UnityClient"
DEFAULT_LOG = REPO_ROOT / "out" / "mvp-unity" / "unity-build.log"
BUILD_METHOD = "Atlas.Mvp.Editor.MvpUnityBuild.BuildFromCommandLine"
TARGETS = ("StandaloneWindows64", "StandaloneLinux64", "StandaloneOSX")
HOST = platform.system()


def info(msg: str) -> None:
    print(f"[build_mvp_unity] {msg}")


def fail(msg: str, code: int = 1) -> None:
    print(f"[build_mvp_unity] ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


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


def project_version(unity_project: Path) -> str | None:
    version_file = unity_project / "ProjectSettings" / "ProjectVersion.txt"
    if not version_file.is_file():
        return None
    for line in version_file.read_text(encoding="utf-8").splitlines():
        prefix = "m_EditorVersion:"
        if line.startswith(prefix):
            return line[len(prefix):].strip()
    fail(f"{version_file} does not contain m_EditorVersion")


def installed_unity_candidates() -> list[Path]:
    home = Path.home()
    if HOST == "Windows":
        roots = [Path("C:/Program Files/Unity/Hub/Editor")]
        candidates = [
            p / "Editor" / "Unity.exe"
            for root in roots
            for p in root.glob("*")
            if p.is_dir()
        ]
        return sorted(candidates, reverse=True)
    if HOST == "Darwin":
        roots = [Path("/Applications/Unity/Hub/Editor")]
        candidates = [
            p / "Unity.app" / "Contents" / "MacOS" / "Unity"
            for root in roots
            for p in root.glob("*")
            if p.is_dir()
        ]
        candidates.append(Path("/Applications/Unity/Unity.app/Contents/MacOS/Unity"))
        return sorted(candidates, reverse=True)
    roots = [home / "Unity" / "Hub" / "Editor", Path("/opt/unity/hub/editor")]
    candidates = [
        p / "Editor" / "Unity"
        for root in roots
        for p in root.glob("*")
        if p.is_dir()
    ]
    candidates.append(Path("/opt/unity/editor/Unity"))
    return sorted(candidates, reverse=True)


def unity_candidates(version: str | None) -> list[Path]:
    home = Path.home()
    if version is None:
        return installed_unity_candidates()
    if HOST == "Windows":
        return [
            Path(f"C:/Program Files/Unity/Hub/Editor/{version}/Editor/Unity.exe"),
            Path(f"C:/Program Files/Unity {version}/Editor/Unity.exe"),
        ]
    if HOST == "Darwin":
        return [
            Path(f"/Applications/Unity/Hub/Editor/{version}/Unity.app/Contents/MacOS/Unity"),
            Path("/Applications/Unity/Unity.app/Contents/MacOS/Unity"),
        ]
    return [
        home / "Unity" / "Hub" / "Editor" / version / "Editor" / "Unity",
        Path("/opt/unity/editor/Unity"),
    ]


def resolve_unity(arg: str | None, unity_project: Path) -> Path:
    if arg:
        unity = Path(arg).expanduser().resolve()
        if not unity.is_file():
            fail(f"Unity executable not found: {unity}")
        return unity

    for env_name in ("UNITY_EXE", "UNITY_PATH"):
        value = os.environ.get(env_name)
        if value:
            unity = Path(value).expanduser().resolve()
            if not unity.is_file():
                fail(f"{env_name} points to missing Unity executable: {unity}")
            return unity

    version = project_version(unity_project)
    if version is None:
        info("ProjectSettings/ProjectVersion.txt is missing; scanning installed Unity editors.")
    for candidate in unity_candidates(version):
        if candidate.is_file():
            if version is None:
                info(f"using installed Unity editor: {candidate}")
            return candidate

    if version is None:
        fail("No installed Unity editor found. Pass --unity or set UNITY_EXE.")
    fail(f"Unity {version} not found. Pass --unity or set UNITY_EXE.")


def validate_project(unity_project: Path) -> None:
    if not (unity_project / "Assets").is_dir():
        fail(f"{unity_project} doesn't look like a Unity project (missing Assets/)")
    for rel in ("ProjectSettings/ProjectVersion.txt", "ProjectSettings/EditorBuildSettings.asset"):
        if not (unity_project / rel).is_file():
            fail(f"{unity_project} is missing {rel}")


def clean_output(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def tail_log(path: Path, max_lines: int = 80) -> None:
    if not path.is_file():
        return
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    print(f"[build_mvp_unity] last {min(max_lines, len(lines))} log lines:")
    for line in lines[-max_lines:]:
        print(line)


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
    validate_project(unity_project)
    unity = resolve_unity(args.unity, unity_project)
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
        tail_log(log)
        return result.returncode

    if not output.exists():
        info(f"Unity exited successfully but output was not found: {output}")
        info(f"log: {log}")
        tail_log(log)
        return 1

    info(f"built {output}")
    info(f"log: {log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
