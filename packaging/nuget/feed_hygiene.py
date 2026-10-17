#!/usr/bin/env python3
"""Salts.Native public-feed policy and exact rc.sha quarantine audit.

No network, credentials or destructive operations. A separate manual workflow
must verify a concrete package VERSION and admin authorization before deletion.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
import tempfile
from typing import Any

RIDS = frozenset({
    "linux-x64", "linux-arm64", "windows-x64", "macos-arm64",
    "android-arm64-v8a", "ios-arm64",
})
RC_SHA = re.compile(r"^2[.]3[.]0-rc[.]sha[0-9a-f]{40}$")
OFFICIAL = re.compile(r"^2[.]3[.]0(?:-rc[.][1-9][0-9]*)?$")
RC_MARKER = "sdk"
PUBLISHING = ".github/workflows/native-sdk-rc-release.yml"
CANDIDATES = (
    ".github/workflows/ace-candidate-native-sdk.yml",
)
REQUIRED_TAG_CHECKS = (
    "refs/tags/v",
    "test \"$TAG\" = \"v$RELEASE_VERSION\"",
    "Verify NuGet contents, version and source SHA",
    "linux-x64", "linux-arm64", "windows-x64", "macos-arm64",
    "android-arm64-v8a", "ios-arm64",
)


class FeedPolicyError(ValueError):
    pass


def ensure(condition: bool, message: str) -> None:
    if not condition:
        raise FeedPolicyError(message)


def semver_prerelease_less(left: str, right: str) -> bool:
    """SemVer 2 comparison needed to prove rc.N < rc.sha* for same base."""
    ensure(left.startswith("2.3.0-") and right.startswith("2.3.0-"),
           "compare only same-base 2.3.0 prereleases")
    a, b = left.split("-", 1)[1].split("."), right.split("-", 1)[1].split(".")
    for p, q in zip(a, b):
        if p == q:
            continue
        p_num, q_num = p.isascii() and p.isdigit(), q.isascii() and q.isdigit()
        if p_num and q_num:
            return int(p) < int(q)
        if p_num != q_num:
            return p_num
        return p < q
    return len(a) < len(b)


def check_policy(root: Path) -> dict[str, Any]:
    ensure(semver_prerelease_less("2.3.0-rc.15", "2.3.0-rc.sha" + "a" * 40),
           "incorrect SemVer safeguard: rc.sha must outrank numeric rc")
    for item in CANDIDATES:
        path = root / item
        ensure(path.is_file(), f"missing candidate workflow: {item}")
        s = path.read_text(encoding="utf-8")
        ensure("actions/upload-artifact@" in s and "dist/*.nupkg" in s,
               f"candidate artifact lost: {item}")
        ensure("dotnet nuget push" not in s, f"incomplete NuGet feed push: {item}")
        ensure("packages: write" not in s, f"candidate has feed-write permission: {item}")
        ensure("Publish immutable prerelease" not in s
               and "Publish opt-in immutable prerelease" not in s,
               f"stale candidate publication action: {item}")
    publisher = (root / PUBLISHING).read_text(encoding="utf-8")
    for x in REQUIRED_TAG_CHECKS:
        ensure(x in publisher, f"tagged release missing guard {x!r}")
    ensure("dotnet nuget push" in publisher and "packages: write" in publisher,
           "official six-RID tag workflow must remain the only public publisher")
    ensure("on:" in publisher or "'on':" in publisher,
           "official tag workflow lacks trigger declaration")
    for path in (root / ".github/workflows").glob("*.yml"):
        if path.as_posix().endswith(PUBLISHING):
            continue
        data = path.read_text(encoding="utf-8")
        ensure("dotnet nuget push" not in data,
               f"unauthorized public NuGet publisher: {path.name}")
    return {"status": "PASS", "rc_sha_versions_are_higher_than_numeric_rc": True,
            "candidate_workflows_are_artifact_only": list(CANDIDATES),
            "only_public_publisher": PUBLISHING, "expected_rids": sorted(RIDS)}


def inspect_extracted(version: str, root: Path) -> dict[str, Any]:
    ensure(RC_SHA.fullmatch(version) is not None,
           "manual cleanup accepts only 2.3.0-rc.sha + exact 40 lowercase hex")
    sdk = root / RC_MARKER
    ensure(sdk.is_dir(), f"missing package sdk/ directory: {sdk}")
    found = {p.name for p in sdk.iterdir() if p.is_dir()}
    ensure(found and found.issubset(RIDS),
           f"unexpected or missing SDK RID directory: {sorted(found)}")
    for rid in found:
        prefix = sdk / rid
        config = prefix / "lib/cmake/Salts/SaltsConfig.cmake"
        manifest = prefix / "salts-sdk-manifest.txt"
        ensure(config.is_file(), f"SDK cannot be inspected: missing {config}")
        ensure(manifest.is_file(), f"SDK provenance missing: {manifest}")
        fields = dict(row.split("=", 1) for row in
                      manifest.read_text(encoding="utf-8-sig").splitlines()
                      if "=" in row)
        ensure(fields.get("version") == version,
               f"SDK version mismatch in {manifest}")
        ensure(fields.get("commit") == version.rsplit(".sha", 1)[1],
               f"SDK source SHA mismatch in {manifest}")
    missing = sorted(RIDS - found)
    ensure(missing, "package is complete: NEVER delete official six-RID SDK")
    # This verdict is a positive *candidate for manual removal*, NOT a deletion.
    return {
        "status": "REVIEW_ELIGIBLE_INCOMPLETE_RC_SHA",
        "version": version,
        "present_rids": sorted(found),
        "missing_rids": missing,
        "source_sha": version.rsplit(".sha", 1)[1],
        "automatic_deletion_authorized": False,
    }


def load_versions(path: Path) -> list[dict[str, Any]]:
    """Read one or more JSON arrays, as emitted by gh api --paginate."""
    raw = path.read_text(encoding="utf-8")
    decoder = json.JSONDecoder()
    offset = 0
    result: list[dict[str, Any]] = []
    while offset < len(raw):
        while offset < len(raw) and raw[offset].isspace():
            offset += 1
        if offset == len(raw):
            break
        obj, offset = decoder.raw_decode(raw, offset)
        ensure(isinstance(obj, list), "expected GitHub package-version arrays")
        ensure(all(isinstance(v, dict) for v in obj), "bad version records")
        result.extend(obj)
    return result


def audit_versions(path: Path, clean: bool) -> dict[str, Any]:
    versions = load_versions(path)
    candidates = [
        {"id": v.get("id"), "name": v.get("name")}
        for v in versions if isinstance(v.get("name"), str)
        and RC_SHA.fullmatch(v["name"])
    ]
    ids = [c["id"] for c in candidates]
    ensure(all(isinstance(i, int) and i > 0 for i in ids),
           "candidate package version metadata lacks stable numeric ID")
    ensure(len(ids) == len(set(ids)), "duplicate package version IDs in response")
    report = {"status": "CLEAN" if not candidates else "RC_SHA_POLLUTION_PRESENT",
              "total_versions": len(versions), "unsafe_rc_sha": candidates}
    if clean:
        ensure(not candidates, f"GitHub Packages still holds {len(candidates)} rc.sha candidates")
    return report



def audit_floating_resolution(assets_path: Path, packages_root: Path) -> dict[str, Any]:
    """Accept only the REAL NuGet-resolved, official complete six-RID SDK.

    No override, synthetic version selection, downloaded Release package
    substitution, or false source path is permitted in the runtime workflow.
    This is intentionally RED while old rc.sha versions outrank numeric RCs.
    """
    data = json.loads(assets_path.read_text(encoding="utf-8"))
    libraries = data.get("libraries")
    targets = data.get("targets")
    ensure(isinstance(libraries, dict) and isinstance(targets, dict),
           "incomplete NuGet project.assets.json (no libraries/targets)")
    matches = []
    for key, metadata in libraries.items():
        if not isinstance(key, str) or not key.lower().startswith("salts.native/"):
            continue
        ensure(isinstance(metadata, dict) and metadata.get("type") == "package",
               f"Salts.Native resolution was not an actual package: {key}")
        matches.append(key.split("/", 1)[1])
    ensure(len(matches) == 1,
           f"expected exactly one actual Salts.Native package resolution: {matches}")
    resolved = matches[0]
    included_targets = [
        key for framework in targets.values() if isinstance(framework, dict)
        for key in framework if isinstance(key, str)
        and key.lower() == ("salts.native/" + resolved).lower()
    ]
    ensure(len(included_targets) == 1,
           "resolved Salts.Native package is absent or duplicated in NuGet targets")
    ensure(not RC_SHA.fullmatch(resolved),
           f"RC_SHA_POLLUTION_PRESENT: NuGet floating 2.3.0-* resolved unsafe {resolved}")
    ensure(OFFICIAL.fullmatch(resolved) is not None,
           f"not an official numeric RC/stable 2.3.0 version: {resolved}")

    root = packages_root / "salts.native" / resolved.lower()
    ensure(root.is_dir(), f"actual restored global package is missing: {root}")
    sdk = root / "sdk"
    ensure(sdk.is_dir(), f"resolved package lacks sdk/: {root}")
    found = {p.name for p in sdk.iterdir() if p.is_dir()}
    ensure(found == RIDS,
           f"RESOLVED_SDK_INCOMPLETE: version={resolved}, missing={sorted(RIDS - found)}, "
           f"unexpected={sorted(found - RIDS)}")

    sha_set = set()
    for rid in sorted(RIDS):
        prefix = sdk / rid
        required = (
            "salts-sdk-manifest.txt",
            "lib/cmake/Salts/SaltsConfig.cmake",
            "lib/cmake/Salts/SaltsTargets.cmake",
            "include/cmeta/function.h",
            "include/cnet/cnet.h",
            "include/unicode/salts_unicode.h",
        )
        for name in required:
            ensure((prefix / name).is_file(),
                   f"RESOLVED_SDK_INCOMPLETE: {resolved}, {rid}, missing {name}")
        fields = dict(line.split("=", 1) for line in
                      (prefix / "salts-sdk-manifest.txt").read_text(
                          encoding="utf-8-sig").splitlines() if "=" in line)
        ensure(fields.get("version") == resolved,
               f"{rid}: SDK manifest version disagrees with NuGet resolution")
        ensure(fields.get("profile") == "release",
               f"{rid}: candidate-only SDK not an official release")
        sha = fields.get("commit", "")
        ensure(re.fullmatch(r"[0-9a-f]{40}", sha) is not None,
               f"{rid}: expected 40-character immutable source SHA")
        sha_set.add(sha)
    ensure(len(sha_set) == 1,
           f"resolved package has inconsistent source commits across six RIDs: {sha_set}")
    source = next(iter(sha_set))
    return {
        "status": "PASS",
        "floating_requirement": "2.3.0-*",
        "actual_resolved_version": resolved,
        "actual_source_commit": source,
        "source_is_immutable_tag_required": True,
        "sdk_rids": sorted(RIDS),
        "package_root": str(root),
        "no_synthetic_override": True,
    }


def floating_self_test() -> dict[str, Any]:
    """Keep the floating admission gate deterministic and offline in PR CI."""
    sha = "21bdb1145fbb2b39ee3fa644173ec037adba7b87"
    release = "2.3.0-rc.15"
    with tempfile.TemporaryDirectory(prefix="salts-floating-restore-") as temp:
        root = Path(temp)
        assets = root / "project.assets.json"
        cache = root / "packages"
        prefix = cache / "salts.native" / release / "sdk"

        def write_assets(version: str) -> None:
            key = "Salts.Native/" + version
            assets.write_text(json.dumps({
                "libraries": {key: {"type": "package", "path": "salts.native/" + version}},
                "targets": {"net8.0": {key: {"type": "package"}}},
            }))
        for rid in RIDS:
            target = prefix / rid
            for file in (
                "lib/cmake/Salts/SaltsConfig.cmake",
                "lib/cmake/Salts/SaltsTargets.cmake",
                "include/cmeta/function.h",
                "include/cnet/cnet.h",
                "include/unicode/salts_unicode.h",
            ):
                dest = target / file
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text("installed\n")
            (target / "salts-sdk-manifest.txt").write_text(
                f"version={release}\ncommit={sha}\nprofile=release\n")
        write_assets(release)
        passed = audit_floating_resolution(assets, cache)
        ensure(passed["status"] == "PASS" and len(passed["sdk_rids"]) == 6,
               "official six-RID fixture did not pass")
        poisoned = "2.3.0-rc.sha" + "b31598521b23f7833a999eb05733e846b6174644"
        write_assets(poisoned)
        try:
            audit_floating_resolution(assets, cache)
        except FeedPolicyError as exc:
            ensure("RC_SHA_POLLUTION_PRESENT" in str(exc),
                   "poisoned floating resolver rejected for wrong reason")
        else:
            raise FeedPolicyError("SHA prerelease improperly admitted as official")
        write_assets(release)
        missing = prefix / "windows-x64" / "lib/cmake/Salts/SaltsConfig.cmake"
        missing.unlink()
        try:
            audit_floating_resolution(assets, cache)
        except FeedPolicyError as exc:
            ensure("RESOLVED_SDK_INCOMPLETE" in str(exc),
                   "incomplete resolved package rejected for wrong reason")
        else:
            raise FeedPolicyError("incomplete six-RID official package accepted")
        missing.write_text("installed\n")
        manifest = prefix / "macos-arm64" / "salts-sdk-manifest.txt"
        manifest.write_text(f"version={release}\ncommit={'f'*40}\nprofile=release\n")
        try:
            audit_floating_resolution(assets, cache)
        except FeedPolicyError as exc:
            ensure("inconsistent source commits" in str(exc),
                   "mixed-SHA package rejected for wrong reason")
        else:
            raise FeedPolicyError("cross-RID source provenance violation accepted")
    return {
        "status": "PASS",
        "scenarios": [
            "real project.assets.json numeric RC + six-RID admitted",
            "semver rc.sha floating selection rejected",
            "missing installed SDK configuration rejected",
            "cross-RID SHA mismatch rejected",
        ],
    }


def self_test(root: Path) -> dict[str, Any]:
    check_policy(root)
    sha = "b31598521b23f7833a999eb05733e846b6174644"
    version = f"2.3.0-rc.sha{sha}"
    with tempfile.TemporaryDirectory(prefix="salts-feed-policy-") as temp:
        p = Path(temp)
        def add_rid(rid: str) -> None:
            d = p / "sdk" / rid
            (d / "lib/cmake/Salts").mkdir(parents=True)
            (d / "lib/cmake/Salts/SaltsConfig.cmake").write_text("versioned\n")
            (d / "salts-sdk-manifest.txt").write_text(
                f"version={version}\ncommit={sha}\nrid={rid}\nprofile=candidate\n")
        add_rid("linux-x64")
        classification = inspect_extracted(version, p)
        ensure(classification["present_rids"] == ["linux-x64"]
               and len(classification["missing_rids"]) == 5,
               "single-RID fixture not recognized")
        for rid in RIDS - {"linux-x64"}:
            add_rid(rid)
        try:
            inspect_extracted(version, p)
        except FeedPolicyError:
            pass
        else:
            raise FeedPolicyError("complete six-RID package incorrectly marked deletable")
        try:
            inspect_extracted("2.3.0-rc.15", p)
        except FeedPolicyError:
            pass
        else:
            raise FeedPolicyError("official numeric RC incorrectly marked deletable")
        listing = p / "listing.json"
        listing.write_text(json.dumps([{"id": 101, "name": version},
                                       {"id": 102, "name": "2.3.0-rc.15"}]))
        report = audit_versions(listing, clean=False)
        ensure(len(report["unsafe_rc_sha"]) == 1,
               "feed-version listing did not isolate sha prerelease")
    return {"status": "PASS", "fixture_rc_sha": version,
            "scenarios": ["single-RID reject", "six-RID protected",
                          "official-numeric-RC protected",
                          "GitHub versions listing", "no candidate feed publishers"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("policy")
    p.add_argument("repo_root", type=Path)
    t = commands.add_parser("self-test")
    t.add_argument("repo_root", type=Path)
    i = commands.add_parser("inspect-dir")
    i.add_argument("version")
    i.add_argument("package_directory", type=Path)
    fr = commands.add_parser("floating-restore")
    fr.add_argument("project_assets_json", type=Path)
    fr.add_argument("nuget_packages_root", type=Path)
    commands.add_parser("floating-self-test")
    v = commands.add_parser("versions")
    v.add_argument("versions_json", type=Path)
    v.add_argument("--expect-clean", action="store_true")
    args = parser.parse_args()
    try:
        if args.command == "policy":
            output = check_policy(args.repo_root)
        elif args.command == "self-test":
            output = self_test(args.repo_root)
        elif args.command == "inspect-dir":
            output = inspect_extracted(args.version, args.package_directory)
        elif args.command == "floating-restore":
            output = audit_floating_resolution(args.project_assets_json, args.nuget_packages_root)
        elif args.command == "floating-self-test":
            output = floating_self_test()
        else:
            output = audit_versions(args.versions_json, args.expect_clean)
    except (FeedPolicyError, OSError, json.JSONDecodeError) as e:
        print(f"RELEASE_FEED_POLICY_FAILED: {e}", file=sys.stderr)
        return 1
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
