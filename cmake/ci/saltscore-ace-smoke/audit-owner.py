#!/usr/bin/env python3
"""Audit final M6 ACE executable and provider against the one installed Core.

Reuses the exact pinned SaltsCore ABI scanner. Fail-closed if native symbol
utilities, imported library identities, or final binaries are missing.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--core-tools", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    opts = parser.parse_args()
    core_tools = opts.core_tools.resolve()
    if not (core_tools / "audit_runtime_owner.py").is_file():
        raise SystemExit("Missing exact pinned SaltsCore audit tool")
    sys.path.insert(0, str(core_tools))
    from audit_runtime_owner import (
        binary_dependencies, dumpbin_path, find_build_image, find_library,
        image_exports, sha256,
    )

    platform = (
        "windows" if sys.platform == "win32" else
        "darwin" if sys.platform == "darwin" else "linux"
    )
    report = {"schema_version": 1, "platform": platform, "ok": False}
    try:
        db = dumpbin_path(None) if platform == "windows" else None
        prefix, build = opts.prefix.resolve(), opts.build_dir.resolve()
        images = {
            "core": find_library(prefix, "core", platform),
            "platform": find_library(prefix, "platform", platform),
            "ace_provider": find_build_image(build, "salts_m6_ace_provider", platform, False),
            "ace_c11": find_build_image(build, "salts_m6_consumer_c", platform, True),
            "ace_cpp17": find_build_image(build, "salts_m6_consumer_cpp", platform, True),
        }
        exports = {k: image_exports(v, platform, db) for k, v in images.items()}
        needed = {k: binary_dependencies(v, platform, db) for k, v in images.items()}
        ownership = {
            "mem_global": "core",
            "mem_buffer_release": "core",
            "mem_slice_release": "core",
            "cmeta_thread_current_token": "platform",
        }
        errors: list[str] = []
        for symbol, expected_owner in ownership.items():
            owners = [name for name, syms in exports.items() if symbol in syms]
            if owners != [expected_owner]:
                errors.append(f"{symbol}: expected {expected_owner}, actual {owners}")
        for name in ("ace_provider", "ace_c11", "ace_cpp17"):
            required = {"core", "platform"}
            if not required.issubset(needed[name]):
                errors.append(f"{name}: missing Core/Platform direct imports {sorted(needed[name])}")
        report.update({
            "artifacts": {
                name: {
                    "path": str(path),
                    "sha256": sha256(path),
                    "saltscore_imports": sorted(needed[name]),
                } for name, path in images.items()
            },
            "expected_binary_owner": ownership,
            "errors": errors,
            "ok": not errors,
        })
    except (OSError, ValueError, RuntimeError) as exc:
        report["errors"] = [str(exc)]
    opts.report.parent.mkdir(parents=True, exist_ok=True)
    opts.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    for issue in report.get("errors", []):
        print("M6 ABI audit:", issue, file=sys.stderr)
    print("M6 ABI owner: " + ("PASS" if report["ok"] else "FAIL"))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
