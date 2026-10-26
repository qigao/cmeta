#!/usr/bin/env python3
"""Reject reintroduced native owners after CMeta-only Salts source cutover."""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
REMOVED = ("unicode", "idna", "cserde", "tinytest", "asn1", "uri")
for name in REMOVED:
    if (ROOT / name).exists():
        raise SystemExit(f"Duplicated native module retained under Salts: {name}")
root = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
for name in ("platform", "concurrency", "coroutine", "native-io", "cnet", "utils"):
    if f"add_subdirectory({name})" in root:
        raise SystemExit(f"Native module illegally built in ACE-only Salts: {name}")
if "find_package(SaltsCore 0.1 CONFIG REQUIRED)" not in root:
    raise SystemExit("SaltsCore installed dependency missing")
if "SALTS_CORE_BACKED_PROFILE" in root:
    raise SystemExit("Legacy native build option present")
print("PASS: migrated module directories removed; CMeta-only Salts build")
