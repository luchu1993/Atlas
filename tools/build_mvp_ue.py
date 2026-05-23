#!/usr/bin/env python3
"""Build the MVP UE client from the command line.

Pipeline:
  1. CMake-build atlas_net_client + atlas_entitydef_client (+ mimalloc)
  2. CMake-build Atlas.Tools.DefDump + Atlas.Mvp.Client, then run DefDump to
     regenerate entity_defs.bin
  3. Stage each artefact to its plugin ThirdParty subdir
  4. Invoke UE's UBT to build UEClientEditor (or another target)

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


def run(cmd: list[str], *, env: dict[str, str] | None = None) -> None:
    info(" ".join(cmd))
    subprocess.run(cmd, check=True, env=env)


def run_capture(cmd: list[str]) -> str:
    info(" ".join(cmd))
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def default_platform() -> str:
    if HOST == "Windows":
        return "Win64"
    if HOST == "Darwin":
        return "Mac"
    return "Linux"


def stage_subdir(plat: str) -> str:
    return {"Win64": "Win64", "Linux": "Linux", "Mac": "Mac"}.get(plat, plat)


def shared_artefact_name(stem: str) -> str:
    if HOST == "Windows":
        return f"{stem}.dll"
    if HOST == "Darwin":
        return f"{stem}.bundle"
    return f"lib{stem}.so"


def import_lib_name(stem: str) -> str | None:
    return f"{stem}.lib" if HOST == "Windows" else None


def mimalloc_name(config: str) -> str:
    if HOST == "Windows":
        return "mimalloc-debug.dll" if config == "Debug" else "mimalloc.dll"
    if HOST == "Darwin":
        return "libmimalloc-debug.dylib" if config == "Debug" else "libmimalloc.dylib"
    return "libmimalloc-debug.so" if config == "Debug" else "libmimalloc.so"


# Each entry maps a CMake target to its plugin ThirdParty subdir. Both DLLs
# ship together as the client SDK and are gated on ATLAS_BUILD_NET_CLIENT.
NATIVE_TARGETS = (
    ("atlas_net_client", "AtlasNetClient"),
    ("atlas_entitydef_client", "AtlasEntityDef"),
)


def build_native(config: str) -> dict[str, Path]:
    preset_map = {"Debug": "debug", "Release": "release"}
    preset = preset_map.get(config)
    if not preset:
        fail(f"unsupported native config: {config} (expected Debug or Release)")

    build_dir = REPO_ROOT / "build" / preset
    wrapper = REPO_ROOT / "tools" / "bin" / ("build.bat" if HOST == "Windows" else "build.sh")
    native_env = os.environ.copy()
    if HOST == "Windows":
        from build import VS_GENERATOR, ensure_ninja, load_msvc_env
        native_env, vs_major = load_msvc_env()
        if vs_major is not None and vs_major in VS_GENERATOR:
            native_env["CMAKE_GENERATOR"] = VS_GENERATOR[vs_major]
        if preset == "debug":
            ensure_ninja(native_env)

    run([str(wrapper), preset, "--config-only"], env=native_env)
    run(["cmake", "-S", str(REPO_ROOT), "-B", str(build_dir),
         "-DATLAS_BUILD_NET_CLIENT=ON",
         "-DATLAS_BUILD_ENTITYDEF_CLIENT=ON"], env=native_env)

    artefacts: dict[str, Path] = {}
    for target, _subdir in NATIVE_TARGETS:
        run(["cmake", "--build", str(build_dir),
             "--target", target, "--config", config], env=native_env)
        artefact = REPO_ROOT / "bin" / preset / shared_artefact_name(target)
        if not artefact.exists():
            fail(f"CMake build succeeded but {artefact} is missing")
        artefacts[target] = artefact
    return artefacts


def stage_one(native: Path, subdir: str, plat: str, *, stage_mimalloc: bool, config: str) -> None:
    target_dir = PLUGIN_ROOT / "ThirdParty" / subdir / stage_subdir(plat)
    target_dir.mkdir(parents=True, exist_ok=True)

    shutil.copy2(native, target_dir / native.name)
    info(f"staged {native.name} -> {target_dir.relative_to(REPO_ROOT)}")

    lib_name = import_lib_name(native.stem)
    if lib_name:
        lib_path = native.parent / lib_name
        if lib_path.exists():
            shutil.copy2(lib_path, target_dir / lib_path.name)
            info(f"staged {lib_path.name} -> {target_dir.relative_to(REPO_ROOT)}")
        else:
            info(f"warning: import lib {lib_path} missing; linker will fail")

    if stage_mimalloc:
        mimalloc = native.parent / mimalloc_name(config)
        if mimalloc.exists():
            shutil.copy2(mimalloc, target_dir / mimalloc.name)
            info(f"staged {mimalloc.name} -> {target_dir.relative_to(REPO_ROOT)}")
        else:
            info(f"warning: {mimalloc} missing; {native.stem} will fail to load")


def stage_native(artefacts: dict[str, Path], config: str, plat: str,
                 ue_build_config: str) -> None:
    for target, subdir in NATIVE_TARGETS:
        if target not in artefacts:
            fail(f"missing native artefact for {target}")
        stage_one(artefacts[target], subdir, plat,
                  stage_mimalloc=(target == "atlas_net_client"),
                  config=config)
    if config == "Debug" and ue_build_config not in {"Debug", "DebugGame"}:
        fail(f"--config Debug stages mimalloc-debug.dll + Debug CRT-linked "
             f"atlas_net_client.dll, which UE's {ue_build_config} editor cannot "
             f"load (vcruntime140d.dll resolution fails). Use --config Release "
             f"or pass --build-config DebugGame.")


# DefDump uses the client assembly because it bundles every entity type the
# UE plugin decodes; base/cell assemblies need native API providers on load.
DEFS_TOOL_PROJECT = (REPO_ROOT / "src" / "csharp" / "Atlas.Tools.DefDump" /
                     "Atlas.Tools.DefDump.csproj")
DEFS_TOOL_ASSEMBLY = "Atlas.Tools.DefDump.exe"
DEFS_SOURCE_PROJECT = REPO_ROOT / "samples" / "mvp" / "Atlas.Mvp.Client" / "Atlas.Mvp.Client.csproj"
DEFS_SOURCE_ASSEMBLY = "Atlas.Mvp.Client.dll"
DEFS_OUTPUT_NAME = "entity_defs.bin"

# Atlas.ClientSample covers container delta paths through StressAvatar tests;
# production keeps the lean MVP descriptor set.
TEST_DEFS_SOURCE_PROJECT = REPO_ROOT / "samples" / "client" / "Atlas.ClientSample.csproj"
TEST_DEFS_SOURCE_ASSEMBLY = "Atlas.ClientSample.dll"
TEST_DEFS_OUTPUT_NAME = "entity_defs_test.bin"

# Atlas.Tools.CppEmitter writes .def data to UE generated headers.
CPP_EMITTER_PROJECT = (REPO_ROOT / "src" / "csharp" / "Atlas.Tools.CppEmitter" /
                       "Atlas.Tools.CppEmitter.csproj")
CPP_EMITTER_ASSEMBLY = "Atlas.Tools.CppEmitter.exe"
CPP_EMITTER_NAMESPACE = "atlas::mvp"
ENTITY_DEFS_DIR = REPO_ROOT / "entity_defs"
GEN_OUTPUT_DIR = REPO_ROOT / "samples" / "mvp" / "UEClient" / "Source" / "UEClient" / "gen"


def per_project_bin(csproj: Path, config: str, tfm: str, leaf: str) -> Path:
    # MSVC-style per-csproj layout: <csproj_dir>/bin/x64/<Config>/<tfm>/<leaf>.
    # Sibling assemblies land here too, so DefDump's AssemblyResolve fallback
    # picks them up without needing the flat bin/<config>/ deploy dir.
    return csproj.parent / "bin" / "x64" / config / tfm / leaf


def build_defs(config: str) -> tuple[Path, Path]:
    preset_map = {"Debug": "debug", "Release": "release"}
    preset = preset_map.get(config)
    if not preset:
        fail(f"unsupported config: {config}")

    run(["dotnet", "build", str(DEFS_TOOL_PROJECT),
         "-c", config, "-p:Platform=x64"])
    run(["dotnet", "build", str(DEFS_SOURCE_PROJECT),
         "-c", config, "-p:Platform=x64"])
    run(["dotnet", "build", str(TEST_DEFS_SOURCE_PROJECT),
         "-c", config, "-p:Platform=x64"])

    exe = per_project_bin(DEFS_TOOL_PROJECT, config, "net10.0", DEFS_TOOL_ASSEMBLY)
    if not exe.exists():
        fail(f"DefDump build succeeded but {exe} is missing")

    def dump(source_proj: Path, source_assembly: str, output_name: str) -> Path:
        assembly = per_project_bin(source_proj, config, "net10.0", source_assembly)
        if not assembly.exists():
            fail(f"{assembly} not found; {source_proj.name} build silently produced nothing")
        out_bin = REPO_ROOT / "build" / preset / output_name
        out_bin.parent.mkdir(parents=True, exist_ok=True)
        run([str(exe), "--assembly", str(assembly), "--out", str(out_bin)])
        if not out_bin.exists():
            fail(f"DefDump returned 0 but {out_bin} is missing")
        return out_bin

    prod = dump(DEFS_SOURCE_PROJECT, DEFS_SOURCE_ASSEMBLY, DEFS_OUTPUT_NAME)
    test = dump(TEST_DEFS_SOURCE_PROJECT, TEST_DEFS_SOURCE_ASSEMBLY, TEST_DEFS_OUTPUT_NAME)
    verify_atdf_matches_cluster(prod, config, exe)
    return prod, test


def run_cpp_emitter(config: str) -> None:
    run(["dotnet", "build", str(CPP_EMITTER_PROJECT),
         "-c", config, "-p:Platform=x64"])
    exe = per_project_bin(CPP_EMITTER_PROJECT, config, "net10.0", CPP_EMITTER_ASSEMBLY)
    if not exe.exists():
        fail(f"CppEmitter build succeeded but {exe} is missing")
    GEN_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    run([str(exe), "--entity-defs", str(ENTITY_DEFS_DIR),
         "--output", str(GEN_OUTPUT_DIR), "--namespace", CPP_EMITTER_NAMESPACE])


def stage_defs(defs_bin: Path, output_name: str) -> None:
    target_dir = PLUGIN_ROOT / "ThirdParty" / "AtlasEntityDef"
    target_dir.mkdir(parents=True, exist_ok=True)
    dest = target_dir / output_name
    shutil.copy2(defs_bin, dest)
    info(f"staged {output_name} -> {dest.relative_to(REPO_ROOT)}")


# ATDF v3 layout: magic(4) + version(2) + flags(2) + digest(32) + ...
_ATDF_DIGEST_OFFSET = 8
_ATDF_DIGEST_LEN = 32


def read_atdf_digest(defs_bin: Path) -> str:
    with defs_bin.open("rb") as f:
        f.seek(_ATDF_DIGEST_OFFSET)
        raw = f.read(_ATDF_DIGEST_LEN)
    if len(raw) != _ATDF_DIGEST_LEN:
        fail(f"{defs_bin} too short for ATDF v3 digest section")
    return raw.hex()


def verify_atdf_matches_cluster(defs_bin: Path, config: str, exe: Path) -> None:
    """Cross-check the staged ATDF digest against bin/<config>/Atlas.Mvp.Cell.dll
    - the assembly the cluster actually loads. Stale build cache between the
    DefDump source assembly and the server-side deploy DLL is the most common
    source of def_mismatch at login."""
    cluster_dll = REPO_ROOT / "bin" / config.lower() / "Atlas.Mvp.Cell.dll"
    if not cluster_dll.exists():
        info(f"cluster dll {cluster_dll} missing; skipping digest verify "
             f"(run build.py {config.lower()} first to enable this check)")
        return
    out = run_capture([str(exe), "--digest-only", str(cluster_dll)])
    cluster_digest = ""
    for line in out.splitlines():
        if line.startswith("DefDump: digest="):
            cluster_digest = line.split("=", 1)[1].strip()
            break
    if not cluster_digest:
        fail(f"DefDump --digest-only produced no digest for {cluster_dll}")
    staged_digest = read_atdf_digest(defs_bin)
    if cluster_digest != staged_digest:
        fail(f"ATDF digest mismatch:\n"
             f"  staged   {staged_digest}\n"
             f"  cluster  {cluster_digest}\n"
             f"  source   {cluster_dll}\n"
             f"Rebuild the cluster (python tools/build.py {config.lower()}) AND "
             f"re-run build_mvp_ue.py without --skip-defs so both sides regenerate.")
    info(f"ATDF digest verified against cluster ({cluster_digest[:16]}…)")


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
    p.add_argument("--skip-defs", action="store_true",
                   help="Skip DefDump regenerating entity_defs.bin (use already-staged file)")
    p.add_argument("--skip-codegen", action="store_true",
                   help="Skip CppEmitter regen (use existing gen/*.gen.h)")
    p.add_argument("--skip-stage", action="store_true",
                   help="Skip staging into Plugin/ThirdParty (use already-staged binaries)")
    p.add_argument("--skip-ue", action="store_true",
                   help="Skip UBT build (useful when only refreshing the staged DLLs)")
    return p.parse_args()


def main() -> int:
    args = parse_args()

    if not args.skip_native:
        artefacts = build_native(args.config)
    else:
        artefacts = {}
        for target, _subdir in NATIVE_TARGETS:
            path = REPO_ROOT / "bin" / args.config.lower() / shared_artefact_name(target)
            if not path.exists():
                fail(f"--skip-native but {path} not found; run without --skip-native first")
            info(f"reusing existing {path}")
            artefacts[target] = path

    if not args.skip_defs:
        prod_defs, test_defs = build_defs(args.config)
    else:
        preset = args.config.lower()
        prod_defs = REPO_ROOT / "build" / preset / DEFS_OUTPUT_NAME
        test_defs = REPO_ROOT / "build" / preset / TEST_DEFS_OUTPUT_NAME
        if not prod_defs.exists(): prod_defs = None
        if not test_defs.exists(): test_defs = None
        info("entity_defs regen skipped per --skip-defs; "
             "leaving any staged copy in place")

    if not args.skip_codegen:
        run_cpp_emitter(args.config)
    else:
        info("CppEmitter skipped per --skip-codegen")

    if not args.skip_stage:
        stage_native(artefacts, args.config, args.platform, args.build_config)
        if prod_defs is not None: stage_defs(prod_defs, DEFS_OUTPUT_NAME)
        if test_defs is not None: stage_defs(test_defs, TEST_DEFS_OUTPUT_NAME)
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
