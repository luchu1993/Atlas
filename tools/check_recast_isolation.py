#!/usr/bin/env python3
"""Reject `#include "Recast..."` / `#include "Detour..."` outside src/lib/navigation_recast/."""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common.paths import resolve_repo_root

ALLOWED_DIR = Path("src") / "lib" / "navigation_recast"
SOURCE_EXTS = {".h", ".hh", ".hpp", ".cc", ".cpp", ".cxx", ".c"}
SCAN_ROOTS = ("src", "tests", "samples")
RECAST_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]\s*(Recast|Detour)')


def iter_sources(repo_root: Path):
    for top in SCAN_ROOTS:
        base = repo_root / top
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix.lower() not in SOURCE_EXTS:
                continue
            yield path


def is_allowed(path: Path, repo_root: Path) -> bool:
    try:
        rel = path.relative_to(repo_root)
    except ValueError:
        return False
    return rel.parts[: len(ALLOWED_DIR.parts)] == ALLOWED_DIR.parts


def find_violations(repo_root: Path):
    violations = []
    for path in iter_sources(repo_root):
        if is_allowed(path, repo_root):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"warn: cannot read {path}: {exc}", file=sys.stderr)
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            if RECAST_INCLUDE_RE.match(line):
                violations.append((path, lineno, line.rstrip()))
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=resolve_repo_root(),
        help="Repository root (default: derived from script location).",
    )
    args = parser.parse_args()

    violations = find_violations(args.repo_root)
    if not violations:
        print(f"Recast isolation OK: no Recast/Detour includes outside {ALLOWED_DIR.as_posix()}/")
        return 0

    print(f"Recast isolation violated; Recast/Detour headers may only be included from "
          f"{ALLOWED_DIR.as_posix()}/:", file=sys.stderr)
    for path, lineno, line in violations:
        rel = path.relative_to(args.repo_root)
        print(f"  {rel.as_posix()}:{lineno}: {line}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
