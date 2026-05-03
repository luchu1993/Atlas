#!/usr/bin/env python3
"""Atlas entity_ids.xml manifest helper.

Subcommands:
    alloc <Name> [--manifest path]      pick next free id and append entry
    list [--manifest path]              show active + deprecated entries
    deprecate <Name> [--manifest path]  mark entry deprecated="true"
    audit [--manifest path] [--rev REV] reject destructive manifest edits

The manifest is the source of truth for entity type_id assignment consumed
by Atlas.Generators.Def. Allocation never recycles ids — once assigned a
name keeps its id forever, and removed entities stay in the file with
deprecated="true".
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Iterable
from xml.etree import ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common.paths import REPO_ROOT

DEFAULT_MANIFEST = REPO_ROOT / "entity_defs" / "entity_ids.xml"
MIN_ID = 1
MAX_ID = 0x3FFF
NAME_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class ManifestError(RuntimeError):
    pass


def log(msg: str) -> None:
    print(f"[def_id] {msg}", flush=True)


def parse_manifest(path: Path) -> ET.ElementTree:
    if not path.exists():
        raise ManifestError(
            f"manifest not found at {path} — pass --manifest <path> or run from repo root"
        )
    try:
        tree = ET.parse(path)
    except ET.ParseError as ex:
        raise ManifestError(f"{path}: malformed XML ({ex})") from ex
    root = tree.getroot()
    if root.tag != "entity_ids":
        raise ManifestError(f"{path}: root element must be <entity_ids>, got <{root.tag}>")
    return tree


def iter_entries(root: ET.Element) -> Iterable[tuple[str, int, bool]]:
    for el in root.findall("entity"):
        name = el.attrib.get("name", "")
        try:
            entry_id = int(el.attrib.get("id", ""))
        except ValueError as ex:
            raise ManifestError(f"<entity name='{name}'> has non-integer id") from ex
        deprecated = el.attrib.get("deprecated", "").lower() == "true"
        yield name, entry_id, deprecated


def collect_used(root: ET.Element) -> tuple[dict[str, int], set[int]]:
    by_name: dict[str, int] = {}
    used_ids: set[int] = set()
    for name, entry_id, _ in iter_entries(root):
        if not name:
            raise ManifestError("<entity> entry missing name attribute")
        if name in by_name:
            raise ManifestError(f"duplicate manifest entry for '{name}'")
        if entry_id in used_ids:
            raise ManifestError(f"duplicate id={entry_id} in manifest")
        if entry_id < MIN_ID or entry_id > MAX_ID:
            raise ManifestError(f"entry '{name}' id={entry_id} out of [{MIN_ID}, {MAX_ID}]")
        by_name[name] = entry_id
        used_ids.add(entry_id)
    return by_name, used_ids


def next_free_id(used_ids: set[int]) -> int:
    if not used_ids:
        return MIN_ID
    candidate = max(used_ids) + 1
    if candidate > MAX_ID:
        for i in range(MIN_ID, MAX_ID + 1):
            if i not in used_ids:
                return i
        raise ManifestError(f"manifest is full ({MAX_ID} ids in use); cannot allocate more")
    return candidate


def atomic_write_text(path: Path, content: str) -> None:
    fd, tmp_name = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        os.replace(tmp_name, path)
    except Exception:
        if os.path.exists(tmp_name):
            os.unlink(tmp_name)
        raise


def serialize_entry(name: str, entry_id: int, deprecated: bool) -> str:
    suffix = ' deprecated="true"' if deprecated else ""
    return f'  <entity name="{name}" id="{entry_id}"{suffix}/>'


def insert_entry(manifest_path: Path, name: str, entry_id: int) -> None:
    """Append a new <entity> line just before </entity_ids>, preserving comments."""
    text = manifest_path.read_text(encoding="utf-8")
    closing = "</entity_ids>"
    idx = text.rfind(closing)
    if idx < 0:
        raise ManifestError(f"{manifest_path}: missing </entity_ids> closing tag")
    insertion = serialize_entry(name, entry_id, deprecated=False) + "\n"
    new_text = text[:idx] + insertion + text[idx:]
    atomic_write_text(manifest_path, new_text)


def mark_deprecated(manifest_path: Path, name: str) -> None:
    text = manifest_path.read_text(encoding="utf-8")
    pattern = re.compile(rf'(<entity\s+name="{re.escape(name)}"\s+id="\d+")\s*/>')
    if not pattern.search(text):
        raise ManifestError(f"entry '{name}' not found in {manifest_path}")
    new_text = pattern.sub(r'\1 deprecated="true"/>', text, count=1)
    atomic_write_text(manifest_path, new_text)


def cmd_alloc(args: argparse.Namespace) -> int:
    name = args.name
    if not NAME_PATTERN.match(name):
        log(f"error: name '{name}' must match [A-Za-z_][A-Za-z0-9_]*")
        return 1

    manifest_path = Path(args.manifest).resolve()
    tree = parse_manifest(manifest_path)
    by_name, used_ids = collect_used(tree.getroot())

    if name in by_name:
        log(f"error: '{name}' already has id={by_name[name]} in {manifest_path}")
        return 1

    new_id = next_free_id(used_ids)
    insert_entry(manifest_path, name, new_id)
    log(f"allocated id={new_id} for '{name}' in {manifest_path}")
    return 0


def cmd_list(args: argparse.Namespace) -> int:
    manifest_path = Path(args.manifest).resolve()
    tree = parse_manifest(manifest_path)
    print(f"{'id':>5}  {'name':<32} state")
    print("-" * 50)
    for name, entry_id, deprecated in sorted(iter_entries(tree.getroot()), key=lambda e: e[1]):
        state = "deprecated" if deprecated else "active"
        print(f"{entry_id:>5}  {name:<32} {state}")
    return 0


def cmd_deprecate(args: argparse.Namespace) -> int:
    manifest_path = Path(args.manifest).resolve()
    tree = parse_manifest(manifest_path)
    by_name, _ = collect_used(tree.getroot())
    if args.name not in by_name:
        log(f"error: '{args.name}' not found in {manifest_path}")
        return 1
    mark_deprecated(manifest_path, args.name)
    log(f"marked '{args.name}' (id={by_name[args.name]}) as deprecated in {manifest_path}")
    return 0


def _git_show(rev: str, repo_relative_path: str) -> str | None:
    try:
        return subprocess.check_output(
            ["git", "show", f"{rev}:{repo_relative_path}"],
            stderr=subprocess.DEVNULL,
            cwd=str(REPO_ROOT),
        ).decode("utf-8")
    except subprocess.CalledProcessError:
        return None


def _entries_from_xml_text(text: str, label: str) -> dict[str, tuple[int, bool]]:
    root = ET.fromstring(text)
    if root.tag != "entity_ids":
        raise ManifestError(f"{label}: root element must be <entity_ids>")
    out: dict[str, tuple[int, bool]] = {}
    for el in root.findall("entity"):
        name = el.attrib.get("name", "")
        try:
            entry_id = int(el.attrib.get("id", ""))
        except ValueError as ex:
            raise ManifestError(f"{label}: <entity name='{name}'> non-integer id") from ex
        deprecated = el.attrib.get("deprecated", "").lower() == "true"
        out[name] = (entry_id, deprecated)
    return out


def cmd_audit(args: argparse.Namespace) -> int:
    manifest_path = Path(args.manifest).resolve()
    try:
        rel = manifest_path.relative_to(REPO_ROOT)
    except ValueError:
        log(f"error: manifest {manifest_path} is outside repo root {REPO_ROOT}")
        return 1

    head_text = _git_show(args.rev, rel.as_posix())
    if head_text is None:
        log(f"manifest at {args.rev} does not exist; treating audit as additive-only baseline")
        return 0

    head_entries = _entries_from_xml_text(head_text, f"{args.rev}:{rel}")
    cur_entries = _entries_from_xml_text(manifest_path.read_text(encoding="utf-8"),
                                         str(manifest_path))

    violations: list[str] = []
    for name, (head_id, head_deprecated) in head_entries.items():
        if name not in cur_entries:
            violations.append(
                f"  - entry '{name}' (id={head_id}) was DELETED; mark deprecated='true' instead"
            )
            continue
        cur_id, cur_deprecated = cur_entries[name]
        if cur_id != head_id:
            violations.append(
                f"  - entry '{name}' id changed {head_id} -> {cur_id}; ids are permanent contracts"
            )
        if head_deprecated and not cur_deprecated:
            violations.append(
                f"  - entry '{name}' was un-deprecated; deprecated entries cannot be revived"
            )

    if violations:
        log(f"manifest audit FAILED against {args.rev}:")
        for v in violations:
            print(v)
        return 1

    added = sorted(set(cur_entries) - set(head_entries))
    if added:
        log(f"manifest audit OK; new entries: {', '.join(added)}")
    else:
        log("manifest audit OK; no entry changes")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(prog="def_id", description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    common_manifest = lambda parser: parser.add_argument(
        "--manifest",
        default=str(DEFAULT_MANIFEST),
        help=f"path to entity_ids.xml (default: {DEFAULT_MANIFEST})",
    )

    p_alloc = sub.add_parser("alloc", help="pick next free id and append entry")
    p_alloc.add_argument("name", help="entity class name (e.g. Hero)")
    common_manifest(p_alloc)
    p_alloc.set_defaults(func=cmd_alloc)

    p_list = sub.add_parser("list", help="show all entries")
    common_manifest(p_list)
    p_list.set_defaults(func=cmd_list)

    p_dep = sub.add_parser("deprecate", help="mark entry deprecated='true'")
    p_dep.add_argument("name", help="entity name to deprecate")
    common_manifest(p_dep)
    p_dep.set_defaults(func=cmd_deprecate)

    p_audit = sub.add_parser("audit",
                             help="reject id renumbering / entry deletion vs git rev")
    p_audit.add_argument("--rev", default="HEAD",
                         help="git revision to compare against (default: HEAD)")
    common_manifest(p_audit)
    p_audit.set_defaults(func=cmd_audit)

    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        return args.func(args)
    except ManifestError as ex:
        log(f"error: {ex}")
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
