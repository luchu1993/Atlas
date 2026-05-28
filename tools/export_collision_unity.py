#!/usr/bin/env python3
"""Export server collision from the MVP Unity client in batch mode.

Drives Unity's -executeMethod hook at Atlas.Mvp.Editor.AtlasCollisionExporter.
The exporter scans the active scene for ServerColliderAuthoring components
with exportToServer=true and writes an Atlas collision asset v2 JSON file.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common.paths import REPO_ROOT  # noqa: E402
from common import unity as unity_helpers  # noqa: E402

TAG = "export_collision_unity"
DEFAULT_UNITY_PROJECT = REPO_ROOT / "samples" / "mvp" / "UnityClient"
DEFAULT_LOG = REPO_ROOT / "out" / "mvp-unity" / "unity-export.log"
EXPORT_METHOD = "Atlas.Mvp.Editor.AtlasCollisionExporter.ExportFromCommandLine"


def info(msg: str) -> None:
    unity_helpers.info(TAG, msg)


def fail(msg: str, code: int = 1) -> None:
    unity_helpers.fail(TAG, msg, code)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--unity-project", default=str(DEFAULT_UNITY_PROJECT),
                   help=f"Unity project root (default: {DEFAULT_UNITY_PROJECT})")
    p.add_argument("--unity", help="Unity executable path; otherwise UNITY_EXE/UNITY_PATH or Unity Hub path is used")
    p.add_argument("--output", required=True, help="Output .collision.json path")
    p.add_argument("--source-hash", help="Override source_hash (default: 'unity:<scene-path>')")
    p.add_argument("--scene", help="Scene asset path to open before exporting (default: project's active scene)")
    p.add_argument("--log", default=str(DEFAULT_LOG), help=f"Unity log path (default: {DEFAULT_LOG})")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    unity_project = Path(args.unity_project).expanduser().resolve()
    unity_helpers.validate_project(TAG, unity_project)
    unity = unity_helpers.resolve_unity(TAG, args.unity, unity_project)
    output = Path(args.output).expanduser().resolve()
    log = Path(args.log).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    log.parent.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(unity),
        "-batchmode",
        "-quit",
        "-nographics",
        "-projectPath", str(unity_project),
        "-executeMethod", EXPORT_METHOD,
        "-logFile", str(log),
        "-atlasExportOutput", str(output),
    ]
    if args.source_hash:
        cmd += ["-atlasExportSourceHash", args.source_hash]
    if args.scene:
        cmd += ["-atlasExportScene", args.scene]

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

    info(f"wrote {output}")
    info(f"log: {log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
