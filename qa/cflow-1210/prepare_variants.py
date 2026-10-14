#!/usr/bin/env python3
"""Prepare two isolated C11 QA builds with the same demand-admission gate.

These disposable Git worktrees start from exact, independently attested old
and fixed production source commits. Changes made here are QA-only and never
part of the release SDK, PR #1217, or the master branch.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

OLD_SHA = "6cf093dc053db98bb21b1788eb2fc1ada4df195e"
FIXED_SHA = "a18c94a30700fa3dd157cc67e8895ea2725a89a3"
OLD_IMPL_BLOB = "d80067a52a306e6b999824fc818d1d4962d11bce"
FIXED_IMPL_BLOB = "c075554cc1d75bf2663063752ca5e6733c0a87bd"
OLD_HEADER_BLOB = "0746fffc0132c214f107b561d8a2a125c1dace5a"
FIXED_HEADER_BLOB = "28991868abfe5b591ec2f8a10746a9e7cef4e3e3"

HOOK_DECL = """\
/* QA INSTRUMENTATION ONLY. Never included in production CFlow artifacts. */
typedef void (*cflow_qa_request_hook_fn)(void);
static cflow_qa_request_hook_fn cflow_qa_request_hook;
void cflow_qa_set_request_hook(cflow_qa_request_hook_fn callback) {
    cflow_qa_request_hook = callback;
}

"""
TEST_REGISTRATION = """\

# PRIVATE QA ONLY. Source and synchronization gate exist only in worktrees
# materialized by qa/cflow-1210/prepare_variants.py.
cmake_add_test(cflow_qa_request_interleave_test
  SOURCES cflow_qa_request_interleave_test.c
  LIBS Salts::CFlow Salts::Platform
  FOLDER "cflow/qa")
"""


def git(source: Path, *args: str) -> str:
    command = ["git", "-C", str(source), *args]
    process = subprocess.run(
        command, text=True, capture_output=True, check=True
    )
    return process.stdout.strip()


def install_variant(name: str, source: Path, fixture: Path) -> dict[str, object]:
    expected_sha = OLD_SHA if name == "baseline" else FIXED_SHA
    expected_blob = OLD_IMPL_BLOB if name == "baseline" else FIXED_IMPL_BLOB
    expected_header = OLD_HEADER_BLOB if name == "baseline" else FIXED_HEADER_BLOB

    if git(source, "rev-parse", "HEAD") != expected_sha:
        raise RuntimeError(f"{name}: wrong exact source commit")
    if git(source, "hash-object", "cflow/src/subscription.c") != expected_blob:
        raise RuntimeError(f"{name}: source production blob drift")
    if git(source, "hash-object", "cflow/include/cflow/reactive.h") != expected_header:
        raise RuntimeError(f"{name}: API header blob drift")

    impl_path = source / "cflow/src/subscription.c"
    before = impl_path.read_text(encoding="utf-8")
    marker = "cflow_status_result cflow_subscription_request_result(\n"
    if before.count(marker) != 1:
        raise RuntimeError(f"{name}: request function marker not unique")

    entry = "    cmeta_mutex_unlock(&r->lock);\n"
    request_tail = (
        "    return (cflow_status_result){schedule_pump(r, false)};\n"
        if name == "baseline"
        else "    return (cflow_status_result){schedule_pump(r, false, true)};\n"
    )
    anchor = entry + request_tail
    request_body = before[before.index(marker) :]
    if request_body.count(anchor) != 1:
        raise RuntimeError(f"{name}: post-admission source anchor changed")
    after = before.replace(
        marker, HOOK_DECL + marker, 1
    ).replace(
        anchor,
        entry + "    if (cflow_qa_request_hook) cflow_qa_request_hook();\n"
        + request_tail,
        1,
    )
    if after.count("    if (cflow_qa_request_hook) cflow_qa_request_hook();") != 1:
        raise RuntimeError(f"{name}: duplicate QA gate")
    impl_path.write_text(after, encoding="utf-8")

    cmake_path = source / "cflow/tests/CMakeLists.txt"
    cmake_before = cmake_path.read_text(encoding="utf-8")
    if "cflow_qa_request_interleave_test" in cmake_before:
        raise RuntimeError(f"{name}: QA registration was already present")
    cmake_path.write_text(
        cmake_before.rstrip("\n") + "\n" + TEST_REGISTRATION, encoding="utf-8"
    )

    test_file = source / "cflow/tests/cflow_qa_request_interleave_test.c"
    if test_file.exists():
        raise RuntimeError(f"{name}: QA test already exists")
    shutil.copyfile(fixture, test_file)

    diff_files = git(source, "diff", "--name-only", "--no-renames").splitlines()
    expected_changed = ["cflow/src/subscription.c", "cflow/tests/CMakeLists.txt"]
    if sorted(diff_files) != sorted(expected_changed):
        raise RuntimeError(f"{name}: unexpectedly modified source files: {diff_files}")
    git(source, "diff", "--check")

    return {
        "variant": name,
        "source_commit": expected_sha,
        "pristine_subscription_blob": expected_blob,
        "pristine_reactive_header_blob": expected_header,
        "qa_gate_only_in_disposable_source": True,
        "qa_c11_test_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
        "tracked_files_modified": diff_files,
        "untracked_qa_test": "cflow/tests/cflow_qa_request_interleave_test.c",
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--fixed", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()

    fixture = args.fixture.resolve()
    if not fixture.is_file():
        raise RuntimeError("Private QA C11 fixture missing")
    records = [
        install_variant("baseline", args.baseline.resolve(), fixture),
        install_variant("fixed", args.fixed.resolve(), fixture),
    ]
    if records[0]["qa_c11_test_sha256"] != records[1]["qa_c11_test_sha256"]:
        raise RuntimeError("Red and green built different QA tests")
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(
        json.dumps(records, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(records, indent=2))


if __name__ == "__main__":
    main()
