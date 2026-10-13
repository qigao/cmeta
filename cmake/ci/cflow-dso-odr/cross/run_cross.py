#!/usr/bin/env python3
"""Source-pinned Windows/macOS Salts::CFlow installed SDK owner conformance.

Two real independently linked consumer images use the installed CMake target
and native OS dynamic loader. Never claim production owner approval from this
candidate-only matrix.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

RELEASE = "21bdb1145fbb2b39ee3fa644173ec037adba7b87"
SYMBOL = "cflow_type_executor_protocol_stats_ptr"


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for data in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(data)
    return digest.hexdigest()


def run(command, *, env=None, logfile=None):
    args = [str(arg) for arg in command]
    print("$ " + subprocess.list2cmdline(args), flush=True)
    p = subprocess.run(args, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True,
                       encoding="utf-8", errors="replace", check=False)
    if logfile is not None:
        with logfile.open("a", encoding="utf-8") as log:
            log.write("$ " + subprocess.list2cmdline(args) + "\n")
            log.write(p.stdout + "\n")
    print(p.stdout[-3500:], flush=True)
    if p.returncode:
        raise RuntimeError(f"command exit {p.returncode}: {args}\n" +
                           p.stdout[-16000:])
    return p.stdout


def source_object(build, source, platform, mode):
    compile_json = build / "compile_commands.json"
    require(compile_json.is_file(), "original source compile database missing")
    rows = json.loads(compile_json.read_text(encoding="utf-8"))
    matches = [x for x in rows if Path(x.get("file", "")).resolve() == source.resolve()]
    require(len(matches) == 1, "expected one original CFlow executor source compile")
    command = matches[0].get("command") or " ".join(matches[0].get("arguments", []))
    ext = "*executor.c.obj" if platform == "windows" else "*executor.c.o"
    objects = [x for x in build.rglob(ext)
               if x.is_file() and "cmeta_cflow.dir" in str(x)]
    require(len(objects) == 1, f"expected one built CFlow executor object: {objects}")
    if platform == "macos" and mode == "static-hidden":
        require("-fvisibility=hidden" in command,
                "macOS static-hidden CFlow source did not use hidden visibility")
    return {"file": str(source), "command": command,
            "real_object": str(objects[0]), "sha256": sha(objects[0])}


def audit_images(platform, mode, prefix, consumer, evidence):
    images = {}
    for role in ("client", "server"):
        name = ("cflow_cross_" + role + ".dll" if platform == "windows"
                else "libcflow_cross_" + role + ".dylib")
        path = consumer / name
        require(path.is_file(), f"real DSO missing: {path}")
        if platform == "windows":
            exported = run(["dumpbin", "/exports", path],
                           logfile=evidence / "image-symbols.log")
            dependencies = run(["dumpbin", "/dependents", path],
                               logfile=evidence / "image-symbols.log")
        else:
            exported = run(["nm", "-gU", path],
                           logfile=evidence / "image-symbols.log")
            dependencies = run(["otool", "-L", path],
                               logfile=evidence / "image-symbols.log")
        require(all(x in exported for x in (
            "salts_cflow_probe_read", "salts_cflow_probe_borrowed",
            "salts_cflow_probe_semantic_equal")),
            f"{role} lacks explicit exported native Provider APIs")
        require(SYMBOL not in exported,
                f"{role} exports forbidden duplicated CFlow metadata global")
        needed = "cmeta_cflow" in dependencies.lower()
        require(needed == (mode == "shared-single"),
                f"{role} installed target does not link selected owner topology")
        images[role] = {"path": str(path), "sha256": sha(path),
                        "needs_shared_cflow": needed,
                        "descriptor_exported": False,
                        "dependencies": dependencies.splitlines()[:55]}
    if mode == "shared-single":
        candidates = [x for x in prefix.rglob("*.dll" if platform == "windows"
                                               else "*.dylib")
                      if x.is_file() and "cmeta_cflow" in x.name.lower()]
        require(candidates, "shared-single CFlow SDK DLL/dylib absent")
        real = {str(x.resolve()) for x in candidates}
        require(len(real) == 1,
                f"multiple real CFlow runtime binary owners: {real}")
        image = Path(next(iter(real)))
        images["owner"] = {"path": str(image), "sha256": sha(image)}
        if platform == "windows":
            exports = run(["dumpbin", "/exports", image],
                          logfile=evidence / "image-symbols.log")
            require(SYMBOL in exports,
                    "shared Windows CFlow DLL doesn't export public descriptor data")
    return images


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", required=True, choices=("windows", "macos"))
    parser.add_argument("--mode", required=True, choices=("static-hidden", "shared-single"))
    args = parser.parse_args()
    platform, mode = args.platform, args.mode
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    source = workspace / "salts-source"
    root = workspace / "cmake/ci/cflow-dso-odr/cross"
    temp = Path(os.environ["RUNNER_TEMP"]).resolve()
    build = temp / f"salts-cflow-{platform}-{mode}-build"
    prefix = temp / f"salts-cflow-{platform}-{mode}-sdk"
    consumer = temp / f"salts-cflow-{platform}-{mode}-consumer"
    evidence = temp / f"salts-cflow-cross-evidence-{platform}-{mode}"
    evidence.mkdir(parents=True, exist_ok=True)
    log = evidence / "full-commands.log"

    actual = run(["git", "-C", source, "rev-parse", "HEAD"],
                 logfile=log).strip()
    require(actual == RELEASE, f"Salts immutable source mismatch: {actual}")
    require((source / "CMakeUserPresets.json").is_file(), "source CMake presets missing")
    tc = Path(os.environ["QIGAO_VCPKG_TOOLCHAIN_FILE"]).resolve()
    require(tc.is_file(), "pinned vcpkg-cache toolchain missing")
    triplet = ("arm64-osx" if platform == "macos"
               else os.environ.get("QIGAO_VCPKG_WINDOWS_TRIPLET", ""))
    require(bool(triplet), "Windows vcpkg target triplet is unknown")
    installed = source / "vcpkg_installed"
    env = dict(os.environ)
    env["PROJECT_ROOT"] = str(source)
    if platform == "windows":
        env["VCPKG_WINDOWS_TRIPLET"] = triplet
    require(bool(env.get("VCPKG_CACHE_REPOSITORY_ROOT")),
            "pinned vcpkg-cache contract not installed")

    preset = "mac-arm64-release-ci" if platform == "macos" else "win-release-ci"
    cmake = [
        "cmake", "--preset", preset, "-S", source, "-B", build,
        "-DCMAKE_INSTALL_PREFIX=" + str(prefix),
        "-DCMAKE_TOOLCHAIN_FILE=" + str(tc),
        "-DVCPKG_INSTALLED_DIR=" + str(installed),
        "-DVCPKG_TARGET_TRIPLET=" + triplet,
        "-DVCPKG_HOST_TRIPLET=" + (
            os.environ.get("QIGAO_VCPKG_WINDOWS_HOST_TRIPLET", triplet)
            if platform == "windows" else triplet),
        "-DCMAKE_PREFIX_PATH=" + str(installed / triplet),
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DBUILD_SHARED_LIBS=" + ("ON" if mode == "shared-single" else "OFF"),
        "-DBUILD_TESTS=OFF", "-DENABLE_TESTS=OFF",
        "-DBUILD_EXAMPLES=OFF", "-DBUILD_BENCHMARKS=OFF",
        "-DCMETA_BUILD_BENCHMARKS=OFF",
        "-DNATIVE_IO_BUILD_BENCHMARKS=OFF", "-DCFLOW_BUILD_BENCHMARKS=OFF",
        "-DCNET_BUILD_BENCHMARKS=OFF", "-DCONCURRENCY_BUILD_BENCHMARKS=OFF",
        "-DENABLE_SANITIZER_ADDRESS=OFF", "-DENABLE_SANITIZER_THREAD=OFF",
        "-DENABLE_SANITIZER_UNDEFINED=OFF",
        "-DCMAKE_SUPPRESS_REGENERATION=ON",
    ]
    if mode == "static-hidden":
        cmake.append("-DCMAKE_PROJECT_Salts_INCLUDE=" +
                     str(root / "static_hidden.cmake"))
    if platform == "windows":
        cmake.append("-DCMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=" +
                     ("ON" if mode == "shared-single" else "OFF"))
    run(cmake, env=env, logfile=log)
    run(["cmake", "--build", build, "--parallel", "2"], env=env, logfile=log)
    run(["cmake", "--install", build, "--prefix", prefix],
        env=env, logfile=log)
    require((prefix / "lib/cmake/Salts/SaltsConfig.cmake").is_file(),
            "installed native SDK CMake config missing")
    source_report = source_object(
        build, source / "cflow/src/executor.c", platform, mode)

    cfg = [
        "cmake", "-S", root, "-B", consumer, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_TOOLCHAIN_FILE=" + str(tc),
        "-DVCPKG_MANIFEST_MODE=OFF",
        "-DVCPKG_TARGET_TRIPLET=" + triplet,
        "-DVCPKG_HOST_TRIPLET=" + triplet,
        "-DVCPKG_INSTALLED_DIR=" + str(installed),
        "-DCMAKE_PREFIX_PATH=" + str(installed / triplet),
        "-DEXACT_SALTS_PREFIX=" + str(prefix),
        "-DCROSS_CFLOW_EXPECTED_IDENTITY=" +
        ("isolated" if mode == "static-hidden" else "shared"),
    ]
    run(cfg, env=env, logfile=log)
    run(["cmake", "--build", consumer, "--parallel", "2"], env=env, logfile=log)
    cache = (consumer / "CMakeCache.txt").read_text(encoding="utf-8")
    found = [x.partition("=")[2] for x in cache.splitlines()
             if x.startswith("Salts_DIR:")]
    require(len(found) == 1 and
            Path(found[0]).resolve() == (prefix / "lib/cmake/Salts").resolve(),
            f"consumer imported incorrect SDK target prefix: {found}")
    images = audit_images(platform, mode, prefix, consumer, evidence)

    runtime = dict(env)
    runtime["PATH"] = os.pathsep.join([
        str(prefix / "bin"), str(prefix / "lib"),
        str(installed / triplet / "bin"), runtime.get("PATH", ""),
    ])
    if platform == "macos":
        runtime["DYLD_LIBRARY_PATH"] = os.pathsep.join([
            str(prefix / "lib"), str(installed / triplet / "lib"),
            runtime.get("DYLD_LIBRARY_PATH", ""),
        ])
    tests = run([
        "ctest", "--test-dir", consumer, "-R", "^cflow_cross_",
        "--repeat", "until-fail:3", "--no-tests=error",
        "--output-on-failure", "-V", "--timeout", "120"],
        env=runtime, logfile=log)
    # CTest differs by platform/version: macOS/MSVC can print
    # "100% tests passed out of 2" without the redundant "0 tests failed".
    # Still require both named cases and SIX actual native process markers.
    require(re.search(r"100% tests passed(?:, 0 tests failed)? out of 2", tests)
            is not None and
            "cflow_cross_client_first" in tests and
            "cflow_cross_server_first" in tests,
            "two named native CFlow DSO loader tests did not both pass")
    for text in ("CFLOW_CROSS_TYPE_SHAPE_EQUAL=1",
                 "CFLOW_CROSS_PUBLIC_SEMANTIC_EQUAL_BOTH=1",
                 "CFLOW_CROSS_LIVE_FIRST_OWNER_OK",
                 "CFLOW_CROSS_TWO_DSO_LOAD_UNLOAD_OK"):
        require(tests.count(text) == 6,
                f"2 loader orders x 3 process executions did not complete: {text}")

    report = {
        "status": "NATIVE_INSTALLED_CFLOW_OWNER_ACCEPTED",
        "platform": platform, "mode": mode,
        "original_salts_release_sha": actual,
        "used_relocated_installed_SDK": True,
        "source_executor": source_report,
        "real_images": images,
        "load_orders": ["client-first", "server-first"],
        "independent_native_test_processes": 6,
        "production_SDK_policy_approved": False,
        "release_or_tag_modified": False,
        "sanitizer_coverage": "NOT_CLAIMED_WIN_MAC_NATIVE",
        "vcpkg_triplet": triplet,
        "vcpkg_toolchain": str(tc),
    }
    (evidence / "owner-result.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    shutil.copy2(build / "compile_commands.json",
                 evidence / "salts-compile_commands.json")
    shutil.copy2(consumer / "compile_commands.json",
                 evidence / "consumer-compile_commands.json")
    print("CFLOW_CROSS_PLATFORM_RESULT=" + report["status"], flush=True)
    print("CFLOW_CROSS_PLATFORM_EVIDENCE=" + str(evidence), flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, KeyError,
            subprocess.CalledProcessError) as exc:
        print("CFlow cross-platform native SDK gate FAILED: " + str(exc),
              file=sys.stderr)
        raise SystemExit(1)
