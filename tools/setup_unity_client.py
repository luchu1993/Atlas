#!/usr/bin/env python3
"""Build Atlas Client SDK + copy it into a Unity project's Assets/Atlas.Client.Unity/."""

from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common.paths import REPO_ROOT

UNITY_SDK_DIR = REPO_ROOT / "src" / "csharp" / "Atlas.Client.Unity"
PLUGINS_ROOT = UNITY_SDK_DIR / "Plugins"

# Excluded from the copy into the user's Assets/ — IDE-only or build artefacts.
EXCLUDED_FROM_COPY = {"Atlas.Client.Unity.csproj", "bin", "obj", ".gitkeep"}

HOST = platform.system()


def info(msg: str) -> None:
    print(f"[setup_unity_client] {msg}")


def fail(msg: str, code: int = 1) -> "None":
    print(f"[setup_unity_client] ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


def native_artefact_name() -> str:
    if HOST == "Windows":
        return "atlas_net_client.dll"
    if HOST == "Darwin":
        return "atlas_net_client.bundle"
    return "libatlas_net_client.so"


def native_plugin_subdir() -> str:
    if HOST == "Windows":
        return "Windows/x86_64"
    if HOST == "Darwin":
        return "macOS"
    return "Linux/x86_64"


def run(cmd: list[str], cwd: Path | None = None) -> None:
    info(" ".join(cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def build_native(config: str) -> Path:
    preset_map = {"Debug": "debug", "Release": "release"}
    preset = preset_map.get(config)
    if not preset:
        fail(f"unsupported config: {config}; expected Debug or Release")

    build_dir = REPO_ROOT / "build" / preset
    if HOST == "Windows":
        run([str(REPO_ROOT / "tools" / "bin" / "build.bat"), preset, "--config-only"])
    else:
        run([str(REPO_ROOT / "tools" / "bin" / "build.sh"), preset, "--config-only"])
    run(["cmake", "-S", str(REPO_ROOT), "-B", str(build_dir),
         "-DATLAS_BUILD_NET_CLIENT=ON"])
    run(["cmake", "--build", str(build_dir),
         "--target", "atlas_net_client", "--config", config])

    artefact = REPO_ROOT / "bin" / preset / native_artefact_name()
    if not artefact.exists():
        fail(f"build succeeded but {artefact} not found")
    return artefact


def build_managed(config: str) -> tuple[Path, Path]:
    shared_proj = REPO_ROOT / "src" / "csharp" / "Atlas.Shared" / "Atlas.Shared.csproj"
    client_proj = REPO_ROOT / "src" / "csharp" / "Atlas.Client" / "Atlas.Client.csproj"
    run(["dotnet", "build", str(shared_proj), "-c", config, "--nologo", "-v", "quiet"])
    run(["dotnet", "build", str(client_proj), "-c", config, "--nologo", "-v", "quiet"])

    shared_dll = (REPO_ROOT / "src" / "csharp" / "Atlas.Shared" / "bin" /
                  config / "netstandard2.1" / "Atlas.Shared.dll")
    client_dll = (REPO_ROOT / "src" / "csharp" / "Atlas.Client" / "bin" /
                  config / "netstandard2.1" / "Atlas.Client.dll")
    if not shared_dll.exists() or not client_dll.exists():
        fail(f"managed build incomplete: {shared_dll} / {client_dll}")
    return shared_dll, client_dll


def stage_plugins(native_dll: Path, shared_dll: Path, client_dll: Path) -> None:
    native_dir = PLUGINS_ROOT / native_plugin_subdir()
    native_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(native_dll, native_dir / native_dll.name)
    info(f"staged {native_dll.name} -> {native_dir.relative_to(REPO_ROOT)}")

    PLUGINS_ROOT.mkdir(parents=True, exist_ok=True)
    shutil.copy2(shared_dll, PLUGINS_ROOT / shared_dll.name)
    shutil.copy2(client_dll, PLUGINS_ROOT / client_dll.name)
    info(f"staged {shared_dll.name} + {client_dll.name} -> "
         f"{PLUGINS_ROOT.relative_to(REPO_ROOT)}")


def resolve_unity_project(arg: str | None) -> Path:
    if arg:
        candidate = Path(arg).expanduser().resolve()
    else:
        try:
            entered = input("UnityProjectRoot (path to your Unity 2022.3 project): ").strip()
        except (EOFError, KeyboardInterrupt):
            fail("no project path provided")
        if not entered:
            fail("no project path provided")
        candidate = Path(entered).expanduser().resolve()

    if not candidate.is_dir():
        fail(f"{candidate} is not a directory")
    if not (candidate / "Assets").is_dir():
        fail(f"{candidate} doesn't look like a Unity project (missing Assets/)")
    return candidate


def _ignore_for_unity(_src: str, names: list[str]) -> list[str]:
    return [n for n in names if n in EXCLUDED_FROM_COPY]


def copy_to_unity_project(unity_project: Path) -> None:
    target = unity_project / "Assets" / "Atlas.Client.Unity"
    if target.exists():
        info(f"removing existing {target.relative_to(unity_project)} (clean replace)")
        shutil.rmtree(target, onerror=_force_remove)
    shutil.copytree(UNITY_SDK_DIR, target, ignore=_ignore_for_unity)
    info(f"copied SDK -> {target}")


def _force_remove(func, path, exc_info) -> None:
    # Windows: shutil.rmtree fails on read-only files (e.g. git-tracked .meta).
    # Strip the read-only bit and retry once.
    import os
    import stat
    os.chmod(path, stat.S_IWRITE)
    func(path)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Wire Atlas Client SDK into a Unity project")
    p.add_argument("--unity-project", help="Path to the Unity project root")
    p.add_argument("--config", default="Release", choices=["Debug", "Release"])
    p.add_argument("--skip-build", action="store_true",
                   help="Skip native + managed builds; assume bin/<config>/ + dotnet outputs exist")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    unity_project = resolve_unity_project(args.unity_project)

    if args.skip_build:
        preset = args.config.lower()
        native_dll = REPO_ROOT / "bin" / preset / native_artefact_name()
        shared_dll = (REPO_ROOT / "src" / "csharp" / "Atlas.Shared" / "bin" /
                      args.config / "netstandard2.1" / "Atlas.Shared.dll")
        client_dll = (REPO_ROOT / "src" / "csharp" / "Atlas.Client" / "bin" /
                      args.config / "netstandard2.1" / "Atlas.Client.dll")
        for path in (native_dll, shared_dll, client_dll):
            if not path.exists():
                fail(f"--skip-build but {path} doesn't exist; rerun without --skip-build")
    else:
        native_dll = build_native(args.config)
        shared_dll, client_dll = build_managed(args.config)

    stage_plugins(native_dll, shared_dll, client_dll)
    copy_to_unity_project(unity_project)

    info("done.")
    info(f"open {unity_project} in Unity Hub (2022.3 LTS), wait for refresh,")
    info("then add an AtlasNetworkManager MonoBehaviour to a GameObject and Play.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
