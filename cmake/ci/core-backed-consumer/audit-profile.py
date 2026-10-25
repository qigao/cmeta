#!/usr/bin/env python3
"""Audit the *provisional real SaltsConfig* consumers after both SDKs move.

Only original Salts CMeta/Plugin/ServiceHost/CFlow implementation is allowed
beside the one installed SaltsCore Foundation DLL/DSO owner. No static Runtime
fallback and no old Salts::Core is a permitted owner.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--core-tools", type=Path, required=True)
    parser.add_argument("--core-prefix", type=Path, required=True)
    parser.add_argument("--salts-prefix", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    module = args.core_tools.resolve() / "audit_runtime_owner.py"
    if not module.is_file():
        raise SystemExit("Exact pinned Core binary-owner auditor is missing")
    sys.path.insert(0, str(module.parent))
    from audit_runtime_owner import (
        binary_dependencies, dumpbin_path, find_build_image, find_library,
        image_exports, native_abi_identity, sha256,
    )

    system = ("windows" if sys.platform == "win32" else
              "darwin" if sys.platform == "darwin" else "linux")
    errors: list[str] = []
    report = {"schema_version": 1, "platform": system,
              "package": "Salts 3.0 (Core-backed Draft)", "ok": False}
    try:
        core = args.core_prefix.resolve()
        salts = args.salts_prefix.resolve()
        build = args.build_dir.resolve()
        expected_config = salts / "lib/cmake/Salts/SaltsTargets.cmake"
        if not expected_config.is_file():
            errors.append(f"Missing actual installed SaltsTargets: {expected_config}")
        db = dumpbin_path(None) if system == "windows" else None
        images = {
            "core": find_library(core, "core", system),
            "platform": find_library(core, "platform", system),
            "native_io": find_library(core, "native_io", system),
            "concurrency": find_library(core, "concurrency", system),
            "provider": find_build_image(build, "salts3_test_provider", system, False),
            "ace_c": find_build_image(build, "salts3_consumer_c11", system, True),
            "ace_cpp": find_build_image(build, "salts3_consumer_cpp17", system, True),
            "cflow_c": find_build_image(build, "salts3_cflow_c11", system, True),
            "cflow_cpp": find_build_image(build, "salts3_cflow_cpp17", system, True),
        }
        for name, image in images.items():
            if not image.is_file():
                errors.append(f"Missing required {name} binary: {image}")
        if errors:
            raise ValueError("; ".join(errors))
        exports = {n: image_exports(p, system, db) for n, p in images.items()}
        imports = {n: binary_dependencies(p, system, db) for n, p in images.items()}
        owners = {
            "mem_global": "core",
            "mem_buffer_release": "core",
            "mem_slice_release": "core",
            "salts_core_runtime_abi_0_1_required": "core",
            "salts_core_utf8_valid": "core",
            "cmeta_thread_current_token": "platform",
            "cmeta_readiness_reactor_init_kind": "platform",
            "native_io_backend_init": "native_io",
            "cmeta_deadline_queue_init": "concurrency",
        }
        for symbol, expected in owners.items():
            actual = [name for name in images if symbol in exports[name]]
            if actual != [expected]:
                errors.append(
                    f"{symbol}: expected exact {expected} shared owner, actual {actual}"
                )
        for consumer in ("ace_c", "ace_cpp", "cflow_c", "cflow_cpp"):
            required = ({"core", "platform", "native_io"} if
                        consumer.startswith("cflow") else {"core", "platform"})
            if not required.issubset(imports[consumer]):
                errors.append(
                    f"{consumer}: required direct shared runtime imports "
                    f"{sorted(required)}, actual {sorted(imports[consumer])}"
                )
        ids = {name: native_abi_identity(images[name], name, system)
               for name in ("core", "platform", "native_io", "concurrency")}
        report.update({
            "artifacts": {
                n: {"path": str(p), "sha256": sha256(p),
                    "core_imports": sorted(imports[n])}
                for n, p in images.items()
            },
            "native_abi_id": ids,
            "sole_owner_symbols": owners,
            "errors": errors,
            "ok": not errors,
        })
    except (RuntimeError, ValueError, OSError) as err:
        report["errors"] = errors or [str(err)]
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    for issue in report.get("errors", []):
        print("Salts3 native ABI: " + issue, file=sys.stderr)
    print("Salts3 native ABI: " + ("PASS" if report["ok"] else "FAIL"))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
