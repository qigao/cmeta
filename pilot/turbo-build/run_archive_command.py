#!/usr/bin/env python3
"""Execute a captured CMake archiver command with fresh-output semantics."""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: run_archive_command.py OUTPUT_ARCHIVE CWD COMMAND_JSON", file=sys.stderr)
        return 2
    output = Path(sys.argv[1])
    working_dir = Path(sys.argv[2])
    try:
        command = json.loads(sys.argv[3])
    except json.JSONDecodeError as exc:
        print(f"invalid archiver command JSON: {exc}", file=sys.stderr)
        return 2
    if not isinstance(command, list) or not command or not all(
        isinstance(token, str) for token in command
    ):
        print("archiver command must be a non-empty string array", file=sys.stderr)
        return 2
    if output.is_symlink():
        print(f"refusing to replace symlink output: {output}", file=sys.stderr)
        return 2
    if output.exists():
        if not output.is_file():
            print(f"refusing to replace non-file output: {output}", file=sys.stderr)
            return 2
        output.unlink()
    subprocess.run(command, cwd=working_dir, check=True)
    if not output.is_file():
        print(f"archiver did not produce expected output: {output}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        print(f"archiver failed with exit status {exc.returncode}", file=sys.stderr)
        raise SystemExit(exc.returncode or 1)
