#!/usr/bin/env python3
"""Enforce Salts as the only CMeta/ACE/CFlow source owner.

Source trees migrated to qigao/salts-core or qigao/crypto must not be
retained as unbuilt fallbacks, aliases, manifests or dormant test harnesses.
"""
from pathlib import Path
import argparse
import re

ROOT = Path(__file__).resolve().parents[1]
MIGRATED = (
    "platform", "concurrency", "coroutine", "native-io", "cnet",
    "unicode", "idna", "cserde", "tinytest", "asn1", "uri",
    "vendor", "presets", "utils", "component", "component-plugin",
    "tinymock", "simd",
)
for name in MIGRATED:
    if (ROOT / name).exists():
        raise SystemExit(f"Duplicate native ownership in Salts: {name}")
for name in ("vcpkg.json", "vcpkg-configuration.json"):
    if (ROOT / name).exists():
        raise SystemExit(f"Obsolete dependency/compatibility manifest: {name}")
for name in (".github/actions", "cmake/vcpkg-cache.nuget.config"):
    if (ROOT / name).exists():
        raise SystemExit(f"Obsolete native build dependency: {name}")
for name in ("ace/include/salts", "plugin/include/salts"):
    if (ROOT / name).exists():
        raise SystemExit(f"Repository-owned header restored under Core namespace: {name}")
if not (ROOT / "CMakeUserPresets.json").is_file():
    raise SystemExit("Missing version-controlled first-party user presets")

cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
required = (
    "project(CMeta VERSION 0.1.0 LANGUAGES C CXX)",
    "find_package(SaltsCore 0.1 CONFIG REQUIRED)",
    "add_subdirectory(cmeta)",
    "add_subdirectory(ace)",
    "add_subdirectory(cflow)",
)
for item in required:
    if item not in cmake:
        raise SystemExit(f"Missing sole ACE owner: {item}")
for line in cmake.splitlines():
    if re.search(r"\badd_subdirectory\((?:platform|concurrency|coroutine|native-io|cnet|utils|uri|unicode|idna|cserde|tinytest|asn1|vendor)\)", line):
        raise SystemExit(f"Native implementation compiled by Salts: {line}")
if "SALTS_CORE_BACKED_PROFILE" in cmake or "GmSSL::" in cmake or "SaltsCore::" in cmake:
    raise SystemExit("Compatibility namespace or second native source owner")
if "NAMESPACE CMeta::" not in cmake or "NAMESPACE Salts::" in cmake:
    raise SystemExit("Repository-owned exports must use only the CMeta:: namespace")

# Check active retained targets, not just the root. Forbid restoring former
# native provider namespaces by hidden CMake source/profile declarations.
for owner in (
    "cmeta", "cflow", "cstl",
    "ace", "plugin",
):
    cmake_path = ROOT / owner / "CMakeLists.txt"
    if not cmake_path.is_file():
        raise SystemExit(f"Missing retained CMeta/ACE source module: {owner}")
    active_cmake = cmake_path.read_text(encoding="utf-8")
    for old in (
        "SaltsCore::", "SaltsCrypto::", "Crypto::", "GmSSL::",
        "ALIAS Salts::",
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
        "cnet/src/cnet_owner.c",
        "cnet/src/cnet_ipc.c",
        "sctp/src/sctp_session.c",
        "uri/src/uri_parser.c",
        "unicode/src/salts_unicode_case.c",
        "idna/src/salts_idna.c",
        "cserde/src/reader.c",
        "tinytest/src/tinytest.c",
        "asn1/src/reader.c",
        "fs/src/fs_root.c",
        "process/src/process.c",
        "core/include/vstr.h",
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

parser = argparse.ArgumentParser(description="Fail-closed Salts ACE/Core source closure")
parser.add_argument("--core-root", type=Path,
                    help="Checked-out, pinned qigao/salts-core source root")
arguments = parser.parse_args()
if arguments.core_root is not None:
    verify_core_owner(arguments.core_root.resolve())

print("PASS: sole native Core and sole CMeta/ACE/CFlow source ownership")
