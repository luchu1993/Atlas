#!/usr/bin/env python3
"""Wire `Packages/com.atlas.client` into a Unity 2022.3 LTS project.

Steps:
  1. Build host-platform native (atlas_net_client) + managed
     (Atlas.Shared, Atlas.Client) artefacts.
  2. Copy them under Packages/com.atlas.client/Plugins/.
  3. Add a "file:..." dependency for com.atlas.client to the user-
     picked UnityProject's Packages/manifest.json.

Run via the platform wrapper:
  Windows: tools\\setup_unity_client.bat [--unity-project PATH] [--config Debug|Release] [--skip-build]
  Linux:   tools/setup_unity_client.sh   [...]
"""

from __future__ import annotations

import argparse
import json
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PACKAGE_DIR = REPO_ROOT / "Packages" / "com.atlas.client"
PLUGINS_ROOT = PACKAGE_DIR / "Plugins"

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
        run([str(REPO_ROOT / "tools" / "build.bat"), preset, "--config-only"])
    else:
        run([str(REPO_ROOT / "tools" / "build.sh"), preset, "--config-only"])
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
    info(f"copied {native_dll.name} -> {native_dir.relative_to(REPO_ROOT)}")

    PLUGINS_ROOT.mkdir(parents=True, exist_ok=True)
    shutil.copy2(shared_dll, PLUGINS_ROOT / shared_dll.name)
    shutil.copy2(client_dll, PLUGINS_ROOT / client_dll.name)
    info(f"copied {shared_dll.name} + {client_dll.name} -> "
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
    if not (candidate / "Assets").is_dir() or not (candidate / "Packages").is_dir():
        fail(f"{candidate} doesn't look like a Unity project (missing Assets/ + Packages/)")
    return candidate


def update_manifest(unity_project: Path) -> None:
    manifest = unity_project / "Packages" / "manifest.json"
    if not manifest.exists():
        fail(f"{manifest} not found")

    text = manifest.read_text(encoding="utf-8")
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        fail(f"{manifest} is not valid JSON ({exc}); please clean comments / trailing commas")

    deps = data.setdefault("dependencies", {})
    file_uri = "file:" + PACKAGE_DIR.resolve().as_posix()
    previous = deps.get("com.atlas.client")
    deps["com.atlas.client"] = file_uri

    backup = manifest.with_suffix(".json.bak")
    if not backup.exists():
        backup.write_text(text, encoding="utf-8")
        info(f"wrote backup {backup.relative_to(unity_project)}")

    manifest.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    if previous == file_uri:
        info(f"manifest.json already pointed at {file_uri} (unchanged)")
    elif previous is None:
        info(f"manifest.json: added com.atlas.client -> {file_uri}")
    else:
        info(f"manifest.json: rewired com.atlas.client {previous} -> {file_uri}")


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
    update_manifest(unity_project)

    info("done.")
    info(f"open {unity_project} in Unity Hub (2022.3 LTS), wait for refresh,")
    info("then add an AtlasNetworkManager MonoBehaviour to a GameObject and Play.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
