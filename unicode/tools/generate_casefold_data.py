#!/usr/bin/env python3
"""Generate immutable Unicode 17 default full C/F casefold data."""
from __future__ import annotations

import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "data" / "17.0.0" / "CaseFolding.txt"
OUTPUT = ROOT / "src" / "unicode_casefold_data.h"


def generated() -> str:
    raw = SOURCE.read_text(encoding="utf-8")
    if not raw.startswith("# CaseFolding-17.0.0.txt\n"):
        raise ValueError("Expected Unicode 17.0.0 CaseFolding source")
    records: dict[int, tuple[str, tuple[int, ...]]] = {}
    for line in raw.splitlines():
        value = line.partition("#")[0].strip()
        if not value:
            continue
        fields = [field.strip() for field in value.split(";")]
        if len(fields) < 3 or fields[1] not in ("C", "F", "S", "T"):
            raise ValueError(f"Malformed fold mapping: {value}")
        scalar, status = int(fields[0], 16), fields[1]
        if status not in ("C", "F"):
            continue
        mapped = tuple(int(item, 16) for item in fields[2].split())
        if not mapped or any(x > 0x10FFFF or 0xD800 <= x <= 0xDFFF for x in mapped):
            raise ValueError(f"Invalid fold value: {value}")
        if scalar not in records or status == "F" or records[scalar][0] != "F":
            records[scalar] = (status, mapped)

    entries: list[tuple[int, int, int]] = []
    values: list[int] = []
    for scalar, (_status, mapped) in sorted(records.items()):
        if mapped == (scalar,):
            continue
        entries.append((scalar, len(values), len(mapped)))
        values.extend(mapped)
    if len(entries) != 1585 or len(values) != 1705 or max(row[2] for row in entries) > 3:
        raise ValueError("Unexpected Unicode 17 mapping count; review source and version")

    lines = [
        "/* Generated from Unicode 17 CaseFolding.txt by unicode/tools/generate_casefold_data.py.",
        " * Data license: unicode/data/LICENSE.txt. Do not modify by hand.",
        " * Authoritative Unicode 17 default C/F mappings; exclude S/T alternates.",
        " */",
        "static const uint32_t salts_unicode_casefold_values[] = {",
    ]
    lines.extend(f"  0x{value:x}u," for value in values)
    lines.append("};")
    lines.append("static const salts_unicode_fold_mapping salts_unicode_casefold_mappings[] = {")
    lines.extend(f"  {{0x{scalar:x}u, {offset}u, {length}u}}," for scalar, offset, length in entries)
    lines.append("};")
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    data = generated()
    if args.check:
        if OUTPUT.read_text(encoding="ascii") != data:
            raise SystemExit("Generated Unicode casefold header differs from source")
    else:
        OUTPUT.write_text(data, encoding="ascii")


if __name__ == "__main__":
    main()
