#!/usr/bin/env python3
"""Append the actual CMake archiver and ranlib actions to a compile-action pilot.

This parser fails closed when it cannot map the archive output, object files,
or finalizer command to known CMake artifacts; it never guesses a link command.
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, NoReturn


def fail(message: str) -> NoReturn:
    print(f"append_cmake_archive_action.py: error: {message}", file=sys.stderr)
    raise SystemExit(2)


def resolve_path(value: str, base: Path) -> Path:
    path = Path(value)
    return path.resolve() if path.is_absolute() else (base / path).resolve()


def resolve_executable(value: str) -> str:
    if os.path.isabs(value):
        return str(Path(value).resolve())
    located = shutil.which(value)
    if not located:
        fail(f"cannot resolve command executable {value!r}")
    return str(Path(located).resolve())


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read JSON at {path}: {exc}")
    if not isinstance(value, dict):
        fail(f"expected a JSON object at {path}")
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--target", required=True)
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    build_dir = args.build_dir.resolve()
    evidence_dir = build_dir / "turbo-build-qualification"
    report_path = evidence_dir / "graph-report.json"
    buildfile_path = evidence_dir / "buildfile.json"
    report = load_json(report_path)
    buildfile = load_json(buildfile_path)

    target = report.get("target", {})
    if target.get("type") != "STATIC_LIBRARY":
        fail(f"expected STATIC_LIBRARY target, got {target.get('type')!r}")
    artifacts = [
        Path(item["path"]).resolve()
        for item in target.get("artifacts", [])
        if isinstance(item, dict) and isinstance(item.get("path"), str)
        and Path(item["path"]).suffix.lower() in {".a", ".lib"}
    ]
    if len(artifacts) != 1:
        fail(f"expected exactly one static-library artifact, found {list(map(str, artifacts))}")
    cmake_archive = artifacts[0]
    if not cmake_archive.is_file():
        fail(f"CMake archive not found: {cmake_archive}")

    link_files = list(build_dir.glob(f"**/CMakeFiles/{args.target}.dir/link.txt"))
    if len(link_files) != 1:
        fail(f"expected exactly one CMake link.txt for {args.target}, found {list(map(str, link_files))}")
    link_file = link_files[0]
    link_cwd = link_file.parent.parent.parent.resolve()
    commands = [part.strip() for part in link_file.read_text(encoding="utf-8").split("&&") if part.strip()]
    if len(commands) != 2:
        fail(f"expected one archiver and one ranlib command in {link_file}; found {len(commands)}")
    try:
        ar_argv = shlex.split(commands[0], posix=True)
        ranlib_argv = shlex.split(commands[1], posix=True)
    except ValueError as exc:
        fail(f"cannot parse CMake archive commands: {exc}")
    if not ar_argv or not ranlib_argv:
        fail("CMake archive command is empty")
    ar_exe = resolve_executable(ar_argv[0])
    ranlib_exe = resolve_executable(ranlib_argv[0])

    compile_report = report.get("compileCommands", [])
    if not isinstance(compile_report, list) or not compile_report:
        fail("compile report has no object actions")
    object_map = {
        Path(entry["originalObject"]).resolve(): Path(entry["turboBuildObject"]).resolve()
        for entry in compile_report
        if isinstance(entry, dict) and entry.get("originalObject") and entry.get("turboBuildObject")
    }
    action_names = [entry.get("action") for entry in compile_report if entry.get("action")]
    if len(object_map) != len(compile_report) or len(action_names) != len(compile_report):
        fail("compile report contains missing or duplicate object/action mappings")

    output_dir = evidence_dir / "archive-stage"
    output_dir.mkdir(parents=True, exist_ok=True)
    raw_archive = output_dir / f"pre-ranlib-{cmake_archive.name}"
    final_archive = output_dir / cmake_archive.name

    archive_token_indexes = []
    rewritten_ar_args: list[str] = []
    replaced_objects = set()
    for token in ar_argv[1:]:
        resolved = resolve_path(token, link_cwd) if not token.startswith("-") else None
        if resolved == cmake_archive:
            archive_token_indexes.append(len(rewritten_ar_args))
            rewritten_ar_args.append(str(raw_archive))
        elif resolved in object_map:
            rewritten_ar_args.append(str(object_map[resolved]))
            replaced_objects.add(resolved)
        else:
            rewritten_ar_args.append(token)
    if len(archive_token_indexes) != 1:
        fail(f"could not uniquely identify archive output in CMake ar command: {ar_argv}")
    if replaced_objects != set(object_map):
        missing = sorted(map(str, set(object_map) - replaced_objects))
        fail(f"CMake ar command did not reference all exported objects; missing={missing}")
    if len(compile_report) != len(object_map):
        fail("compile object/action cardinality mismatch")

    ranlib_archive_indexes = []
    rewritten_ranlib_args: list[str] = []
    for token in ranlib_argv[1:]:
        resolved = resolve_path(token, link_cwd) if not token.startswith("-") else None
        if resolved == cmake_archive:
            ranlib_archive_indexes.append(len(rewritten_ranlib_args))
            rewritten_ranlib_args.append(str(final_archive))
        else:
            rewritten_ranlib_args.append(token)
    if len(ranlib_archive_indexes) != 1:
        fail(f"could not uniquely identify archive operand in CMake ranlib command: {ranlib_argv}")
    rewritten_ranlib = [ranlib_exe, *rewritten_ranlib_args]

    ar_version = subprocess.run([ar_exe, "--version"], check=False, capture_output=True, text=True)
    ar_version_line = (ar_version.stdout or ar_version.stderr).splitlines()
    ar_label = ar_version_line[0] if ar_version_line else Path(ar_exe).name
    archive_action_name = f"archive-{args.target}"
    actions = buildfile.get("actions")
    if not isinstance(actions, list):
        fail("generated buildfile has no actions array")
    if any(isinstance(action, dict) and action.get("name") == archive_action_name for action in actions):
        fail(f"buildfile already contains action {archive_action_name}")

    actions.append({
        "name": archive_action_name,
        "depends": action_names,
        "executable": ar_exe,
        "args": rewritten_ar_args,
        "cwd": str(link_cwd),
        "inputs": [str(object_map[original]) for original in sorted(object_map, key=str)],
        "outputs": [str(raw_archive)],
        "toolchain": f"cmake-archive/{Path(ar_exe).name}/{ar_label}",
        "inputs_complete": False,
        "environment_complete": False,
        "toolchain_complete": False,
        "paths_normalized": True,
    })

    finalizer = source_root / "pilot/turbo-build/finalize_archive.py"
    if not finalizer.is_file():
        fail(f"archive finalizer script not found: {finalizer}")
    python_exe = str(Path(sys.executable).resolve())
    actions.append({
        "name": f"finalize-{args.target}-archive",
        "depends": [archive_action_name],
        "executable": python_exe,
        "args": [
            str(finalizer),
            str(raw_archive),
            str(final_archive),
            str(link_cwd),
            json.dumps(rewritten_ranlib),
        ],
        "cwd": str(link_cwd),
        "inputs": [str(raw_archive), str(finalizer), ranlib_exe],
        "outputs": [str(final_archive)],
        "toolchain": f"cmake-archive-finalize/{Path(python_exe).name}/{Path(ranlib_exe).name}",
        "inputs_complete": False,
        "environment_complete": False,
        "toolchain_complete": False,
        "paths_normalized": True,
    })

    report["scope"] = "compile actions plus captured static archive/ranlib; full transitive and toolchain closure remains unproven"
    report["archive"] = {
        "cmakeArchive": str(cmake_archive),
        "turboBuildRawArchive": str(raw_archive),
        "turboBuildArchive": str(final_archive),
        "linkCommandFile": str(link_file),
        "workingDirectory": str(link_cwd),
        "archiverCommand": [ar_exe, *ar_argv[1:]],
        "turboBuildArchiverCommand": [ar_exe, *rewritten_ar_args],
        "ranlibCommand": rewritten_ranlib,
        "dependsOn": action_names,
        "completeness": False,
    }
    buildfile_path.write_text(json.dumps(buildfile, indent=2) + "\n", encoding="utf-8")
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Appended CMake archive graph for {args.target}: {len(action_names)} compile dependencies + ar + ranlib")
    print(f"  CMake archive:      {cmake_archive}")
    print(f"  TurboBuild archive: {final_archive}")
    print("  Completeness:       false (cache-hit claims remain disabled)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
