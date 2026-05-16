#!/usr/bin/env python3
"""Build the MVP UE client from the command line.

Pipeline:
  1. CMake-build atlas_net_client.dll (and mimalloc.dll dependency)
  2. Stage dll + import lib + mimalloc to AtlasUE plugin's ThirdParty/Win64/
  3. Invoke UE's UBT to build UEClientEditor (or another target)

Defaults match the Unity counterpart (Release config); pass --config Debug to
mirror a Debug Unity / SDK build.
"""

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

UE_PROJECT = REPO_ROOT / "samples" / "mvp" / "UEClient" / "UEClient.uproject"
PLUGIN_ROOT = REPO_ROOT / "samples" / "mvp" / "UEClient" / "Plugins" / "AtlasUE"
HOST = platform.system()

UE_TARGETS = ("UEClient", "UEClientEditor", "UEClientServer")
UE_BUILD_CONFIGS = ("Debug", "DebugGame", "Development", "Shipping", "Test")


def info(msg: str) -> None:
    print(f"[build_mvp_ue] {msg}")


def fail(msg: str, code: int = 1) -> None:
    print(f"[build_mvp_ue] ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


def run(cmd: list[str]) -> None:
    info(" ".join(cmd))
    subprocess.run(cmd, check=True)


def default_platform() -> str:
    if HOST == "Windows":
        return "Win64"
    if HOST == "Darwin":
        return "Mac"
    return "Linux"


def stage_subdir(plat: str) -> str:
    return {"Win64": "Win64", "Linux": "Linux", "Mac": "Mac"}.get(plat, plat)


def native_artefact_name() -> str:
    if HOST == "Windows":
        return "atlas_net_client.dll"
    if HOST == "Darwin":
        return "atlas_net_client.bundle"
    return "libatlas_net_client.so"


def native_import_lib_name() -> str | None:
    return "atlas_net_client.lib" if HOST == "Windows" else None


def mimalloc_name(config: str) -> str:
    if HOST == "Windows":
        return "mimalloc-debug.dll" if config == "Debug" else "mimalloc.dll"
    if HOST == "Darwin":
        return "libmimalloc-debug.dylib" if config == "Debug" else "libmimalloc.dylib"
    return "libmimalloc-debug.so" if config == "Debug" else "libmimalloc.so"


def build_native(config: str) -> Path:
    preset_map = {"Debug": "debug", "Release": "release"}
    preset = preset_map.get(config)
    if not preset:
        fail(f"unsupported native config: {config} (expected Debug or Release)")

    build_dir = REPO_ROOT / "build" / preset
    wrapper = REPO_ROOT / "tools" / "bin" / ("build.bat" if HOST == "Windows" else "build.sh")
    run([str(wrapper), preset, "--config-only"])
    run(["cmake", "-S", str(REPO_ROOT), "-B", str(build_dir),
         "-DATLAS_BUILD_NET_CLIENT=ON"])
    run(["cmake", "--build", str(build_dir),
         "--target", "atlas_net_client", "--config", config])

    artefact = REPO_ROOT / "bin" / preset / native_artefact_name()
    if not artefact.exists():
        fail(f"CMake build succeeded but {artefact} is missing")
    return artefact


def stage_native(native: Path, config: str, plat: str) -> None:
    target_dir = PLUGIN_ROOT / "ThirdParty" / "AtlasNetClient" / stage_subdir(plat)
    target_dir.mkdir(parents=True, exist_ok=True)

    shutil.copy2(native, target_dir / native.name)
    info(f"staged {native.name} -> {target_dir.relative_to(REPO_ROOT)}")

    lib_name = native_import_lib_name()
    if lib_name:
        lib_path = native.parent / lib_name
        if lib_path.exists():
            shutil.copy2(lib_path, target_dir / lib_path.name)
            info(f"staged {lib_path.name} -> {target_dir.relative_to(REPO_ROOT)}")
        else:
            info(f"warning: import lib {lib_path} missing; linker will fail")

    mimalloc = native.parent / mimalloc_name(config)
    if mimalloc.exists():
        shutil.copy2(mimalloc, target_dir / mimalloc.name)
        info(f"staged {mimalloc.name} -> {target_dir.relative_to(REPO_ROOT)}")
    else:
        info(f"warning: {mimalloc} missing; atlas_net_client will fail to load")


def resolve_ue_root(arg: str | None) -> Path:
    candidate = arg or os.environ.get("UE_ROOT")
    if not candidate:
        fail("Set --ue-root or UE_ROOT to the UnrealEngine source root "
             "(folder containing Engine/Build/BatchFiles/).")
    ue = Path(candidate).expanduser().resolve()
    if not (ue / "Engine" / "Build" / "BatchFiles").is_dir():
        fail(f"{ue} does not look like a UE engine root (missing Engine/Build/BatchFiles/)")
    return ue


def build_ue(ue_root: Path, target: str, build_config: str, plat: str) -> None:
    if HOST == "Windows":
        script = ue_root / "Engine" / "Build" / "BatchFiles" / "Build.bat"
    else:
        script = ue_root / "Engine" / "Build" / "BatchFiles" / "Build.sh"
    if not script.is_file():
        fail(f"UE build script not found: {script}")
    cmd = [str(script), target, plat, build_config, str(UE_PROJECT)]
    run(cmd)


def parse_args() -> argparse.Namespace:
    plat = default_platform()
    p = argparse.ArgumentParser(description="Build the MVP UE client end-to-end.")
    p.add_argument("--config", default="Release", choices=["Debug", "Release"],
                   help="atlas_net_client native build config (default: Release)")
    p.add_argument("--ue-root",
                   help="UE source root; falls back to UE_ROOT env var")
    p.add_argument("--target", default="UEClientEditor", choices=UE_TARGETS,
                   help="UE target to build (default: UEClientEditor)")
    p.add_argument("--build-config", default="Development", choices=UE_BUILD_CONFIGS,
                   help="UE target config (default: Development)")
    p.add_argument("--platform", default=plat,
                   help=f"UE target platform (default: {plat})")
    p.add_argument("--skip-native", action="store_true",
                   help="Skip atlas_net_client CMake build (assume bin/<config>/ exists)")
    p.add_argument("--skip-stage", action="store_true",
                   help="Skip staging into Plugin/ThirdParty (use already-staged binaries)")
    p.add_argument("--skip-ue", action="store_true",
                   help="Skip UBT build (useful when only refreshing the staged DLLs)")
    return p.parse_args()


def main() -> int:
    args = parse_args()

    if not args.skip_native:
        native = build_native(args.config)
    else:
        native = REPO_ROOT / "bin" / args.config.lower() / native_artefact_name()
        if not native.exists():
            fail(f"--skip-native but {native} not found; run without --skip-native first")
        info(f"reusing existing {native}")

    if not args.skip_stage:
        stage_native(native, args.config, args.platform)
    else:
        info("staging skipped per --skip-stage")

    if not args.skip_ue:
        ue_root = resolve_ue_root(args.ue_root)
        build_ue(ue_root, args.target, args.build_config, args.platform)
    else:
        info("UE build skipped per --skip-ue")

    info("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
