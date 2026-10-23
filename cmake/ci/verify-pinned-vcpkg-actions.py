#!/usr/bin/env python3
"""Fail closed on mutable cache providers and enforce the curl-only CI transport."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys


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
CURL_MIGRATED_FILES = (
    "native-build.yml",
    "cnet-tongsuo-rfc8998.yml",
    "cnet-module-once-tsan.yml",
    "cnet-module-once-mixed-sdk-tsan.yml",
    "cnet-databind-source-asan.yml",
    "ace-candidate-native-sdk.yml",
)
FORBIDDEN_MIGRATED = re.compile(
    r"mono-complete|brew\s+install\s+mono|setup-vcpkg-cache@|setup-re2c-tools@|"
    r"dotnet\s+(?:restore|nuget)",
    re.IGNORECASE,
)


def check(lines: dict[str, str]) -> dict:
    rows = []
    for filename, content in sorted(lines.items()):
        if re.search(r"mono-complete|brew\s+install\s+mono|dotnet\s+restore", content, re.IGNORECASE):
            raise ValueError(f"{filename}: Mono installation or dotnet restore is forbidden in CI")
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
            rows.append(dict(
                workflow=filename,
                line=lineno,
                action=action,
                commit=sha,
                action_definition_blob=PROVIDER_SOURCE_BLOBS.get(name),
            ))

    for filename in CURL_MIGRATED_FILES:
        body = lines.get(filename)
        if body is None:
            raise ValueError(f"required migrated workflow is missing: {filename}")
        if len(re.findall(
                r"^\s*(?:-\s*)?uses:\s+\./\.github/actions/setup-vcpkg-curl-cache\s*$",
                body, re.MULTILINE)) != 1:
            raise ValueError(
                f"{filename}: expected exactly one local curl-only cache action")
        for cache_ref in re.findall(r"^\s*cache-ref:\s*([^\s#]+)", body, re.MULTILINE):
            if EXACT_SHA.fullmatch(cache_ref) is None:
                raise ValueError(f"{filename}: mutable curl cache-ref {cache_ref!r}")
        forbidden = FORBIDDEN_MIGRATED.search(body)
        if forbidden:
            raise ValueError(
                f"{filename}: forbidden managed package transport remains: {forbidden.group(0)!r}")

    return {
        "status": "PASS",
        "curl_migrated_workflows": list(CURL_MIGRATED_FILES),
        "external_provider_references": rows,
        "runtime_check_required": True,
    }


def self_test() -> dict:
    fixture = {
        filename: "      - uses: ./.github/actions/setup-vcpkg-curl-cache\n"
        for filename in CURL_MIGRATED_FILES
    }
    fixture["legacy-pinned.yml"] = (
        "      - uses: qigao/vcpkg-cache/.github/actions/setup-vcpkg-cache@"
        + "d4afc8cd2f5ed3585c1e26469e379a087ce9ecf3\n"
    )
    report = check(fixture)
    if report["status"] != "PASS":
        raise ValueError("valid curl-only fixture was rejected")

    invalid = dict(fixture)
    invalid["native-build.yml"] += "      - run: sudo apt-get install mono-complete\n"
    try:
        check(invalid)
    except ValueError:
        pass
    else:
        raise ValueError("Mono installation was accepted in migrated workflow")

    for bad_ref in ("master", "main", "v1", "123456789", "0" * 39 + "X"):
        invalid = dict(fixture)
        invalid["legacy-pinned.yml"] = (
            "      - uses: qigao/vcpkg-cache/.github/actions/setup-vcpkg-cache@"
            + bad_ref + "\n"
        )
        try:
            check(invalid)
        except ValueError:
            pass
        else:
            raise ValueError(f"invalid provider action ref was accepted: {bad_ref}")

    return {
        "status": "PASS",
        "curl_only": list(CURL_MIGRATED_FILES),
        "rejected": ["Mono install", "mutable provider refs", "short/nonhex provider SHAs"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            result = self_test()
        else:
            workflows = args.repo / ".github/workflows"
            if not workflows.is_dir():
                raise ValueError(f"GitHub workflows directory missing: {workflows}")
            docs = {}
            for suffix in ("*.yml", "*.yaml"):
                docs.update({
                    file.name: file.read_text(encoding="utf-8")
                    for file in workflows.glob(suffix)
                })
            result = check(docs)

            required = (
                args.repo / ".github/actions/setup-vcpkg-curl-cache/action.yml",
                args.repo / "cmake/ci/github-packages.py",
                args.repo / "cmake/ci/setup-vcpkg-curl-cache.py",
                args.repo / "cmake/ci/restore-vcpkg-cache.py",
            )
            for path in required:
                if not path.is_file():
                    raise ValueError(f"curl-only cache implementation missing: {path}")
            combined = "\n".join(path.read_text(encoding="utf-8") for path in required)
            if re.search(
                    r"mono-complete|brew\s+install\s+mono|dotnet\s+(?:restore|nuget)|"
                    r"setup-vcpkg-cache@|setup-re2c-tools@",
                    combined, re.IGNORECASE):
                raise ValueError("curl-only implementation contains managed package transport")
    except (OSError, ValueError) as exc:
        print(f"VCPKG_ACTION_PIN_POLICY_FAILED: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
