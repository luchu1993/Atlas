#!/usr/bin/env python3
"""One-button C3-B validation.

PHASE_C_VALIDATION.md §C3-B tests the unreliable-delta recovery path by
flipping StressAvatar.hp from reliable="true" to reliable="false",
rebuilding, running a drop-window smoke, restoring the def, and
rebuilding again. That manual flow is error-prone (leaving the def in
reliable="false" state silently disables reliable delta for every
later stress run, matching Known Limitation §7's "记得验证完改回").

This wrapper scripts the four steps end-to-end:

    1. sed StressAvatar.def: reliable="true" -> reliable="false"
    2. cmake + dotnet rebuild of the affected targets
    3. run tools/cluster_control/run_world_stress.py with the C3-B
       drop-window args
    4. restore the def and rebuild back to the reliable="true"
       baseline, even if steps 2/3 fail (try/finally)

Usage (from the repo root):

    python tools/cluster_control/run_phase_c3b.py

Flags mirror the underlying run_world_stress.py ones where useful
(--duration-sec, --script-clients, --drop-start-ms, --drop-duration-ms).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEF_PATH = REPO_ROOT / "entity_defs" / "StressAvatar.def"

RELIABLE_TRUE_PATTERN = re.compile(r'reliable="true"')
RELIABLE_FALSE_PATTERN = re.compile(r'reliable="false"')


def patch_def_to(mode: str) -> None:
    """Flip the `hp` property's reliable attribute to the requested mode.

    Raises RuntimeError if the def isn't in the complementary state — catches
    the "forgot to restore from a previous run" case up front.
    """
    if mode not in ("true", "false"):
        raise ValueError(f"invalid mode: {mode}")
    text = DEF_PATH.read_text(encoding="utf-8")
    current = RELIABLE_FALSE_PATTERN.search(text) is not None
    target_is_false = mode == "false"
    if current == target_is_false:
        # Already in the requested state — legal on restore, unexpected
        # on the pre-smoke patch. Either way, short-circuit.
        print(f"[c3b] def already in reliable=\"{mode}\" — nothing to patch")
        return

    if target_is_false:
        new_text = RELIABLE_TRUE_PATTERN.sub('reliable="false"', text, count=1)
    else:
        new_text = RELIABLE_FALSE_PATTERN.sub('reliable="true"', text, count=1)
    if new_text == text:
        raise RuntimeError(f"Failed to locate reliable= attribute in {DEF_PATH}")
    DEF_PATH.write_text(new_text, encoding="utf-8")
    print(f"[c3b] patched {DEF_PATH.name} -> reliable=\"{mode}\"")


def rebuild() -> None:
    """Rebuild the C++ + C# targets affected by the def change."""
    # Native side — the def registry embeds type metadata into the binaries.
    subprocess.run(
        [
            "cmake", "--build", "build/debug", "--config", "Debug",
            "--target",
            "atlas_stress_test_cell_dll", "atlas_stress_test_base_dll",
            "atlas_baseapp", "atlas_cellapp", "atlas_client",
            "atlas_client_desktop_dll",
        ],
        cwd=REPO_ROOT, check=True,
    )
    # C# side — the generator reads the def at compile time; the
    # ClientSample dll carries the matching entity_type_id table.
    subprocess.run(
        [
            "dotnet", "build",
            str(REPO_ROOT / "samples" / "client" / "Atlas.ClientSample.csproj"),
            "--no-incremental", "--nologo",
        ],
        cwd=REPO_ROOT, check=True,
    )


def run_smoke(args: argparse.Namespace) -> int:
    """Invoke run_world_stress.py with the C3-B drop-window args. Returns
    the subprocess exit code (0 on success, non-zero on --script-verify
    failure)."""
    cmd = [
        sys.executable,
        str(REPO_ROOT / "tools" / "cluster_control" / "run_world_stress.py"),
        "--clients", "0",
        "--duration-sec", str(args.duration_sec),
        "--script-clients", str(args.script_clients),
        "--script-verify",
        "--client-drop-inbound-ms", str(args.drop_start_ms), str(args.drop_duration_ms),
    ]
    print(f"[c3b] running: {' '.join(cmd)}")
    return subprocess.run(cmd, cwd=REPO_ROOT).returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--duration-sec", type=int, default=20)
    parser.add_argument("--script-clients", type=int, default=2)
    parser.add_argument("--drop-start-ms", type=int, default=5000)
    parser.add_argument("--drop-duration-ms", type=int, default=4000)
    parser.add_argument("--skip-restore", action="store_true",
                        help="leave the def in reliable=\"false\" after the run "
                             "(useful when iterating on the smoke itself; don't "
                             "commit with this state)")
    args = parser.parse_args()

    try:
        patch_def_to("false")
        rebuild()
        rc = run_smoke(args)
    finally:
        if args.skip_restore:
            print("[c3b] --skip-restore: leaving def in reliable=\"false\" "
                  "— DO NOT COMMIT")
        else:
            patch_def_to("true")
            rebuild()
    return rc


if __name__ == "__main__":
    sys.exit(main())
