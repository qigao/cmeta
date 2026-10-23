#!/usr/bin/env python3
"""Resolve exact vcpkg ABI references, then populate the read-only files cache via curl."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


PRESETS = {
    "win-release-ci",
    "linux-dev-ci",
    "linux-tsan-ci",
    "linux-release-ci",
    "linux-arm64-release-ci",
    "linux-clang-release-ci",
    "mac-x64-release-ci",
    "mac-arm64-release-ci",
    "mac-arm64-clang-release-ci",
    "android-arm64-v8a-release-ci",
    "ios-arm64-release-ci",
}


def normalize_nuget_prefix(path: Path) -> None:
    """Match vcpkg's NuGet provider prefix normalization for x-write packages.config."""
    prefix = os.environ.get("X_VCPKG_NUGET_ID_PREFIX", "")
    if not prefix:
        return
    if not re.fullmatch(r"[A-Za-z0-9_.+-]+", prefix):
        raise ValueError(f"invalid X_VCPKG_NUGET_ID_PREFIX: {prefix!r}")
    tree = ET.parse(path)
    root = tree.getroot()
    changed = False
    for item in root:
        package_id = item.attrib.get("id", "")
        canonical = prefix + "_"
        if package_id.startswith(canonical):
            continue
        if not package_id.startswith(prefix):
            raise ValueError(
                f"packages.config entry {package_id!r} does not use configured prefix {prefix!r}")
        suffix = package_id[len(prefix):]
        if not suffix:
            raise ValueError("empty package identity after NuGet prefix")
        item.set("id", canonical + suffix)
        changed = True
    if changed:
        tree.write(path, encoding="utf-8", xml_declaration=True)


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


def populate(config: Path, cache: Path, label: str) -> None:
    if not config.is_file():
        raise SystemExit(f"canonical vcpkg did not emit packages.config for {label}")
    normalize_nuget_prefix(config)
    count = validate_packages_config(config)
    print(f"Resolved {count} exact vcpkg package/ABI references for {label}", flush=True)
    root = Path(__file__).resolve().parents[2]
    subprocess.run([
        sys.executable, str(root / "cmake/ci/github-packages.py"),
        "cache", str(config), str(cache),
    ], cwd=root, check=True)
    print(f"Restored exact GitHub Packages cache entries into {cache}", flush=True)


def restore_preset(root: Path, preset: str, cache: Path) -> None:
    if preset not in PRESETS:
        raise ValueError(f"unsupported Salts CI preset: {preset}")
    config = root / "build/cache-preflight" / preset / "packages.config"
    config.parent.mkdir(parents=True, exist_ok=True)
    config.unlink(missing_ok=True)

    # Use the actual release/sanitizer preset so compiler, target triplet,
    # overlays, chainload toolchain and manifest produce the same ABI graph as build.
    command = [
        "cmake", "--preset", preset, "-B", str(config.parent),
        "-DSALTS_CACHE_PREFLIGHT=ON",
        f"-DVCPKG_INSTALL_OPTIONS=--dry-run;--x-write-nuget-packages-config={config.as_posix()}",
    ]
    subprocess.run(command, cwd=root, check=True)
    populate(config, cache, preset)


def restore_manifest(root: Path, manifest_root: Path, triplet: str,
                     host_triplet: str, label: str, cache: Path) -> None:
    manifest_root = manifest_root.resolve()
    if not (manifest_root / "vcpkg.json").is_file():
        raise ValueError(f"manifest root has no vcpkg.json: {manifest_root}")
    for value, name in ((triplet, "triplet"), (host_triplet, "host triplet"), (label, "label")):
        if not re.fullmatch(r"[A-Za-z0-9_.+-]+", value):
            raise ValueError(f"invalid {name}: {value!r}")

    work = root / "build/cache-preflight" / ("manifest-" + label)
    config = work / "packages.config"
    install_root = work / "installed"
    work.mkdir(parents=True, exist_ok=True)
    config.unlink(missing_ok=True)

    exe = Path(os.environ["VCPKG_ROOT"]) / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
    if not exe.is_file():
        raise ValueError(f"canonical vcpkg executable is missing: {exe}")
    command = [
        str(exe), "install",
        f"--x-manifest-root={manifest_root}",
        f"--x-install-root={install_root}",
        f"--triplet={triplet}",
        f"--host-triplet={host_triplet}",
        "--dry-run",
        f"--x-write-nuget-packages-config={config}",
    ]
    subprocess.run(command, cwd=manifest_root, check=True)
    populate(config, cache, f"{label}:{triplet}/{host_triplet}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode")

    preset = sub.add_parser("preset")
    preset.add_argument("name", choices=sorted(PRESETS))

    manifest = sub.add_parser("manifest")
    manifest.add_argument("manifest_root", type=Path)
    manifest.add_argument("--triplet", required=True)
    manifest.add_argument("--host-triplet", required=True)
    manifest.add_argument("--label", required=True)

    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = Path(__file__).resolve().parents[2]
    cache = Path(os.environ["VCPKG_BINARY_CACHE"]).resolve()
    cache.mkdir(parents=True, exist_ok=True)

    if args.mode == "preset":
        restore_preset(root, args.name, cache)
    elif args.mode == "manifest":
        restore_manifest(root, args.manifest_root, args.triplet,
                         args.host_triplet, args.label, cache)
    else:
        raise SystemExit("select 'preset <name>' or 'manifest <root> ...'")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
