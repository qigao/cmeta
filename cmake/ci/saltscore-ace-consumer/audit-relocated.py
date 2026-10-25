#!/usr/bin/env python3
"""Fail-closed native ABI audit after *both* installed SDKs were relocated."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--core-tools", type=Path, required=True)
    parser.add_argument("--core-prefix", type=Path, required=True)
    parser.add_argument("--m6-prefix", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    audit_module = args.core_tools.resolve() / "audit_runtime_owner.py"
    if not audit_module.is_file():
        raise SystemExit("missing exact pinned SaltsCore ABI audit tool")
    sys.path.insert(0, str(audit_module.parent))
    from audit_runtime_owner import (
        binary_dependencies, dumpbin_path, find_build_image, find_library,
        image_exports, native_abi_identity, sha256,
    )

    osname = ("windows" if sys.platform == "win32" else
              "darwin" if sys.platform == "darwin" else "linux")
    report = {"schema_version": 1, "platform": osname, "ok": False}
    errors: list[str] = []
    try:
        core = args.core_prefix.resolve()
        m6 = args.m6_prefix.resolve()
        build = args.build_dir.resolve()
        dumpbin = dumpbin_path(None) if osname == "windows" else None
        plugin_name = ("salts_m6_host_primary.dll" if osname == "windows" else
                       "libsalts_m6_host_primary.dylib" if osname == "darwin" else
                       "libsalts_m6_host_primary.so")
        images = {
            "core": find_library(core, "core", osname),
            "platform": find_library(core, "platform", osname),
            "installed_provider": m6 / ("bin" if osname == "windows" else "lib") / plugin_name,
            "external_c": find_build_image(build, "m6_installed_c", osname, True),
            "external_cpp": find_build_image(build, "m6_installed_cpp", osname, True),
        }
        for name, path in images.items():
            if not path.is_file():
                errors.append(f"{name}: missing installed binary: {path}")
        if errors:
            raise ValueError("; ".join(errors))
        exported = {name: image_exports(path, osname, dumpbin)
                    for name, path in images.items()}
        imports = {name: binary_dependencies(path, osname, dumpbin)
                   for name, path in images.items()}
        expected = {
            "mem_global": "core",
            "mem_buffer_release": "core",
            "mem_slice_release": "core",
            "salts_core_runtime_abi_0_1_required": "core",
            "salts_core_utf8_valid": "core",
            "cmeta_thread_current_token": "platform",
        }
        for symbol, owner in expected.items():
            actual = [name for name in images if symbol in exported[name]]
            if actual != [owner]:
                errors.append(f"{symbol}: expected sole {owner}, defined in {actual}")
        for consumer in ("external_c", "external_cpp"):
            if not {"core", "platform"}.issubset(imports[consumer]):
                errors.append(
                    f"{consumer}: missing imported Core/Platform: {sorted(imports[consumer])}"
                )
        ids = {owner: native_abi_identity(images[owner], owner, osname)
               for owner in ("core", "platform")}
        report.update({
            "artifacts": {
                name: {"path": str(path), "sha256": sha256(path),
                       "saltscore_imports": sorted(imports[name])}
                for name, path in images.items()
            },
            "native_abi_identity": ids,
            "sole_owner_symbols": expected,
            "errors": errors,
            "ok": not errors,
        })
    except (ValueError, RuntimeError, OSError) as exc:
        report["errors"] = errors or [str(exc)]
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    for failure in report.get("errors", []):
        print(f"Relocated M6 ABI error: {failure}", file=sys.stderr)
    print("Relocated M6 ABI: " + ("PASS" if report["ok"] else "FAIL"))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
