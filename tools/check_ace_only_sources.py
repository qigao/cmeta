#!/usr/bin/env python3
"""Enforce Salts 3 as the only CMeta/ACE/CFlow source owner.

Source trees migrated to qigao/salts-core or qigao/crypto must not be
retained as unbuilt fallbacks, aliases, manifests or dormant test harnesses.
The original SIMD donor remains pending provenance/upper-layer acceptance;
it is retained on disk but must never enter the active Salts build graph.
"""
from pathlib import Path
import argparse
import re

ROOT = Path(__file__).resolve().parents[1]
MIGRATED = (
    "platform", "concurrency", "coroutine", "native-io", "cnet",
    "unicode", "idna", "cserde", "tinytest", "asn1", "uri",
    "vendor", "presets", "utils",
)
for name in MIGRATED:
    if (ROOT / name).exists():
        raise SystemExit(f"Duplicate native ownership in Salts: {name}")
for name in ("vcpkg.json", "vcpkg-configuration.json", "CMakeUserPresets.json"):
    if (ROOT / name).exists():
        raise SystemExit(f"Obsolete dependency/compatibility manifest: {name}")
for name in (".github/actions", "cmake/vcpkg-cache.nuget.config"):
    if (ROOT / name).exists():
        raise SystemExit(f"Obsolete native build dependency: {name}")

cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
required = (
    "project(Salts VERSION 3.0.0 LANGUAGES C CXX)",
    "find_package(SaltsCore 0.1 CONFIG REQUIRED)",
    "add_subdirectory(cmeta)",
    "add_subdirectory(component-runtime)",
    "add_subdirectory(cflow)",
)
for item in required:
    if item not in cmake:
        raise SystemExit(f"Missing sole ACE owner: {item}")
if "SALTS_CORE_BACKED_PROFILE" in cmake or "GmSSL::" in cmake or "SaltsCore::" in cmake:
    raise SystemExit("Compatibility namespace or second native source owner")

# Presence of this donor is not a completed-migration claim. The full frozen
# source/API ledger remains the authority for final migration completeness.
for name in (
    "CMakeLists.txt", "README.md", "include/salts/simd.h", "src/simd.c",
    "tests/CMakeLists.txt", "tests/simd_test.c", "tests/simd_header_cpp_test.cpp",
):
    if not (ROOT / "simd" / name).is_file():
        raise SystemExit(f"Pending original SIMD donor absent: simd/{name}")

# Check active retained targets, not just the root. Forbid restoring former
# native provider namespaces by hidden CMake source/profile declarations.
active_cmake_paths = [ROOT / "CMakeLists.txt"]
for owner in (
    "cmeta", "cflow", "cstl", "tinymock", "component",
    "component-plugin", "component-runtime", "plugin",
):
    cmake_path = ROOT / owner / "CMakeLists.txt"
    if not cmake_path.is_file():
        raise SystemExit(f"Missing retained CMeta/ACE source module: {owner}")
    active_cmake_paths.append(cmake_path)

for cmake_path in active_cmake_paths:
    active_cmake = cmake_path.read_text(encoding="utf-8")
    if re.search(
        r"\badd_subdirectory\s*\(\s*[\"]?(?:\./)?"
        r"(?:platform|concurrency|coroutine|native-io|cnet|utils|uri|unicode|"
        r"idna|cserde|tinytest|asn1|vendor|simd)(?:[/\s\"\)]|$)",
        active_cmake,
    ) or re.search(r"\bsimd/(?:src|include)(?:/|[\"\s\)])", active_cmake):
        raise SystemExit(f"Native implementation compiled or installed by Salts: {cmake_path}")
    if re.search(r"\b(?:ALIAS\s+Salts::SIMD|add_library\s*\(\s*Salts::SIMD\b)", active_cmake):
        raise SystemExit(f"Second Salts::SIMD provider in {cmake_path}")
    for old in (
        "SaltsCore::", "SaltsCrypto::", "Crypto::", "GmSSL::",
        "SALTS_CORE_BACKED_PROFILE", "SALTS_PLUGIN_USE_SALTSCORE",
        "SALTS_CFLOW_USE_SALTSCORE",
    ):
        if old in active_cmake:
            raise SystemExit(f"Disallowed native/compatibility target in {cmake_path}: {old}")

def verify_core_owner(core: Path) -> None:
    """Ensure all removed native files have one source owner, never shims."""
    required = (
        "core/src/cmeta_buffer.c",
        "platform/src/c11_threads.c",
        "concurrency/src/disruptor.c",
        "coroutine/src/coro.c",
        "native-io/src/native_io.c",
        "cnet/CMakeLists.txt",
        "cnet/src/cnet_owner.c",
        "cnet/src/cnet_ipc.c",
        "sctp/src/sctp_session.c",
        "uri/src/uri_parser.c",
        "unicode/src/salts_unicode_case.c",
        "idna/src/salts_idna.c",
        "cserde/src/reader.c",
        "tinytest/src/tinytest.c",
        "fs/src/fs_root.c",
        "process/src/process.c",
        "core/include/vstr.h",
        "simd/CMakeLists.txt",
        "simd/src/simd.c",
        "simd/src/simd_types.c",
        "simd/include/salts/simd.h",
        "simd/include/salts/simd_types.h",
        "cmake/SaltsCoreConfig.cmake.in",
    )
    for name in required:
        if not (core / name).is_file():
            raise SystemExit(f"Native implementation absent from sole Core owner: {name}")
    vstr = (core / "core/include/vstr.h").read_text(encoding="utf-8")
    if "CMETA_" in vstr or re.search(r"#\s*include\s*[<\"]cmeta[/>\"]", vstr):
        raise SystemExit("Native borrowed vstr must not depend on CMeta reflection")
    c = (core / "CMakeLists.txt").read_text(encoding="utf-8")
    if "NAMESPACE Salts::" not in c or "add_library(Salts::CNet ALIAS" not in (
        core / "cnet/CMakeLists.txt"
    ).read_text(encoding="utf-8"):
        raise SystemExit("Canonical Salts:: native target owners missing from Core")
    simd = (core / "simd/CMakeLists.txt").read_text(encoding="utf-8")
    if "add_subdirectory(simd)" not in c or any(item not in simd for item in (
        "add_library(saltscore_simd STATIC src/simd.c src/simd_types.c)",
        "add_library(Salts::SIMD ALIAS saltscore_simd)",
        "EXPORT_NAME SIMD", "ARCHIVE DESTINATION",
    )):
        raise SystemExit("Canonical static Salts::SIMD source owner missing from Core")
    if "Salts::CMeta" in simd:
        raise SystemExit("Native SIMD must not link back to CMeta reflection")
    for name in (
        "simd/src/simd.c", "simd/src/simd_types.c",
        "simd/include/salts/simd.h", "simd/include/salts/simd_types.h",
    ):
        contents = (core / name).read_text(encoding="utf-8")
        if re.search(r"#\s*include\s*[<\"]cmeta[/>\"]", contents):
            raise SystemExit(f"Native SIMD must not include CMeta reflection: {name}")

parser = argparse.ArgumentParser(description="Fail-closed Salts ACE/Core source closure")
parser.add_argument("--core-root", type=Path,
                    help="Checked-out, pinned qigao/salts-core source root")
arguments = parser.parse_args()
if arguments.core_root is not None:
    verify_core_owner(arguments.core_root.resolve())

print("PASS: sole native Core and sole CMeta/ACE/CFlow source ownership")
