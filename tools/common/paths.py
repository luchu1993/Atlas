from pathlib import Path

# tools/common/paths.py → tools/common → tools → repo root.
REPO_ROOT = Path(__file__).resolve().parents[2]


def resolve_repo_root() -> Path:
    return REPO_ROOT


def bin_dir(build_subdir: str = "debug") -> Path:
    return REPO_ROOT / "bin" / build_subdir
