#!/usr/bin/env python3
"""Export a narrow set of compile actions from a real Salts CMake target.

CMake File API identifies the target/source set; compile_commands.json provides
the exact per-source compiler commands. The result intentionally remains
incomplete (system headers, environment and full toolchain closure are not
proven), so this is graph-coverage evidence, not safe cache-hit qualification.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, NoReturn

COMPILE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}


def fail(message: str) -> NoReturn:
    print(f"export_cmake_compile_actions.py: error: {message}", file=sys.stderr)
    raise SystemExit(2)


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read JSON at {path}: {exc}")
    if not isinstance(value, dict):
        fail(f"expected a JSON object at {path}")
    return value


def resolve_path(value: str, base: Path) -> Path:
    path = Path(value)
    return path.resolve() if path.is_absolute() else (base / path).resolve()


def replace_option_value(argv: list[str], option: str, value: str) -> bool:
    for index, token in enumerate(argv[:-1]):
        if token == option:
            argv[index + 1] = value
            return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--target", required=True)
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    build_dir = args.build_dir.resolve()
    compile_commands_path = build_dir / "compile_commands.json"
    reply_dir = build_dir / ".cmake/api/v1/reply"
    if not compile_commands_path.is_file():
        fail(f"missing CMAKE_EXPORT_COMPILE_COMMANDS output: {compile_commands_path}")

    index_files = list(reply_dir.glob("index-*.json"))
    if not index_files:
        fail(f"no CMake File API index found under {reply_dir}")
    index = read_json(max(index_files, key=lambda path: path.stat().st_mtime_ns))
    codemodel_entry = index.get("reply", {}).get("codemodel-v2")
    if not isinstance(codemodel_entry, dict) or not codemodel_entry.get("jsonFile"):
        fail("CMake File API reply has no codemodel-v2 response")
    codemodel = read_json(reply_dir / codemodel_entry["jsonFile"])
    configurations = codemodel.get("configurations", [])
    configuration = next(
        (item for item in configurations if item.get("name") == "Release"),
        configurations[0] if configurations else None,
    )
    if not isinstance(configuration, dict):
        fail("CMake codemodel contains no configurations")

    target_refs = [item for item in configuration.get("targets", [])
                   if item.get("name") == args.target]
    if len(target_refs) != 1:
        fail(f"expected exactly one target named {args.target}, found {len(target_refs)}")
    target_ref = target_refs[0]
    target_json = read_json(reply_dir / target_ref["jsonFile"])
    target_type = target_json.get("type")
    if target_type not in {"OBJECT_LIBRARY", "STATIC_LIBRARY"}:
        fail(f"unsupported target type for compile-only pilot: {target_type!r}")

    target_paths = target_json.get("paths", {})
    target_source_dir = Path(target_paths.get("source", source_root)).resolve()
    source_items = [item for item in target_json.get("sources", [])
                    if isinstance(item, dict) and isinstance(item.get("path"), str)]
    target_sources = {
        resolve_path(item["path"], target_source_dir)
        for item in source_items
    }
    expected_compile_sources = {
        path for path in target_sources if path.suffix in COMPILE_EXTENSIONS
    }
    if not expected_compile_sources:
        fail(f"target {args.target} exposes no recognizable compile source files")

    try:
        compile_commands = json.loads(compile_commands_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read compile_commands.json: {exc}")
    if not isinstance(compile_commands, list):
        fail("compile_commands.json is not an array")

    compile_by_source: dict[Path, dict[str, Any]] = {}
    for entry in compile_commands:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            continue
        command_dir = Path(entry.get("directory", build_dir)).resolve()
        source = resolve_path(entry["file"], command_dir)
        if source in expected_compile_sources:
            if source in compile_by_source:
                fail(f"duplicate compile command for target source {source}")
            compile_by_source[source] = entry
    missing = sorted(expected_compile_sources - set(compile_by_source), key=str)
    extra = sorted(set(compile_by_source) - expected_compile_sources, key=str)
    if missing or extra:
        fail(f"compile_commands coverage mismatch; missing={list(map(str, missing))}, extra={list(map(str, extra))}")

    output_dir = build_dir / "turbo-build-qualification"
    output_dir.mkdir(parents=True, exist_ok=True)
    declared_inputs = sorted(
        (path for path in target_sources if path.is_file()), key=str
    )
    actions: list[dict[str, Any]] = []
    compile_report: list[dict[str, Any]] = []

    for source in sorted(expected_compile_sources, key=str):
        entry = compile_by_source[source]
        command_dir = Path(entry.get("directory", build_dir)).resolve()
        if isinstance(entry.get("arguments"), list):
            argv = [str(value) for value in entry["arguments"]]
        elif isinstance(entry.get("command"), str):
            argv = shlex.split(entry["command"], posix=True)
        else:
            fail(f"compile command for {source} has neither arguments nor command")
        if len(argv) < 2:
            fail(f"compile command for {source} is unexpectedly short")

        compiler = argv[0]
        if not os.path.isabs(compiler):
            located = shutil.which(compiler)
            if not located:
                fail(f"cannot resolve compiler executable {compiler!r}")
            compiler = str(Path(located).resolve())
        argv[0] = compiler

        original_output: str | None = None
        for index_arg, token in enumerate(argv[:-1]):
            if token == "-o":
                original_output = argv[index_arg + 1]
                break
        if original_output is None:
            fail(f"compile command for {source} has no separate -o output argument")
        original_object = resolve_path(original_output, command_dir)
        if not original_object.is_file():
            fail(f"CMake object is missing; build target {args.target} first: {original_object}")

        token = hashlib.sha256(str(source).encode("utf-8")).hexdigest()[:12]
        turbo_object = output_dir / f"{source.stem}-{token}-turbobuild.o"
        turbo_depfile = output_dir / f"{source.stem}-{token}-turbobuild.d"
        action_argv = list(argv)
        replace_option_value(action_argv, "-o", str(turbo_object))
        replace_option_value(action_argv, "-MF", str(turbo_depfile))
        replace_option_value(action_argv, "-MT", str(turbo_object))
        replace_option_value(action_argv, "-MQ", str(turbo_object))

        compiler_version = subprocess.run(
            [compiler, "--version"], check=False, capture_output=True, text=True
        )
        version_lines = (compiler_version.stdout or compiler_version.stderr).splitlines()
        toolchain_label = f"cmake-file-api/{Path(compiler).name}/"
        toolchain_label += version_lines[0] if version_lines else "version-unavailable"
        action_name = f"compile-{args.target}-{source.stem}-{token}"
        actions.append({
            "name": action_name,
            "executable": compiler,
            "args": action_argv[1:],
            "cwd": str(command_dir),
            "inputs": [str(path) for path in declared_inputs],
            "outputs": [str(turbo_object)],
            "toolchain": toolchain_label,
            "inputs_complete": False,
            "environment_complete": False,
            "toolchain_complete": False,
            "paths_normalized": True,
        })
        compile_report.append({
            "action": action_name,
            "source": str(source),
            "compiler": compiler,
            "workingDirectory": str(command_dir),
            "originalObject": str(original_object),
            "turboBuildObject": str(turbo_object),
            "arguments": argv[1:],
            "turboBuildArguments": action_argv[1:],
        })

    buildfile_path = output_dir / "buildfile.json"
    buildfile_path.write_text(
        json.dumps({"version": 1, "actions": actions}, indent=2) + "\n",
        encoding="utf-8",
    )
    target_dependencies = [
        {
            "id": dependency.get("id"),
            "name": dependency.get("name"),
            "dependencyType": dependency.get("dependencyType"),
        }
        for dependency in target_json.get("dependencies", [])
        if isinstance(dependency, dict)
    ]
    report = {
        "schema": 1,
        "scope": "compile actions only; no archive/link action or complete dependency closure",
        "cmakeVersion": index.get("cmake", {}).get("version", {}).get("string"),
        "configuration": configuration.get("name", ""),
        "target": {
            "name": target_ref.get("name"),
            "id": target_ref.get("id"),
            "type": target_type,
            "sourceFiles": [str(path) for path in sorted(target_sources, key=str)],
            "dependencies": target_dependencies,
        },
        "compileCommands": compile_report,
        "declaredInputs": [str(path) for path in declared_inputs],
        "completeness": {
            "inputs": False,
            "environment": False,
            "toolchain": False,
            "reason": "Explicit target source/header set only; transitive/system headers and complete toolchain/environment closure are not proven.",
        },
        "buildfile": str(buildfile_path),
    }
    (output_dir / "graph-report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Exported {args.target}: {len(actions)} compile actions")
    print(f"  CMake File API source compile coverage: {len(compile_by_source)}/{len(expected_compile_sources)}")
    print("  Completeness: false (cache hits disabled; compile-only target slice)")
    print(f"  Buildfile: {buildfile_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
