#!/usr/bin/env python3
"""End-to-end MVP UE launcher: build → cluster → UE Editor.

Default flow:
  1. Run tools/build_mvp_ue.py (Release; --no-build to skip).
  2. Spawn the MVP cluster in the background (run_world_stress.py with
     MVP base/cell assemblies; --no-cluster to skip).
  3. Wait briefly for cluster cold-start, then open UnrealEditor on the
     UEClient project so the developer can hit PIE.
  4. When UnrealEditor exits (or Ctrl+C), close the cluster cleanly by
     closing its stdin — run_world_stress.py's wait loop drops on EOF.

Use --no-ue with --no-cluster=false to keep just the cluster up (e.g.
for separate UE Editor sessions).
"""
from __future__ import annotations

import argparse
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
UE_PROJECT = REPO_ROOT / "samples" / "mvp" / "UEClient" / "UEClient.uproject"


def info(msg: str) -> None:
    print(f"[run_mvp_ue] {msg}")


def fail(msg: str, code: int = 1) -> "subprocess.NoReturn":
    print(f"[run_mvp_ue] ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--config", default="Release", choices=["Debug", "Release"],
                   help="atlas_net_client / DefDump config (default: Release; "
                        "UE Editor cannot load Debug-CRT DLLs)")
    p.add_argument("--build-config", default="Development",
                   choices=["Debug", "DebugGame", "Development", "Shipping", "Test"],
                   help="UE target config (default: Development)")
    p.add_argument("--ue-root", help="UE engine root; falls back to UE_ROOT env var")
    p.add_argument("--no-build", action="store_true",
                   help="skip build_mvp_ue.py (assumes plugin + ATDF already staged)")
    p.add_argument("--no-cluster", action="store_true",
                   help="skip cluster launch (e.g. cluster already running)")
    p.add_argument("--no-ue", action="store_true",
                   help="skip launching UnrealEditor (keep cluster only)")
    p.add_argument("--cluster-warmup-sec", type=float, default=8.0,
                   help="seconds to wait after spawning cluster before opening UE "
                        "(cold-start floor; default 8)")
    return p.parse_args()


def resolve_ue_root(arg: str | None) -> Path:
    candidate = arg or os.environ.get("UE_ROOT")
    if not candidate:
        fail("Set --ue-root or UE_ROOT to the UnrealEngine source root")
    ue = Path(candidate).expanduser().resolve()
    if not (ue / "Engine" / "Binaries").is_dir():
        fail(f"{ue} does not look like a UE engine root (missing Engine/Binaries/)")
    return ue


def ue_editor_exe(ue_root: Path) -> Path:
    plat = platform.system()
    if plat == "Windows":
        return ue_root / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"
    if plat == "Darwin":
        return ue_root / "Engine" / "Binaries" / "Mac" / "UnrealEditor.app" / "Contents" / "MacOS" / "UnrealEditor"
    return ue_root / "Engine" / "Binaries" / "Linux" / "UnrealEditor"


def run_build(config: str, build_config: str, ue_root_arg: str | None) -> None:
    cmd = [sys.executable, str(REPO_ROOT / "tools" / "build_mvp_ue.py"),
           "--config", config, "--build-config", build_config]
    if ue_root_arg:
        cmd.extend(["--ue-root", ue_root_arg])
    info(" ".join(cmd))
    subprocess.run(cmd, check=True)


def spawn_cluster(config: str) -> subprocess.Popen:
    """Spawn run_world_stress with MVP assemblies; returns the Popen handle.
    Cluster is foreground-blocking on stdin; close stdin to make it exit."""
    bin_name = config.lower()
    bin_dir = REPO_ROOT / "bin" / bin_name
    base_dll = bin_dir / "Atlas.Mvp.Base.dll"
    cell_dll = bin_dir / "Atlas.Mvp.Cell.dll"
    if not base_dll.exists() or not cell_dll.exists():
        fail(f"cluster assemblies missing under {bin_dir}; run "
             f"python tools/build.py {bin_name} first")
    cmd = [sys.executable, str(REPO_ROOT / "tools" / "cluster_control" / "run_world_stress.py"),
           "--build-dir", f"build/{bin_name}",
           "--config", config,
           "--baseapp-count", "1",
           "--cellapp-count", "1",
           "--login-port", "20018",
           "--base-assembly", str(base_dll),
           "--cell-assembly", str(cell_dll),
           "--cellapp-update-hertz", "20",
           "--baseapp-update-hertz", "20",
           "--clients", "0",
           "--keep-cluster",
           "--load-refresh-sec", "0"]
    info("spawning cluster: " + " ".join(cmd))
    return subprocess.Popen(cmd, stdin=subprocess.PIPE)


def launch_ue(ue_root: Path) -> int:
    exe = ue_editor_exe(ue_root)
    if not exe.is_file():
        fail(f"UnrealEditor not found at {exe}")
    if not UE_PROJECT.is_file():
        fail(f"UE project not found: {UE_PROJECT}")
    info(f"launching {exe.name} {UE_PROJECT.name}")
    return subprocess.run([str(exe), str(UE_PROJECT)]).returncode


def shutdown_cluster(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    info("closing cluster stdin (graceful shutdown)")
    try:
        if proc.stdin is not None:
            proc.stdin.close()
    except OSError:
        pass
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        info("cluster did not exit in 30s, terminating")
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


def main() -> int:
    args = parse_args()

    if not args.no_build:
        run_build(args.config, args.build_config, args.ue_root)

    cluster: subprocess.Popen | None = None
    try:
        if not args.no_cluster:
            cluster = spawn_cluster(args.config)
            info(f"cluster spawned (pid={cluster.pid}); warming up "
                 f"{args.cluster_warmup_sec:.1f}s")
            time.sleep(args.cluster_warmup_sec)
            if cluster.poll() is not None:
                fail(f"cluster died during warmup (rc={cluster.returncode})")

        if not args.no_ue:
            ue_root = resolve_ue_root(args.ue_root)
            ue_rc = launch_ue(ue_root)
            info(f"UnrealEditor exited rc={ue_rc}")
        elif cluster is not None:
            info("--no-ue set; cluster keeps running until Ctrl+C")
            cluster.wait()
    except KeyboardInterrupt:
        info("interrupted")
    finally:
        if cluster is not None:
            shutdown_cluster(cluster)

    return 0


if __name__ == "__main__":
    sys.exit(main())
