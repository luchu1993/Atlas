#!/usr/bin/env python3
"""Stage Atlas Client SDK + the MVP entity script DLL into samples/mvp/UnityClient/.

Wraps tools/setup_unity_client (which builds + copies the SDK and native plugin
into <unity-project>/Assets/Atlas.Client.Unity/), then builds Atlas.Mvp.Client
under netstandard2.1 and drops it under Assets/Plugins/Atlas.Mvp/ so Unity loads
the entity classes alongside the SDK.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common.paths import REPO_ROOT  # noqa: E402

DEFAULT_UNITY_PROJECT = REPO_ROOT / "samples" / "mvp" / "UnityClient"
MVP_CLIENT_CSPROJ = REPO_ROOT / "samples" / "mvp" / "Atlas.Mvp.Client" / "Atlas.Mvp.Client.csproj"
MVP_PLUGIN_NAME = "Atlas.Mvp"


def info(msg: str) -> None:
    print(f"[setup_mvp_unity] {msg}")


def fail(msg: str, code: int = 1) -> None:
    print(f"[setup_mvp_unity] ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


def run(cmd: list[str]) -> None:
    info(" ".join(cmd))
    subprocess.run(cmd, check=True)


def stage_mvp_dll(unity_project: Path, config: str) -> None:
    run(["dotnet", "build", str(MVP_CLIENT_CSPROJ),
         "-c", config, "-f", "netstandard2.1", "--nologo", "-v", "quiet"])
    dll = (REPO_ROOT / "samples" / "mvp" / "Atlas.Mvp.Client" / "bin" /
           config / "netstandard2.1" / "Atlas.Mvp.Client.dll")
    if not dll.exists():
        fail(f"expected {dll} after build, not found")

    target = unity_project / "Assets" / "Plugins" / MVP_PLUGIN_NAME
    target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(dll, target / dll.name)
    info(f"staged {dll.name} -> {target.relative_to(unity_project)}/")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--unity-project", default=str(DEFAULT_UNITY_PROJECT),
                   help=f"Unity project root (default: {DEFAULT_UNITY_PROJECT})")
    # Native plugin defaults to Release: Debug links Tracy + debug CRT + mimalloc-debug,
    # which appear to Unity as a DllNotFoundException on atlas_net_client itself.
    p.add_argument("--client-config", default="Release", choices=["Debug", "Release"],
                   help="Native plugin + SDK build config (default: Release)")
    p.add_argument("--config", default="Release", choices=["Debug", "Release"],
                   help="Atlas.Mvp.Client.dll dotnet build config (default: Release)")
    p.add_argument("--skip-build", action="store_true",
                   help="Skip both SDK build and MVP DLL build; assume bin/<cfg>/ outputs exist")
    args = p.parse_args()

    unity_project = Path(args.unity_project).resolve()
    if not (unity_project / "Assets").is_dir():
        fail(f"{unity_project} doesn't look like a Unity project (missing Assets/)")

    sdk_cmd = ["python", str(REPO_ROOT / "tools" / "setup_unity_client.py"),
               "--unity-project", str(unity_project), "--config", args.client_config]
    if args.skip_build:
        sdk_cmd.append("--skip-build")
    run(sdk_cmd)

    stage_mvp_dll(unity_project, args.config)

    info("done.")
    info(f"open {unity_project} in Unity Hub (Unity 6 LTS, 6000.0.x), wait for refresh,")
    info("add Bootstrap.cs to an empty GameObject, configure host/port/user, hit Play.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
