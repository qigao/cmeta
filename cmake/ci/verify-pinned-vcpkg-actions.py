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
EXISTING_CANDIDATE_VCPKG_PIN = "fafb1efe5ac2440bfd7a08e493988d0459d89e51"
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
PINNED_PROVIDER_FILES = (
    "cnet-tongsuo-rfc8998.yml",
    "cnet-module-once-tsan.yml",
    "cnet-module-once-mixed-sdk-tsan.yml",
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
            if filename in PINNED_PROVIDER_FILES:
                expected = (
                    EXISTING_CANDIDATE_VCPKG_PIN
                    if filename == "ace-candidate-native-sdk.yml"
                    and name == "setup-vcpkg-cache"
                    else PIN
                )
                if sha != expected:
                    raise ValueError(
                        f"{filename}:{lineno}: reviewed pin changed: {sha} != {expected}")
            rows.append(dict(
                workflow=filename, line=lineno, action=action, commit=sha,
                action_definition_blob=PROVIDER_SOURCE_BLOBS[name]
                if sha == PIN else None,
            ))

    for filename in PINNED_PROVIDER_FILES:
        actual = [x for x in rows if x["workflow"] == filename]
        if len(actual) != 2 or {
            x["action"].rsplit("/", 1)[-1] for x in actual
        } != {"setup-vcpkg-cache", "setup-re2c-tools"}:
            raise ValueError(
                f"{filename}: expected one exact vcpkg and one exact re2c Action")
    migrated_count = sum(
        x["workflow"] in PINNED_PROVIDER_FILES and x["commit"] == PIN for x in rows)
    # Each retained workflow has two provider actions; ACE keeps its vcpkg pin.
    expected_migrated_count = 2 * len(PINNED_PROVIDER_FILES) - 1
    if migrated_count != expected_migrated_count:
        raise ValueError(
            f"expected {expected_migrated_count} reviewed refs, got {migrated_count}")
    native = lines.get("native-build.yml", "")
    if not re.search(r"^\s*uses:\s+\./\.github/actions/setup-vcpkg-curl-cache\s*$", native, re.MULTILINE):
        raise ValueError("main native producer workflow must use the local curl-only cache action")
    if re.search(r"mono-complete|brew install mono|setup-vcpkg-cache@|setup-re2c-tools@|dotnet\s+(?:restore|nuget)", native, re.IGNORECASE):
        raise ValueError("main native producer workflow must not install Mono or invoke NuGet restore")
    return {
        "status": "PASS",
        "migrated_provider_revision": PIN,
        "pinned_action_definitions": PROVIDER_SOURCE_BLOBS,
        "migrated_workflows": len(PINNED_PROVIDER_FILES),
        "migrated_actions": migrated_count,
        "preserved_candidate_vcpkg_commit": EXISTING_CANDIDATE_VCPKG_PIN,
        "provider_references_total": len(rows),
        "references": rows,
        "runtime_check_required": True,
    }


def self_test() -> dict:
    fixture: dict[str, str] = {
        "native-build.yml": "      - uses: ./.github/actions/setup-vcpkg-curl-cache",
    }
    for filename in PINNED_PROVIDER_FILES:
        fixture[filename] = "\\n".join(
            f"      - uses: qigao/vcpkg-cache/.github/actions/{kind}@"
            + (EXISTING_CANDIDATE_VCPKG_PIN
               if filename == "ace-candidate-native-sdk.yml"
               and kind == "setup-vcpkg-cache" else PIN)
            for kind in ("setup-re2c-tools", "setup-vcpkg-cache"))
    report = check(fixture)
    if report["status"] != "PASS":
        raise ValueError("valid exact-pin fixture was rejected")
    for invalid in ("master", "main", "v1", "123456789", "0" * 39 + "X"):
        invalid_fixture = dict(fixture)
        invalid_fixture["cnet-tongsuo-rfc8998.yml"] = invalid_fixture[
            "cnet-tongsuo-rfc8998.yml"].replace("@" + PIN, "@" + invalid, 1)
        try:
            check(invalid_fixture)
        except ValueError:
            pass
        else:
            raise ValueError(f"invalid provider action ref was accepted: {invalid}")
    return {
        "status": "PASS",
        "positive": f"{report['migrated_actions']} reviewed SHA pins plus 1 preserved ACE vcpkg pin",
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
            action = args.repo / ".github/actions/setup-vcpkg-curl-cache/action.yml"
            transport = args.repo / "cmake/ci/github-packages.py"
            setup = args.repo / "cmake/ci/setup-vcpkg-curl-cache.py"
            restore = args.repo / "cmake/ci/restore-vcpkg-cache.py"
            for required in (action, transport, setup, restore):
                if not required.is_file():
                    raise ValueError(f"curl-only cache implementation missing: {required}")
            action_text = action.read_text(encoding="utf-8")
            if re.search(r"mono-complete|brew install mono|dotnet\s+(?:restore|nuget)|setup-vcpkg-cache@", action_text, re.IGNORECASE):
                raise ValueError("curl-only cache action must not install Mono or invoke NuGet restore")
    except (OSError, ValueError) as exc:
        print(f"VCPKG_ACTION_PIN_POLICY_FAILED: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
