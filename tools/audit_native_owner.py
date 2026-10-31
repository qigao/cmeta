#!/usr/bin/env python3
"""Audit the canonical native executables against the one installed Core owner.

Only original Salts CMeta/Plugin/ServiceHost/CFlow implementation is allowed
beside the one installed SaltsCore Foundation DLL/DSO owner. No static Runtime
fallback and no old Salts::Core is a permitted owner.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import os
import re
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--core-tools", type=Path, required=True)
    parser.add_argument("--core-prefix", type=Path, required=True)
    parser.add_argument("--cmeta-prefix", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    module = args.core_tools.resolve() / "audit_runtime_owner.py"
    if not module.is_file():
        raise SystemExit("Exact pinned Core binary-owner auditor is missing")
    sys.path.insert(0, str(module.parent))
    from audit_runtime_owner import (
        binary_dependencies, find_build_image, find_library,
        image_exports, native_abi_identity, sha256,
    )

    system = ("windows" if sys.platform == "win32" else
              "darwin" if sys.platform == "darwin" else "linux")
    errors: list[str] = []
    report = {"schema_version": 1, "platform": system,
              "package": "CMeta 4.0 native API", "ok": False}
    try:
        core = args.core_prefix.resolve()
        cmeta = args.cmeta_prefix.resolve()
        build = args.build_dir.resolve()
        expected_config = cmeta / "lib/cmake/CMeta/CMetaTargets.cmake"
        if not expected_config.is_file():
            errors.append(f"Missing actual installed CMetaTargets: {expected_config}")
        # The production Windows graph uses llvm-mingw, including its DLL
        # prefix and PE tools. Do not require an unrelated MSVC installation
        # or relabel the previous MSVC SDK as the MinGW ABI.
        if system == "windows":
            root = Path(os.environ["LLVM_MINGW_ROOT"])
            readobj = root / "bin/llvm-readobj.exe"
            if not readobj.is_file():
                raise ValueError(f"Missing selected llvm-mingw PE auditor: {readobj}")

            def pe_output(path: Path, option: str) -> str:
                return subprocess.run([str(readobj), option, str(path)],
                                      text=True, capture_output=True,
                                      check=True).stdout

            def installed_library(part: str) -> Path:
                path = core / "bin" / f"libsaltscore_{part}-0.1.dll"
                if not path.is_file():
                    raise ValueError(f"Missing llvm-mingw 0.1 DLL: {path}")
                return path

            def exports_of(path: Path) -> set[str]:
                return set(re.findall(r"(?m)^\s*Name: (\S+)\s*$",
                                      pe_output(path, "--coff-exports")))

            def imports_of(path: Path) -> set[str]:
                return set(re.findall(
                    r"libsaltscore_(core|platform|concurrency|coroutine|native_io)-0[.]1[.]dll",
                    pe_output(path, "--coff-imports"), re.I))

            def build_image(name: str, executable: bool) -> Path:
                pattern = f"{name}.exe" if executable else f"lib{name}.dll"
                paths = {p.resolve() for p in build.rglob(pattern)}
                if len(paths) != 1:
                    raise ValueError(f"Expected one llvm-mingw image {pattern}: {paths}")
                return next(iter(paths))
        else:
            def installed_library(part: str) -> Path:
                return find_library(core, part, system)

            def exports_of(path: Path) -> set[str]:
                return image_exports(path, system, None)

            def imports_of(path: Path) -> set[str]:
                return binary_dependencies(path, system, None)

            def build_image(name: str, executable: bool) -> Path:
                return find_build_image(build, name, system, executable)

        images = {
            "core": installed_library("core"),
            "platform": installed_library("platform"),
            "native_io": installed_library("native_io"),
            "concurrency": installed_library("concurrency"),
            "provider": build_image("salts_service_host_provider_fixture", False),
            "ace_c": build_image("salts_owner_c11", True),
            "ace_cpp": build_image("salts_owner_cpp17", True),
            "cflow_c": build_image("salts_cflow_owner_c11", True),
            "cflow_cpp": build_image("salts_cflow_owner_cpp17", True),
        }
        for name, image in images.items():
            if not image.is_file():
                errors.append(f"Missing required {name} binary: {image}")
        if errors:
            raise ValueError("; ".join(errors))
        exports = {n: exports_of(p) for n, p in images.items()}
        imports = {n: imports_of(p) for n, p in images.items()}
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
        ids = {name: (images[name].name if system == "windows" else
                      native_abi_identity(images[name], name, system))
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
    except (RuntimeError, ValueError, OSError, KeyError,
            subprocess.CalledProcessError) as err:
        report["errors"] = errors or [str(err)]
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    for issue in report.get("errors", []):
        print("CMeta native ABI: " + issue, file=sys.stderr)
    print("CMeta native ABI: " + ("PASS" if report["ok"] else "FAIL"))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
