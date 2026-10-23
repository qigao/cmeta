#!/usr/bin/env python3
"""Resolve exact vcpkg ABI references, then populate the read-only files cache via curl."""
from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


PRESETS = {
    "win-release-ci",
    "linux-release-ci",
    "linux-arm64-release-ci",
    "linux-clang-release-ci",
    "mac-x64-release-ci",
    "mac-arm64-release-ci",
    "mac-arm64-clang-release-ci",
    "android-arm64-v8a-release-ci",
    "ios-arm64-release-ci",
}


def validate_packages_config(path: Path) -> int:
    root = ET.parse(path).getroot()
    if root.tag != "packages":
        raise ValueError("canonical vcpkg output is not a packages.config document")
    seen: set[tuple[str, str]] = set()
    for item in root:
        if item.tag != "package" or set(item.attrib) != {"id", "version"}:
            raise ValueError("unexpected entry in canonical packages.config")
        package_id, version = item.attrib["id"], item.attrib["version"]
        if not re.fullmatch(r"[A-Za-z0-9_.+-]+", package_id):
            raise ValueError(f"invalid package identity: {package_id!r}")
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+-vcpkg[0-9a-f]{64}", version):
            raise ValueError(f"non-canonical vcpkg ABI version for {package_id}: {version!r}")
        key = (package_id.lower(), version.lower())
        if key in seen:
            raise ValueError(f"duplicate package reference: {package_id}/{version}")
        seen.add(key)
    if not seen:
        raise ValueError("canonical vcpkg produced an empty package list")
    return len(seen)


def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] not in PRESETS:
        raise SystemExit("usage: restore-vcpkg-cache.py <exact-release-preset>")
    root = Path(__file__).resolve().parents[2]
    preset = sys.argv[1]
    cache = Path(os.environ["VCPKG_BINARY_CACHE"]).resolve()
    config = root / "build/cache-preflight" / preset / "packages.config"
    config.parent.mkdir(parents=True, exist_ok=True)
    config.unlink(missing_ok=True)

    # Use the actual release preset so the selected compiler, target triplet,
    # overlay ports, toolchain and manifest produce the same ABI graph as build.
    # The root CMakeLists exits before defining production targets in this mode.
    command = [
        "cmake", "--preset", preset, "-B", str(config.parent),
        "-DSALTS_CACHE_PREFLIGHT=ON",
        f"-DVCPKG_INSTALL_OPTIONS=--dry-run;--x-write-nuget-packages-config={config.as_posix()}",
    ]
    subprocess.run(command, cwd=root, check=True)
    if not config.is_file():
        raise SystemExit("canonical vcpkg did not emit packages.config")
    count = validate_packages_config(config)
    print(f"Resolved {count} exact vcpkg package/ABI references for {preset}", flush=True)

    subprocess.run([
        sys.executable, str(root / "cmake/ci/github-packages.py"),
        "cache", str(config), str(cache),
    ], cwd=root, check=True)
    print(f"Restored exact GitHub Packages cache entries into {cache}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
