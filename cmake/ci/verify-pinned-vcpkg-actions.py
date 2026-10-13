#!/usr/bin/env python3
"""Fail closed on mutable qigao/vcpkg-cache composite Actions references.

This is a source/provenance policy, not an assertion that an external provider
or exact consumer SDK version is qualified without the relevant executed CI.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

PIN = "d4afc8cd2f5ed3585c1e26469e379a087ce9ecf3"
PROVIDER_SOURCE_BLOBS = {
    "setup-vcpkg-cache": "0a23ba176cbe8846ca164c2477f9a1e401cdc072",
    "setup-re2c-tools": "3bbe8cb2b96ebe8a17b1970448ac0b25c186eced",
}
EXTRACT = re.compile(
    r"^\s*(?:-\s*)?uses:\s+"
    r"(qigao/vcpkg-cache/.github/actions/(setup-vcpkg-cache|setup-re2c-tools))"
    r"@(\S+)\s*$"
)
ANY_PROVIDER = re.compile(
    r"^\s*(?:-\s*)?uses:\s+qigao/vcpkg-cache/.github/actions/"
)
EXACT_SHA = re.compile(r"^[0-9a-f]{40}$")
MIGRATED_FILES = (
    "native-build.yml",
    "cnet-tongsuo-rfc8998.yml",
    "cnet-module-once-tsan.yml",
    "cflow-1105-consumer-tsan.yml",
    "cstl-dso-odr-conformance.yml",
    "cnet-module-once-mixed-sdk-tsan.yml",
    "candidate-native-sdk.yml",
    "ace-candidate-native-sdk.yml",
)


def check(lines: dict[str, str]) -> dict:
    rows = []
    for filename, content in sorted(lines.items()):
        for lineno, line in enumerate(content.splitlines(), 1):
            if not ANY_PROVIDER.match(line):
                continue
            match = EXTRACT.fullmatch(line)
            if match is None:
                raise ValueError(
                    f"{filename}:{lineno}: unrecognized or unpinned vcpkg-cache action")
            action, name, sha = match.groups()
            if EXACT_SHA.fullmatch(sha) is None:
                raise ValueError(
                    f"{filename}:{lineno}: mutable/unreviewed provider ref {sha!r}")
            if filename in MIGRATED_FILES and sha != PIN:
                raise ValueError(
                    f"{filename}:{lineno}: reviewed pin changed: {sha} != {PIN}")
            rows.append(dict(
                workflow=filename, line=lineno, action=action, commit=sha,
                action_definition_blob=PROVIDER_SOURCE_BLOBS[name]
                if sha == PIN else None,
            ))

    for filename in MIGRATED_FILES:
        expected = 1 if filename in (
            "candidate-native-sdk.yml", "ace-candidate-native-sdk.yml"
        ) else 2
        actual = [x for x in rows if x["workflow"] == filename]
        if len(actual) != expected:
            raise ValueError(
                f"{filename}: expected {expected} immutable provider actions, "
                f"found {len(actual)}")
    if not any(x["workflow"] == "native-build.yml" for x in rows):
        raise ValueError("main native producer workflow has no pinned provider")
    return {
        "status": "PASS",
        "migrated_provider_revision": PIN,
        "pinned_action_definitions": PROVIDER_SOURCE_BLOBS,
        "migrated_workflows": len(MIGRATED_FILES),
        "migrated_actions": 14,
        "provider_references_total": len(rows),
        "references": rows,
        "runtime_check_required": True,
    }


def self_test() -> dict:
    fixture: dict[str, str] = {}
    for filename in MIGRATED_FILES:
        count = 1 if filename in (
            "candidate-native-sdk.yml", "ace-candidate-native-sdk.yml"
        ) else 2
        kinds = ("setup-re2c-tools",) if count == 1 else (
            "setup-re2c-tools", "setup-vcpkg-cache")
        fixture[filename] = "\n".join(
            f"      - uses: qigao/vcpkg-cache/.github/actions/{kind}@{PIN}"
            for kind in kinds)
    report = check(fixture)
    if report["status"] != "PASS":
        raise ValueError("valid exact-pin fixture was rejected")
    for invalid in ("master", "main", "v1", "123456789", "0" * 39 + "X"):
        invalid_fixture = dict(fixture)
        invalid_fixture["native-build.yml"] = invalid_fixture[
            "native-build.yml"].replace("@" + PIN, "@" + invalid, 1)
        try:
            check(invalid_fixture)
        except ValueError:
            pass
        else:
            raise ValueError(f"invalid provider action ref was accepted: {invalid}")
    return {
        "status": "PASS",
        "positive": "14 references in 8 changed workflows",
        "rejected": ["master", "main", "v1", "short SHA", "nonhex SHA"],
    }


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo", type=Path, default=Path("."))
    p.add_argument("--self-test", action="store_true")
    args = p.parse_args()
    try:
        if args.self_test:
            result = self_test()
        else:
            root = args.repo / ".github/workflows"
            if not root.is_dir():
                raise ValueError(f"GitHub workflows directory missing: {root}")
            docs = {}
            for suffix in ("*.yml", "*.yaml"):
                docs.update({
                    file.name: file.read_text(encoding="utf-8")
                    for file in root.glob(suffix)
                })
            result = check(docs)
    except (OSError, ValueError) as exc:
        print(f"VCPKG_ACTION_PIN_POLICY_FAILED: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
