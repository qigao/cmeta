#!/usr/bin/env python3
"""Bootstrap the pinned shared vcpkg contract without NuGet or Mono."""
from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


def run(*args: str, cwd: Path | None = None) -> str:
    return subprocess.check_output(args, text=True, cwd=cwd).strip()


def append_env(values: dict[str, str]) -> None:
    with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as output:
        for key, value in values.items():
            if "\n" in value or "\r" in value:
                raise ValueError(f"newline in environment value: {key}")
            output.write(f"{key}={value}\n")


def append_outputs(values: dict[str, str]) -> None:
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        for key, value in values.items():
            if "\n" in value or "\r" in value:
                raise ValueError(f"newline in output value: {key}")
            output.write(f"{key}={value}\n")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: setup-vcpkg-curl-cache.py <shared-cache-checkout>")

    infrastructure = Path(sys.argv[1]).resolve()
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    temporary = Path(os.environ["RUNNER_TEMP"]).resolve()
    if not (infrastructure / ".github/actions/setup-vcpkg-cache/action.yml").is_file():
        raise ValueError("shared vcpkg-cache checkout is incomplete")

    scripts = (infrastructure / "vcpkg-scripts-revision.txt").read_text().strip()
    expected_tool_version = (infrastructure / "vcpkg-tool-version.txt").read_text().strip()
    contract = (infrastructure / "cache-contract-version.txt").read_text().strip()
    revision = run(
        sys.executable,
        str(infrastructure / "scripts/cache-contract-revision.py"),
        str(infrastructure),
    )
    if (not re.fullmatch(r"[0-9a-f]{40}", scripts)
            or not re.fullmatch(r"[0-9a-f]{64}", revision)
            or not contract.isdigit()
            or not re.fullmatch(r"[0-9A-Za-z][0-9A-Za-z.-]*", expected_tool_version)):
        raise ValueError("invalid canonical vcpkg cache identity")

    root = temporary / f"qigao-vcpkg-{scripts}"
    if root.exists():
        raise ValueError(f"expected a fresh vcpkg directory: {root}")
    root.mkdir()
    run("git", "-C", str(root), "init")
    run("git", "-C", str(root), "remote", "add", "origin",
        "https://github.com/microsoft/vcpkg.git")

    fetched = False
    for attempt in range(1, 4):
        result = subprocess.run(
            ["git", "-c", "protocol.version=2", "-C", str(root),
             "fetch", "--no-tags", "origin", scripts],
            check=False,
        )
        if result.returncode == 0:
            checkout = subprocess.run(
                ["git", "-C", str(root), "checkout", "--detach", "FETCH_HEAD"],
                check=False,
            )
            if checkout.returncode == 0:
                fetched = True
                break
        if attempt < 3:
            time.sleep(attempt * 2)
    if not fetched:
        raise RuntimeError("failed to fetch the exact vcpkg scripts revision after 3 attempts")

    baselines = run(
        sys.executable,
        str(infrastructure / "scripts/vcpkg-required-commits.py"),
        str(infrastructure),
        str(workspace),
    )
    for baseline in baselines.splitlines():
        if not re.fullmatch(r"[0-9a-f]{40}", baseline):
            raise ValueError("invalid vcpkg baseline")
        exists = subprocess.run(
            ["git", "-C", str(root), "cat-file", "-e", baseline + "^{commit}"],
            capture_output=True,
        )
        if exists.returncode:
            fetched_baseline = False
            for attempt in range(1, 4):
                result = subprocess.run(
                    ["git", "-c", "protocol.version=2", "-C", str(root),
                     "fetch", "--no-tags", "origin", baseline],
                    check=False,
                )
                if result.returncode == 0:
                    fetched_baseline = True
                    break
                if attempt < 3:
                    time.sleep(attempt * 2)
            if not fetched_baseline:
                raise RuntimeError(f"failed to fetch required vcpkg baseline {baseline}")
        run("git", "-C", str(root), "cat-file", "-e",
            baseline + ":versions/baseline.json")

    if (run("git", "-C", str(root), "rev-parse", "HEAD") != scripts
            or run("git", "-C", str(root), "rev-parse", "--is-shallow-repository") != "false"):
        raise ValueError("canonical vcpkg scripts/history mismatch")
    promisor = subprocess.run(
        ["git", "-C", str(root), "config", "--get", "remote.origin.promisor"],
        text=True, capture_output=True, check=False,
    )
    partial_filter = subprocess.run(
        ["git", "-C", str(root), "config", "--get", "remote.origin.partialclonefilter"],
        text=True, capture_output=True, check=False,
    )
    if promisor.returncode == 0 and promisor.stdout.strip() == "true":
        raise ValueError("canonical vcpkg checkout must not use a promisor remote")
    if partial_filter.returncode == 0 and partial_filter.stdout.strip():
        raise ValueError("canonical vcpkg checkout must not use a partial-clone filter")

    if os.name == "nt":
        bootstrap = ["cmd", "/c", str(root / "bootstrap-vcpkg.bat"), "-disableMetrics"]
    else:
        if os.environ.get("RUNNER_OS") == "Linux" and os.environ.get("RUNNER_ARCH") == "ARM64":
            # Preserve the shared cache producer's ARM64 tool-selection contract.
            # The binary cache itself is files-only, so enabling downloaded host
            # tools cannot reintroduce NuGet/Mono as a package transport.
            os.environ["VCPKG_FORCE_SYSTEM_BINARIES"] = "1"
            os.environ["VCPKG_FORCE_DOWNLOADED_BINARIES"] = "1"
            append_env({
                "VCPKG_FORCE_SYSTEM_BINARIES": "1",
                "VCPKG_FORCE_DOWNLOADED_BINARIES": "1",
            })
        bootstrap = ["sh", str(root / "bootstrap-vcpkg.sh"), "-disableMetrics"]
    subprocess.run(bootstrap, cwd=root, check=True)

    exe = root / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
    version = run(str(exe), "version").splitlines()[0]
    version = version.removeprefix("vcpkg package management program version ")
    if version != expected_tool_version:
        raise ValueError(f"canonical vcpkg tool mismatch: {version} != {expected_tool_version}")

    assets = temporary / f"qigao-vcpkg-contract-v{contract}-{revision}"
    if assets.exists():
        shutil.rmtree(assets)
    assets.mkdir()
    for directory in ("ports", "triplets"):
        shutil.copytree(infrastructure / directory, assets / directory)
    (assets / "cmake").mkdir()
    shutil.copy2(
        infrastructure / "cmake/QigaoVcpkgToolchain.cmake",
        assets / "cmake/QigaoVcpkgToolchain.cmake",
    )

    cache = workspace / "build/vcpkg-binary-cache"
    cache.mkdir(parents=True, exist_ok=True)
    values = {
        "VCPKG_ROOT": root.as_posix(),
        "VCPKG_TOOL_REVISION": expected_tool_version,
        "VCPKG_SCRIPTS_REVISION": scripts,
        "VCPKG_CACHE_CONTRACT_VERSION": contract,
        "VCPKG_CACHE_REVISION": revision,
        "VCPKG_CACHE_REPOSITORY_ROOT": assets.as_posix(),
        "VCPKG_OVERLAY_PORTS": (assets / "ports").as_posix(),
        "VCPKG_OVERLAY_TRIPLETS": (assets / "triplets").as_posix(),
        "QIGAO_VCPKG_TOOLCHAIN_FILE": (assets / "cmake/QigaoVcpkgToolchain.cmake").as_posix(),
        "QIGAO_VCPKG_WINDOWS_TRIPLET": "qigao-x64-windows-v145",
        "QIGAO_VCPKG_WINDOWS_HOST_TRIPLET": "qigao-x64-windows-v145",
        "VCPKG_BINARY_CACHE": cache.as_posix(),
        "VCPKG_BINARY_SOURCES": f"clear;files,{cache.as_posix()},readwrite",
        "VCPKG_NUGET_REPOSITORY": "https://github.com/qigao/vcpkg-cache",
    }
    append_env(values)
    append_outputs({
        "contract_version": contract,
        "cache_revision": revision,
        "tool_revision": expected_tool_version,
        "scripts_revision": scripts,
        "toolchain_file": values["QIGAO_VCPKG_TOOLCHAIN_FILE"],
    })
    print(
        f"Configured curl-only binary cache: contract=v{contract} revision={revision} "
        f"vcpkg={expected_tool_version} scripts={scripts}"
    )
    print("VCPKG_BINARY_SOURCES uses only the local read/write files provider; no NuGet executable or Mono.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
