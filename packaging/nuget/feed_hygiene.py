#!/usr/bin/env python3
"""Fail-closed checks for the immutable CMeta.Native 0.1 SDK package."""

from __future__ import annotations

import argparse
import json
from pathlib import PurePosixPath, Path
import re
import sys
import xml.etree.ElementTree as ET
import zipfile


RIDS = {
    "linux-x64": "none",
    "linux-arm64": "none",
    "windows-x64": "none",
    "windows-arm64": "none",
    "macos-x64": "none",
    "macos-arm64": "none",
    "android-x64": "none",
    "android-arm64": "none",
}
SHA = re.compile(r"^[0-9a-f]{40}$")
PACKAGE_VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
DISPLAY_VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]{3}$")


class PolicyError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PolicyError(message)


def fields(data: bytes, entry: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in data.decode("utf-8-sig").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        require(key not in result, f"{entry}: duplicate manifest key {key}")
        result[key] = value
    return result


def normalized_names(package: zipfile.ZipFile) -> set[str]:
    names: set[str] = set()
    for raw in package.namelist():
        name = raw.replace("\\", "/")
        path = PurePosixPath(name)
        require(not path.is_absolute() and ".." not in path.parts,
                f"unsafe package path: {raw}")
        names.add(name)
    return names


def nuspec_version(package: zipfile.ZipFile, names: set[str]) -> str:
    specs = sorted(name for name in names if name.count("/") == 0
                   and name.lower().endswith(".nuspec"))
    require(len(specs) == 1, f"expected one root nuspec, found {specs}")
    root = ET.fromstring(package.read(specs[0]))
    values = [node.text for node in root.iter()
              if node.tag.rsplit("}", 1)[-1] == "version"]
    require(len(values) == 1 and values[0] is not None,
            "nuspec must contain exactly one version")
    return values[0]


def verify_package(path: Path, display_version: str, package_version: str,
                   source_sha: str, core_sha: str) -> dict[str, object]:
    require(path.is_file(), f"package does not exist: {path}")
    require(DISPLAY_VERSION.fullmatch(display_version) is not None,
            f"invalid display release version: {display_version}")
    require(PACKAGE_VERSION.fullmatch(package_version) is not None,
            f"invalid normalized package version: {package_version}")
    require(SHA.fullmatch(source_sha) is not None, "invalid source SHA")
    require(SHA.fullmatch(core_sha) is not None, "invalid SaltsCore SHA")

    with zipfile.ZipFile(path) as package:
        names = normalized_names(package)
        require(nuspec_version(package, names) == package_version,
                "NuGet metadata version does not match the normalized release")
        expected_manifests = {
            f"sdk/{rid}/cmeta-sdk-manifest.txt" for rid in RIDS
        }
        actual_manifests = {
            name for name in names if name.endswith("/cmeta-sdk-manifest.txt")
        }
        require(actual_manifests == expected_manifests,
                f"incorrect SDK RID set: {sorted(actual_manifests ^ expected_manifests)}")

        for rid, protection in sorted(RIDS.items()):
            prefix = f"sdk/{rid}/"
            manifest_name = prefix + "cmeta-sdk-manifest.txt"
            manifest = fields(package.read(manifest_name), manifest_name)
            expected = {
                "release": display_version,
                "package": package_version,
                "commit": source_sha,
                "core_commit": core_sha,
                "rid": rid,
                "profile": "release",
                "compiler": "llvm",
                "protection": protection,
            }
            require(manifest == expected,
                    f"{rid}: manifest mismatch: {manifest!r}")
            required = (
                "include/cmeta/function.h",
                "include/cflow/cflow.h",
                "include/cmeta/configurator.h",
                "include/cmeta/service_host.h",
                "lib/cmake/CMeta/CMetaConfig.cmake",
                "lib/cmake/CMeta/CMetaConfigVersion.cmake",
                "lib/cmake/CMeta/CMetaTargets.cmake",
            )
            for relative in required:
                require(prefix + relative in names,
                        f"{rid}: missing installed SDK file {relative}")
            require(not any(name.startswith(prefix + "lib/cmake/SaltsCore/")
                            for name in names),
                    f"{rid}: CMeta.Native must not duplicate SaltsCore")

    return {
        "status": "PASS",
        "display_version": display_version,
        "package_version": package_version,
        "source_sha": source_sha,
        "core_sha": core_sha,
        "rids": sorted(RIDS),
    }


def published_absent(version: str, inventory: Path) -> dict[str, object]:
    require(PACKAGE_VERSION.fullmatch(version) is not None,
            f"invalid package version: {version}")
    records = json.loads(inventory.read_text(encoding="utf-8"))
    require(isinstance(records, list), "package inventory must be a JSON array")
    versions = []
    for record in records:
        require(isinstance(record, dict) and isinstance(record.get("name"), str),
                "malformed package version record")
        versions.append(record["name"])
    require(version.lower() not in {item.lower() for item in versions},
            f"NuGet version {version} already exists; immutable publish refused")
    return {"status": "PASS", "new_version": version,
            "existing_versions_checked": len(versions)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    verify = commands.add_parser("verify")
    verify.add_argument("package", type=Path)
    verify.add_argument("display_version")
    verify.add_argument("package_version")
    verify.add_argument("source_sha")
    verify.add_argument("core_sha")
    absent = commands.add_parser("published-absent")
    absent.add_argument("package_version")
    absent.add_argument("inventory", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "verify":
            result = verify_package(args.package, args.display_version,
                                    args.package_version, args.source_sha,
                                    args.core_sha)
        else:
            result = published_absent(args.package_version, args.inventory)
    except (PolicyError, OSError, json.JSONDecodeError,
            ET.ParseError, zipfile.BadZipFile) as error:
        print(f"RELEASE_POLICY_FAILED: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
