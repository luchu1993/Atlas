from pathlib import Path

# tools/common/paths.py → tools/common → tools → repo root.
REPO_ROOT = Path(__file__).resolve().parents[2]


def resolve_repo_root() -> Path:
    return REPO_ROOT


def bin_dir(build_subdir: str = "debug") -> Path:
    return REPO_ROOT / "bin" / build_subdir


def dotnet_tfm_dir(parent: Path) -> Path:
    """Return the single ``net*/`` child of ``parent``; raise if absent or ambiguous."""
    if not parent.is_dir():
        raise FileNotFoundError(f"{parent} does not exist; build the project first")
    candidates = sorted(p for p in parent.iterdir() if p.is_dir() and p.name.startswith("net"))
    if not candidates:
        raise FileNotFoundError(
            f"No net*/ output directory under {parent}; build the project first")
    if len(candidates) > 1:
        raise FileNotFoundError(
            f"Multiple net*/ output directories under {parent}: "
            f"{[p.name for p in candidates]} — pin TFM explicitly")
    return candidates[0]
