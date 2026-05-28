"""Shared helpers for Unity batch-mode tooling: editor discovery + project checks."""

from __future__ import annotations

import os
import platform
import sys
from pathlib import Path

HOST = platform.system()


def info(tag: str, msg: str) -> None:
    print(f"[{tag}] {msg}")


def fail(tag: str, msg: str, code: int = 1) -> None:
    print(f"[{tag}] ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


def project_version(unity_project: Path) -> str | None:
    version_file = unity_project / "ProjectSettings" / "ProjectVersion.txt"
    if not version_file.is_file():
        return None
    for line in version_file.read_text(encoding="utf-8").splitlines():
        prefix = "m_EditorVersion:"
        if line.startswith(prefix):
            return line[len(prefix):].strip()
    return None


def installed_unity_candidates() -> list[Path]:
    home = Path.home()
    if HOST == "Windows":
        roots = [Path("C:/Program Files/Unity/Hub/Editor")]
        candidates = [
            p / "Editor" / "Unity.exe"
            for root in roots
            for p in root.glob("*")
            if p.is_dir()
        ]
        return sorted(candidates, reverse=True)
    if HOST == "Darwin":
        roots = [Path("/Applications/Unity/Hub/Editor")]
        candidates = [
            p / "Unity.app" / "Contents" / "MacOS" / "Unity"
            for root in roots
            for p in root.glob("*")
            if p.is_dir()
        ]
        candidates.append(Path("/Applications/Unity/Unity.app/Contents/MacOS/Unity"))
        return sorted(candidates, reverse=True)
    roots = [home / "Unity" / "Hub" / "Editor", Path("/opt/unity/hub/editor")]
    candidates = [
        p / "Editor" / "Unity"
        for root in roots
        for p in root.glob("*")
        if p.is_dir()
    ]
    candidates.append(Path("/opt/unity/editor/Unity"))
    return sorted(candidates, reverse=True)


def unity_candidates(version: str | None) -> list[Path]:
    home = Path.home()
    if version is None:
        return installed_unity_candidates()
    if HOST == "Windows":
        return [
            Path(f"C:/Program Files/Unity/Hub/Editor/{version}/Editor/Unity.exe"),
            Path(f"C:/Program Files/Unity {version}/Editor/Unity.exe"),
        ]
    if HOST == "Darwin":
        return [
            Path(f"/Applications/Unity/Hub/Editor/{version}/Unity.app/Contents/MacOS/Unity"),
            Path("/Applications/Unity/Unity.app/Contents/MacOS/Unity"),
        ]
    return [
        home / "Unity" / "Hub" / "Editor" / version / "Editor" / "Unity",
        Path("/opt/unity/editor/Unity"),
    ]


def resolve_unity(tag: str, arg: str | None, unity_project: Path) -> Path:
    if arg:
        unity = Path(arg).expanduser().resolve()
        if not unity.is_file():
            fail(tag, f"Unity executable not found: {unity}")
        return unity

    for env_name in ("UNITY_EXE", "UNITY_PATH"):
        value = os.environ.get(env_name)
        if value:
            unity = Path(value).expanduser().resolve()
            if not unity.is_file():
                fail(tag, f"{env_name} points to missing Unity executable: {unity}")
            return unity

    version = project_version(unity_project)
    if version is None:
        info(tag, "ProjectSettings/ProjectVersion.txt is missing; scanning installed editors.")
    for candidate in unity_candidates(version):
        if candidate.is_file():
            if version is None:
                info(tag, f"using installed Unity editor: {candidate}")
            return candidate

    if version is None:
        fail(tag, "No installed Unity editor found. Pass --unity or set UNITY_EXE.")
    fail(tag, f"Unity {version} not found. Pass --unity or set UNITY_EXE.")
    raise RuntimeError("unreachable")  # fail() exits; satisfies type checker


def validate_project(tag: str, unity_project: Path) -> None:
    if not (unity_project / "Assets").is_dir():
        fail(tag, f"{unity_project} doesn't look like a Unity project (missing Assets/)")
    for rel in ("ProjectSettings/ProjectVersion.txt",
                "ProjectSettings/EditorBuildSettings.asset"):
        if not (unity_project / rel).is_file():
            fail(tag, f"{unity_project} is missing {rel}")


def tail_log(tag: str, path: Path, max_lines: int = 80) -> None:
    if not path.is_file():
        return
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    print(f"[{tag}] last {min(max_lines, len(lines))} log lines:")
    for line in lines[-max_lines:]:
        print(line)
