#!/usr/bin/env python3
"""Export one real Salts CMake compile action from File API + compile_commands.

This deliberately creates an *incomplete* TurboBuild action: system headers,
ambient environment and full toolchain closure are not proven here, so action
skipping/cache-hit claims stay disabled.
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
from typing import Any


def fail(message: str) -> "NoReturn":
    print(f"export_cmake_graph.py: error: {message}", file=sys.stderr)
    raise SystemExit(2)


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read JSON at {path}: {exc}")
    if not isinstance(value, dict):
        fail(f"expected a JSON object at {path}")
    return value


def resolve_report_path(value: str, source_root: Path, build_dir: Path) -> Path:
    candidate = Path(value)
    if candidate.is_absolute():
        return candidate.resolve()
    for base in (source_root, build_dir):
        trial = (base / candidate).resolve()
        if trial.exists():
            return trial
    return (source_root / candidate).resolve()


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
    parser.add_argument("--target", default="cmeta_wsparser")
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    build_dir = args.build_dir.resolve()
    target_source = (source_root / "tools/wsparser/websocket_frame_parser.c").resolve()
    header_source = (source_root / "tools/wsparser/websocket_frame_parser.h").resolve()
    compile_commands_path = build_dir / "compile_commands.json"
    reply_dir = build_dir / ".cmake/api/v1/reply"

    if not target_source.is_file() or not header_source.is_file():
        fail(f"expected target source/header were not found under {source_root}")
    if not compile_commands_path.is_file():
        fail(f"missing CMAKE_EXPORT_COMPILE_COMMANDS output: {compile_commands_path}")

    index_files = list(reply_dir.glob("index-*.json"))
    if not index_files:
        fail(f"no CMake File API index found under {reply_dir}")
    index_path = max(index_files, key=lambda path: path.stat().st_mtime_ns)
    index = read_json(index_path)
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
    target_refs = [
        item for item in configuration.get("targets", [])
        if item.get("name") == args.target
    ]
    if len(target_refs) != 1:
        fail(f"expected exactly one CMake target named {args.target}, found {len(target_refs)}")
    target_ref = target_refs[0]
    target_json = read_json(reply_dir / target_ref["jsonFile"])
    if target_json.get("type") != "OBJECT_LIBRARY":
        fail(f"expected {args.target} to be OBJECT_LIBRARY, got {target_json.get('type')!r}")

    target_sources = [
        resolve_report_path(item["path"], source_root, build_dir)
        for item in target_json.get("sources", [])
        if isinstance(item, dict) and isinstance(item.get("path"), str)
    ]
    if target_source not in target_sources:
        fail(f"CMake File API target {args.target} does not list {target_source}")

    try:
        compile_commands = json.loads(compile_commands_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read compile_commands.json: {exc}")
    if not isinstance(compile_commands, list):
        fail("compile_commands.json is not an array")
    compile_entries = [
        entry for entry in compile_commands
        if isinstance(entry, dict)
        and isinstance(entry.get("file"), str)
        and Path(entry["file"]).resolve() == target_source
    ]
    if len(compile_entries) != 1:
        fail(f"expected exactly one compile command for {target_source}, found {len(compile_entries)}")
    entry = compile_entries[0]
    command_directory = Path(entry.get("directory", "")).resolve()
    if isinstance(entry.get("arguments"), list):
        argv = [str(value) for value in entry["arguments"]]
    elif isinstance(entry.get("command"), str):
        argv = shlex.split(entry["command"], posix=True)
    else:
        fail("compile command has neither an arguments array nor a command string")
    if len(argv) < 2:
        fail("CMake compile command is unexpectedly short")

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
        fail("CMake compile command does not contain a separate -o output argument")
    cmake_object = Path(original_output)
    if not cmake_object.is_absolute():
        cmake_object = (command_directory / cmake_object).resolve()
    if not cmake_object.is_file():
        fail(f"CMake's object output is missing; build target {args.target} first: {cmake_object}")

    output_dir = build_dir / "turbo-build-qualification"
    output_dir.mkdir(parents=True, exist_ok=True)
    turbo_object = output_dir / "websocket_frame_parser-turbobuild.o"
    turbo_depfile = output_dir / "websocket_frame_parser-turbobuild.d"
    action_argv = list(argv)
    replace_option_value(action_argv, "-o", str(turbo_object))
    replace_option_value(action_argv, "-MF", str(turbo_depfile))
    replace_option_value(action_argv, "-MT", str(turbo_object))
    replace_option_value(action_argv, "-MQ", str(turbo_object))

    compiler_version = subprocess.run(
        [compiler, "--version"], check=False, capture_output=True, text=True
    )
    version_line = (compiler_version.stdout or compiler_version.stderr).splitlines()
    toolchain_label = f"cmake-file-api/{Path(compiler).name}/"
    toolchain_label += version_line[0] if version_line else "version-unavailable"

    action_inputs = [str(target_source), str(header_source)]
    buildfile = {
        "version": 1,
        "actions": [
            {
                "name": f"compile-{args.target}",
                "executable": compiler,
                "args": action_argv[1:],
                "cwd": str(command_directory),
                "inputs": action_inputs,
                "outputs": [str(turbo_object)],
                "toolchain": toolchain_label,
                "inputs_complete": False,
                "environment_complete": False,
                "toolchain_complete": False,
                "paths_normalized": True,
            }
        ],
    }
    buildfile_path = output_dir / "buildfile.json"
    buildfile_path.write_text(json.dumps(buildfile, indent=2) + "\n", encoding="utf-8")

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
        "scope": "single CMake OBJECT_LIBRARY compile action; not the full Salts build graph",
        "cmakeVersion": index.get("cmake", {}).get("version", {}).get("string"),
        "configuration": configuration.get("name", ""),
        "target": {
            "name": target_ref.get("name"),
            "id": target_ref.get("id"),
            "type": target_json.get("type"),
            "sourceFiles": [str(path) for path in target_sources],
            "dependencies": target_dependencies,
        },
        "compileCommand": {
            "compiler": compiler,
            "workingDirectory": str(command_directory),
            "source": str(target_source),
            "originalObject": str(cmake_object),
            "turboBuildObject": str(turbo_object),
            "arguments": argv[1:],
            "turboBuildArguments": action_argv[1:],
        },
        "declaredInputs": action_inputs,
        "completeness": {
            "inputs": False,
            "environment": False,
            "toolchain": False,
            "reason": "This probe captures the explicit source and local header only; system-header and complete toolchain/environment closure are not proven.",
        },
        "buildfile": str(buildfile_path),
    }
    (output_dir / "graph-report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Exported {args.target}: 1 compile action")
    print(f"  CMake object:      {cmake_object}")
    print(f"  TurboBuild object: {turbo_object}")
    print(f"  Completeness:      false (cache hits disabled)")
    print(f"  Buildfile:         {buildfile_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
