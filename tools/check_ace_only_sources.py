#!/usr/bin/env python3
"""Enforce Salts 3 as the only CMeta/ACE/CFlow source owner.

Source trees migrated to qigao/salts-core or qigao/crypto must not be
retained as unbuilt fallbacks, aliases, manifests or dormant test harnesses.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MIGRATED = (
    "platform", "concurrency", "coroutine", "native-io", "cnet",
    "unicode", "idna", "cserde", "tinytest", "asn1", "uri",
    "vendor", "presets",
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
for line in cmake.splitlines():
    if re.search(r"\badd_subdirectory\((?:platform|concurrency|coroutine|native-io|cnet|utils|uri|unicode|idna|cserde|tinytest|asn1|vendor)\)", line):
        raise SystemExit(f"Native implementation compiled by Salts: {line}")
if "SALTS_CORE_BACKED_PROFILE" in cmake or "GmSSL::" in cmake or "SaltsCore::" in cmake:
    raise SystemExit("Compatibility namespace or second native source owner")
print("PASS: Salts 3.0 has only CMeta/ACE/CFlow owners")
