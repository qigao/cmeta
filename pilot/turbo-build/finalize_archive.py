#!/usr/bin/env python3
"""Copy a raw CMake-compatible archive and run the captured ranlib command."""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 5:
        print("usage: finalize_archive.py RAW_ARCHIVE FINAL_ARCHIVE CWD RANLIB_COMMAND_JSON", file=sys.stderr)
        return 2
    raw_archive = Path(sys.argv[1])
    final_archive = Path(sys.argv[2])
    working_dir = Path(sys.argv[3])
    try:
        ranlib_command = json.loads(sys.argv[4])
    except json.JSONDecodeError as exc:
        print(f"invalid ranlib command JSON: {exc}", file=sys.stderr)
        return 2
    if not isinstance(ranlib_command, list) or not ranlib_command or not all(
        isinstance(token, str) for token in ranlib_command
    ):
        print("ranlib command must be a non-empty string array", file=sys.stderr)
        return 2
    if not raw_archive.is_file():
        print(f"missing raw archive: {raw_archive}", file=sys.stderr)
        return 2
    final_archive.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(raw_archive, final_archive)
    subprocess.run(ranlib_command, cwd=working_dir, check=True)
    if not final_archive.is_file():
        print(f"ranlib did not leave the expected archive: {final_archive}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        print(f"ranlib failed with exit status {exc.returncode}", file=sys.stderr)
        raise SystemExit(exc.returncode or 1)
