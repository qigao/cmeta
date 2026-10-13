#!/usr/bin/env python3
"""Compile exact pinned Salts rc.15; relocate installed SDK; run real native DSOs.

Test-only. No production ABI/topology changes, no source-tree install fallback,
no automatic retry, and no bogus cross-OS runtime success claim.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys

SOURCE_SHA = "21bdb1145fbb2b39ee3fa644173ec037adba7b87"


def execute(argv: list[str], env: dict[str, str] | None = None) -> str:
    print("RUN " + " ".join(shlex.quote(item) for item in argv), flush=True)
    proc = subprocess.run(argv, env=env, text=True,
                          encoding="utf-8", errors="replace",
                          stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, check=False)
    print(proc.stdout[-4500:], flush=True)
    if proc.returncode != 0:
        raise RuntimeError(
            f"command failed ({proc.returncode}): {argv}\n{proc.stdout[-12000:]}"
        )
    return proc.stdout


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def strict_one(directory: Path, pattern: str) -> Path:
    matches = sorted(p for p in directory.glob(pattern) if p.is_file())
    if len(matches) != 1:
        raise RuntimeError(
            f"expected one exact {pattern} within {directory}, got {matches}"
        )
    return matches[0]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("hidden", "shared"), required=True)
    args = parser.parse_args()
    system = platform.system()
    if system not in ("Darwin", "Windows"):
        raise RuntimeError(f"wrong runtime host {system}")
    if platform.machine().lower() not in (
        ("arm64", "aarch64") if system == "Darwin" else ("amd64", "x86_64")
    ):
        raise RuntimeError("wrong target architecture for this native gate")

    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    temporary = Path(os.environ["RUNNER_TEMP"]).resolve()
    source = workspace / "salts-source"
    consumer_source = workspace / "cmake/ci/cstl-dso-odr/native-consumer"
    mode = args.mode
    rid = "macos-arm64" if system == "Darwin" else "windows-x64"
    triplet = "arm64-osx" if system == "Darwin" else os.environ[
        "QIGAO_VCPKG_WINDOWS_TRIPLET"
    ]
    host_triplet = triplet if system == "Darwin" else os.environ[
        "QIGAO_VCPKG_WINDOWS_HOST_TRIPLET"
    ]
    label = f"{rid}-{mode}"
    build = temporary / f"cstl-source-{label}"
    original = temporary / f"cstl-install-{label}" / "sdk" / rid
    relocated = temporary / f"cstl-relocated-{label}" / "sdk" / rid
    consumer_build = temporary / f"cstl-consumer-{label}"
    evidence = temporary / f"cstl-native-evidence-{label}"
    evidence.mkdir(parents=True, exist_ok=True)
    toolchain = Path(os.environ["QIGAO_VCPKG_TOOLCHAIN_FILE"]).resolve()
    installed_vcpkg = workspace / "vcpkg_installed" / f"cstl-native-{label}"
    overlay = workspace / "cmake/ci/cstl-dso-odr/hidden_static.cmake"
    sha = execute(["git", "-C", str(source), "rev-parse", "HEAD"]).strip()
    if sha != SOURCE_SHA:
        raise RuntimeError(f"wrong Salts source {sha}")
    if not toolchain.is_file() or not overlay.is_file():
        raise RuntimeError("missing exact vcpkg toolchain or hidden-static overlay")
    for path in (build, original, relocated, consumer_build):
        if path.exists():
            raise RuntimeError(f"expected new isolated build/install destination {path}")

    configure = [
        "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DBUILD_TESTS=OFF", "-DENABLE_TESTS=OFF",
        "-DBUILD_EXAMPLES=OFF", "-DBUILD_BENCHMARKS=OFF",
        "-DCMETA_BUILD_BENCHMARKS=OFF",
        "-DCMETA_BUILD_NATIVE_THUNKS=OFF",
        "-DNATIVE_IO_BUILD_BENCHMARKS=OFF",
        "-DCOROUTINE_BUILD_BENCHMARKS=OFF",
        "-DCNET_BUILD_BENCHMARKS=OFF",
        "-DCFLOW_BUILD_BENCHMARKS=OFF",
        "-DCONCURRENCY_BUILD_BENCHMARKS=OFF",
        "-DCMAKE_SUPPRESS_REGENERATION=ON",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DBUILD_SHARED_LIBS=" + ("ON" if mode == "shared" else "OFF"),
        # Test-only Windows candidate: export the actual compiled native C
        # data/functions from shared imported SDK DLLs. Preserve released ABI.
        "-DCMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=" +
        ("ON" if system == "Windows" and mode == "shared" else "OFF"),
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DCMAKE_INSTALL_PREFIX={original}",
        f"-DVCPKG_TARGET_TRIPLET={triplet}",
        f"-DVCPKG_HOST_TRIPLET={host_triplet}",
        f"-DVCPKG_INSTALLED_DIR={installed_vcpkg}",
    ]
    if system == "Darwin":
        configure.append("-DCMAKE_OSX_ARCHITECTURES=arm64")
    elif not shutil.which("cl.exe"):
        raise RuntimeError("MSVC Developer Cmd environment not loaded")
    if mode == "hidden" and system == "Darwin":
        configure.append(f"-DCMAKE_PROJECT_Salts_INCLUDE={overlay}")
    execute(configure)
    execute(["cmake", "--build", str(build), "--parallel", "2"])
    execute(["cmake", "--install", str(build), "--prefix", str(original)])
    if not (original / "lib/cmake/Salts/SaltsConfig.cmake").is_file():
        raise RuntimeError("CMake-installed source SDK missing SaltsConfig")
    orig_header = source / "cstl/include/cstl/detail/instance_meta.h"
    installed_header = original / "include/cstl/detail/instance_meta.h"
    if not installed_header.is_file() or digest(orig_header) != digest(installed_header):
        raise RuntimeError("installed public CSTL descriptor ABI header differs from pinned source")

    relocated.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(original), str(relocated))
    if original.exists() or not (relocated / "lib/cmake/Salts/SaltsConfig.cmake").is_file():
        raise RuntimeError("relocation must remove original SDK location")

    if system == "Darwin":
        core = strict_one(relocated / "lib", "libsalts.2.3.dylib")
    else:
        core = strict_one(relocated / "bin", "salts.dll")
    config = [
        "cmake", "-S", str(consumer_source), "-B", str(consumer_build), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DVCPKG_TARGET_TRIPLET={triplet}",
        f"-DVCPKG_HOST_TRIPLET={host_triplet}",
        f"-DVCPKG_INSTALLED_DIR={installed_vcpkg}",
        f"-DCMAKE_PREFIX_PATH={installed_vcpkg / triplet}",
        f"-DEXACT_SALTS_PREFIX={relocated}",
        f"-DEXACT_CORE_LIBRARY={core}",
        f"-DEXACT_CSTL_MODE={mode}",
    ]
    if system == "Darwin":
        config.append("-DCMAKE_OSX_ARCHITECTURES=arm64")
    execute(config)
    execute(["cmake", "--build", str(consumer_build), "--parallel", "2"])

    provider_extension = ".dll" if system == "Windows" else ".dylib"
    provider_a = strict_one(consumer_build, "*cstl_provider_a*" + provider_extension)
    provider_b = strict_one(consumer_build, "*cstl_provider_b*" + provider_extension)
    cpp17 = consumer_build / (
        "cstl_cpp17_consumer.exe" if system == "Windows" else "cstl_cpp17_consumer"
    )
    if not cpp17.is_file():
        raise RuntimeError("independent C++17 public consumer did not link")
    native = consumer_build / (
        "cstl_native_loader.exe" if system == "Windows" else "cstl_native_loader"
    )
    if not native.is_file():
        raise RuntimeError("independent C11 native loader did not link")

    report_images = {}
    for name, image in (("core", core), ("a", provider_a),
                        ("b", provider_b)):
        if system == "Darwin":
            dependencies = execute(["otool", "-L", str(image)])
            exports = execute(["nm", "-gU", str(image)])
            if mode == "shared":
                if "libcmeta_cstl" not in dependencies:
                    raise RuntimeError(f"shared CSTL missing from {name} dylib install linkage")
                if name in ("a", "b") and "_stl_bplus_tree_generic_desc" in exports:
                    raise RuntimeError("Provider exported a duplicate shared descriptor")
            elif "_stl_bplus_tree_generic_desc" in exports:
                raise RuntimeError(f"hidden-static {name} exports a private descriptor")
        else:
            dependencies = execute(["dumpbin", "/DEPENDENTS", str(image)])
            exports = execute(["dumpbin", "/EXPORTS", str(image)])
            if name in ("a", "b") and "salts_test_cstl_bplus_generic" not in exports:
                raise RuntimeError(f"Provider DLL {name} has no public descriptor-query export")
        report_images[name] = {
            "path": str(image), "sha256": digest(image),
            "dependency_audit": dependencies, "exports_audit": exports,
        }

    # Remove ambient SDK library search paths; use only the isolated installed
    # prefix, independent vcpkg closure and OS runtime directories.
    env = dict(os.environ)
    if system == "Darwin":
        env["DYLD_LIBRARY_PATH"] = os.pathsep.join([
            str(relocated / "lib"), str(installed_vcpkg / triplet / "lib"),
            str(consumer_build)
        ])
    else:
        systemroot = Path(os.environ["SystemRoot"])
        paths = [
            relocated / "bin", installed_vcpkg / triplet / "bin",
            consumer_build, systemroot / "System32", systemroot
        ]
        env["PATH"] = os.pathsep.join(str(x) for x in paths)
    ctest = shutil.which("ctest")
    if not ctest:
        raise RuntimeError("CTest executable missing")
    output = execute([
        ctest, "--test-dir", str(consumer_build),
        "--no-tests=error", "--output-on-failure",
    ], env=env)
    if "100% tests passed" not in output:
        raise RuntimeError("C11 and C++17 installed SDK runtime tests not evidenced")
    result = execute([
        str(native), str(core), str(provider_a), str(provider_b), mode
    ], env=env)
    for token in ("CSTL_GENERIC_SEMANTIC_EQUAL=1",
                  "CSTL_GENERIC_POINTER_EQUAL=" + ("1" if mode == "shared" else "0"),
                  "CSTL_LIVE_PROVIDER_SURVIVED_OTHER_UNLOAD",
                  "CSTL_CONTAINER_RANGE_CROSS_DSO=1",
                  "CSTL_CONTAINER_RANGE_AFTER_OTHER_UNLOAD=1",
                  "CSTL_NATIVE_DSO_LOAD_UNLOAD_OK"):
        if token not in result:
            raise RuntimeError(f"native descriptor/owner assertion missing: {token}")

    report = {
        "rid": rid, "mode": mode, "source_commit": sha,
        "source_head": os.environ.get("SOURCE_HEAD_SHA", "unknown"),
        "tested_checkout": os.environ.get("GITHUB_SHA", "local"),
        "run_id": os.environ.get("GITHUB_RUN_ID", "local"),
        "installed_sdk_relocated": True,
        "public_header_sha256": digest(orig_header),
        "consumer_c11_and_cpp17_executed": True,
        "native_dso_unload_executed": True,
        "images": report_images,
        "vcpkg_triplet": triplet,
        "vcpkg_revision": os.environ.get("VCPKG_CACHE_REVISION", "unknown"),
    }
    (evidence / "native-result.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    (evidence / "ctest.log").write_text(output, encoding="utf-8")
    (evidence / "native-runtime.log").write_text(result, encoding="utf-8")
    print(f"CSTL_NATIVE_RESULT={rid}/{mode}:PASS", flush=True)
    print(f"CSTL_NATIVE_EVIDENCE={evidence}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError,
            subprocess.CalledProcessError) as exc:
        print(f"Native installed CSTL owner conformance FAILED: {exc}",
              file=sys.stderr)
        raise SystemExit(1)
