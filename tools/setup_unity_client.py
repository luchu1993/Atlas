#!/usr/bin/env python3
"""Build Atlas Client SDK + copy it into a Unity project's Assets/Atlas.Client.Unity/."""

from __future__ import annotations

import argparse
import filecmp
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


def stage_plugins(native_dll: Path, shared_dll: Path, client_dll: Path,
                  config: str) -> None:
    native_dir = PLUGINS_ROOT / native_plugin_subdir()
    native_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(native_dll, native_dir / native_dll.name)
    info(f"staged {native_dll.name} -> {native_dir.relative_to(REPO_ROOT)}")

    stage_native_runtime_deps(native_dir, config)

    PLUGINS_ROOT.mkdir(parents=True, exist_ok=True)
    shutil.copy2(shared_dll, PLUGINS_ROOT / shared_dll.name)
    shutil.copy2(client_dll, PLUGINS_ROOT / client_dll.name)
    info(f"staged {shared_dll.name} + {client_dll.name} -> "
         f"{PLUGINS_ROOT.relative_to(REPO_ROOT)}")


def stage_native_runtime_deps(native_dir: Path, config: str) -> None:
    """Copy DLLs atlas_net_client imports (mimalloc); else Unity surfaces a
    misleading DllNotFoundException pointing at atlas_net_client itself."""
    for stale_name in stale_native_runtime_dep_names(config):
        stale = native_dir / stale_name
        if stale.exists():
            stale.unlink()
            info(f"removed stale {stale.name} from {native_dir.relative_to(REPO_ROOT)}")

    preset = config.lower()
    if HOST == "Windows":
        mimalloc_name = "mimalloc-debug.dll" if config == "Debug" else "mimalloc.dll"
        candidates = [
            REPO_ROOT / "bin" / preset / mimalloc_name,
            REPO_ROOT / "build" / preset / "_deps" / "mimalloc-build" / config / mimalloc_name,
        ]
    elif HOST == "Linux":
        lib = "libmimalloc-debug.so" if config == "Debug" else "libmimalloc.so"
        candidates = [
            REPO_ROOT / "bin" / preset / lib,
            REPO_ROOT / "build" / preset / "_deps" / "mimalloc-build" / lib,
        ]
    else:
        return

    src = next((p for p in candidates if p.exists()), None)
    if src is None:
        info(f"warning: mimalloc not found in {[str(c) for c in candidates]}; "
             f"native plugin may DllNotFoundException at runtime")
        return
    shutil.copy2(src, native_dir / src.name)
    info(f"staged {src.name} -> {native_dir.relative_to(REPO_ROOT)}")


def stale_native_runtime_dep_names(config: str) -> list[str]:
    if HOST == "Windows":
        return ["mimalloc.dll"] if config == "Debug" else ["mimalloc-debug.dll"]
    if HOST == "Linux":
        return ["libmimalloc.so"] if config == "Debug" else ["libmimalloc-debug.so"]
    return []


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


def copy_to_unity_project(unity_project: Path) -> None:
    target = unity_project / "Assets" / "Atlas.Client.Unity"
    target.mkdir(parents=True, exist_ok=True)

    expected_files: set[Path] = set()
    expected_dirs: set[Path] = {Path(".")}
    copied = 0
    for src in UNITY_SDK_DIR.rglob("*"):
        rel = src.relative_to(UNITY_SDK_DIR)
        if any(part in EXCLUDED_FROM_COPY for part in rel.parts):
            continue
        if src.is_dir():
            expected_dirs.add(rel)
            continue
        expected_files.add(rel)
        expected_dirs.add(rel.parent)
        if _copy_if_changed(src, target / rel):
            copied += 1

    removed = _remove_stale_unity_files(target, expected_files, expected_dirs)
    info(f"synced SDK -> {target} ({copied} copied, {removed} stale removed)")


def _copy_if_changed(src: Path, dst: Path) -> bool:
    if dst.exists() and filecmp.cmp(src, dst, shallow=False):
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copy2(src, dst)
    except PermissionError:
        fail(f"cannot update {dst}; close Unity Editor and rerun setup_mvp_unity")
    return True


def _remove_stale_unity_files(target: Path, expected_files: set[Path],
                              expected_dirs: set[Path]) -> int:
    removed = 0
    for path in sorted(target.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        rel = path.relative_to(target)
        if path.is_file() and rel not in expected_files:
            try:
                path.unlink()
            except PermissionError:
                fail(f"cannot remove stale {path}; close Unity Editor and rerun setup_mvp_unity")
            removed += 1
        elif path.is_dir() and rel not in expected_dirs:
            try:
                path.rmdir()
                removed += 1
            except OSError:
                pass
    return removed


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

    stage_plugins(native_dll, shared_dll, client_dll, args.config)
    copy_to_unity_project(unity_project)

    info("done.")
    info(f"open {unity_project} in Unity Hub (2022.3 LTS), wait for refresh,")
    info("then add an AtlasNetworkManager MonoBehaviour to a GameObject and Play.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
