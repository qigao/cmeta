#!/usr/bin/env python3
"""Test three *exact pinned source* CSTL metadata ownership topologies.

No production CMake change, no ABI fallback, no disabled ASan ODR checks.
A structurally expected ASan ODR diagnostic is a valid *negative result*,
not evidence that the rejected static topology is safe for production.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

SALTS_SHA = "21bdb1145fbb2b39ee3fa644173ec037adba7b87"
MODES = ("static-default", "static-hidden", "shared-single")
SYMBOL = "stl_bplus_tree_container_desc"
EXPECTED_ODR = "ERROR: AddressSanitizer: odr-violation"


def run(args: list[str], *, env: dict[str, str] | None = None,
        allow_failure: bool = False) -> subprocess.CompletedProcess[str]:
    print("$ " + " ".join(shlex.quote(s) for s in args), flush=True)
    proc = subprocess.run(args, env=env, text=True, encoding="utf-8",
                          errors="replace", stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, check=False)
    if proc.returncode and not allow_failure:
        raise RuntimeError(
            f"command failed (exit {proc.returncode}): {args}\n"
            + proc.stdout[-22000:]
        )
    print(proc.stdout[-3000:], flush=True)
    return proc


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def recorded_symbols(path: Path) -> dict[str, object]:
    if not path.is_file():
        raise RuntimeError(f"missing actual linked ELF image {path}")
    static = run(["nm", "--defined-only", str(path)]).stdout
    dynamic = run(["nm", "-D", "--defined-only", str(path)]).stdout
    # GCC ASan runtime entrypoints are *undefined imports* from libasan.so,
    # not symbols defined inside each instrumented DSO. Inspect all ELF
    # symbol entries and DT_NEEDED; --defined-only rejects valid ASan code.
    all_symbols = run(["nm", "-a", str(path)]).stdout
    needed = run(["readelf", "-d", str(path)]).stdout
    return {
        "image": str(path),
        "sha256": file_sha256(path),
        "defined_descriptor": bool(re.search(
            rf"(?m)^.*\b{SYMBOL}$", static
        )),
        "exports_descriptor": bool(re.search(
            rf"(?m)^.*\b{SYMBOL}$", dynamic
        )),
        "needs_shared_cstl": bool(re.search(
            r"\(NEEDED\).*libcmeta_cstl\.so", needed
        )),
        "asan_symbols": (
            "__asan_" in all_symbols and "libasan.so" in needed
        ),
        "needed_entries": [
            line.strip() for line in needed.splitlines() if "(NEEDED)" in line
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=MODES, required=True)
    args = parser.parse_args()
    mode = args.mode
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    source = workspace / "salts-source"
    root = workspace / "cmake/ci/cstl-dso-odr"
    temporary = Path(os.environ["RUNNER_TEMP"]).resolve()
    build = temporary / ("salts-cstl-" + mode + "-build")
    prefix = temporary / ("salts-cstl-" + mode + "-sdk")
    consumer = temporary / ("salts-cstl-" + mode + "-consumer")
    evidence = temporary / ("salts-cstl-evidence-" + mode)
    evidence.mkdir(parents=True, exist_ok=True)
    actual_sha = run(["git", "-C", str(source), "rev-parse", "HEAD"]).stdout.strip()
    if actual_sha != SALTS_SHA:
        raise RuntimeError(f"wrong pinned Salts source {actual_sha}")

    toolchain = Path(os.environ["QIGAO_VCPKG_TOOLCHAIN_FILE"]).resolve()
    if not toolchain.is_file():
        raise RuntimeError(f"missing exact vcpkg-cache toolchain: {toolchain}")
    installed_vcpkg = workspace / "vcpkg_installed" / ("cstl-odr-" + mode)
    overlay = root / "hidden_static.cmake"
    configure = [
        "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        "-DCMAKE_C_COMPILER=/usr/bin/gcc",
        "-DCMAKE_CXX_COMPILER=/usr/bin/g++",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DENABLE_SANITIZER_ADDRESS=ON",
        "-DENABLE_SANITIZER_UNDEFINED=ON",
        "-DENABLE_SANITIZER_THREAD=OFF",
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
        "-DBUILD_SHARED_LIBS=" +
        ("ON" if mode == "shared-single" else "OFF"),
        "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain),
        "-DCMAKE_INSTALL_PREFIX=" + str(prefix),
        "-DVCPKG_TARGET_TRIPLET=x64-linux",
        "-DVCPKG_HOST_TRIPLET=x64-linux",
        "-DVCPKG_INSTALLED_DIR=" + str(installed_vcpkg),
    ]
    if mode == "static-hidden":
        configure.append("-DCMAKE_PROJECT_Salts_INCLUDE=" + str(overlay))
    run(configure)
    run(["cmake", "--build", str(build), "--parallel", "2"])
    run(["cmake", "--install", str(build), "--prefix", str(prefix)])

    commands = json.loads((build / "compile_commands.json").read_text(
        encoding="utf-8"
    ))
    meta_path = (source / "cstl/src/associative_meta.c").resolve()
    meta_objects = []
    for item in commands:
        if Path(item["file"]).resolve() != meta_path:
            continue
        flags = item.get("arguments") or shlex.split(item["command"])
        if not all(flag in flags for flag in (
            "-fsanitize=address", "-fsanitize=undefined",
            "-fno-omit-frame-pointer"
        )):
            raise RuntimeError(f"uninstrumented real CSTL metadata: {flags}")
        if mode == "static-hidden" and "-fvisibility=hidden" not in flags:
            raise RuntimeError("hidden topology never compiled CSTL with hidden symbols")
        if "-o" not in flags:
            raise RuntimeError("CSTL compiler output unknown")
        object_path = (Path(item["directory"]) / flags[flags.index("-o") + 1]).resolve()
        if not object_path.is_file():
            raise RuntimeError(f"CSTL object not built {object_path}")
        meta_objects.append(str(object_path))
    if len(meta_objects) != 1:
        raise RuntimeError(f"wrong CSTL source object count {len(meta_objects)}")

    consumer_args = [
        "cmake", "-S", str(root / "consumer"), "-B", str(consumer), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain),
        "-DVCPKG_MANIFEST_MODE=OFF",
        "-DVCPKG_TARGET_TRIPLET=x64-linux",
        "-DVCPKG_HOST_TRIPLET=x64-linux",
        "-DVCPKG_INSTALLED_DIR=" + str(installed_vcpkg),
        "-DCMAKE_PREFIX_PATH=" + str(installed_vcpkg / "x64-linux"),
        "-DEXACT_SALTS_PREFIX=" + str(prefix),
    ]
    run(consumer_args)
    run(["cmake", "--build", str(consumer), "--parallel", "2"])

    core = prefix / "lib/libsalts.so.2.3"
    provider = consumer / "libcstl_probe_provider.so"
    other_provider = consumer / "libcstl_probe_provider_other.so"
    loader = consumer / "cstl_probe_loader"
    if not loader.is_file():
        raise RuntimeError(f"missing independent host loader {loader}")

    images = {
        "core": recorded_symbols(core),
        "provider": recorded_symbols(provider),
        "provider_other": recorded_symbols(other_provider),
    }
    if not all(data["asan_symbols"] for data in images.values()):
        raise RuntimeError("one of the two real DSOs lacks ASan code")
    if mode != "shared-single":
        if not all(data["defined_descriptor"] for data in images.values()):
            raise RuntimeError("static topology did not actually embed CSTL in both DSOs")
        if mode == "static-default" and not all(
            data["exports_descriptor"] for data in images.values()
        ):
            raise RuntimeError("default static topology lacks public duplicate descriptor")
    else:
        cstl = sorted((prefix / "lib").glob("libcmeta_cstl.so.*"))
        if not cstl:
            raise RuntimeError("expected one shared CSTL descriptor provider")
        images["cstl"] = recorded_symbols(cstl[0])
        if not images["cstl"]["defined_descriptor"]:
            raise RuntimeError("shared CSTL does not own descriptor")
        if not all(data["needs_shared_cstl"] for data in (
            images["core"], images["provider"], images["provider_other"]
        )):
            raise RuntimeError("both DSOs must depend on one shared CSTL SONAME")
        if any(data["defined_descriptor"] for data in (
            images["core"], images["provider"], images["provider_other"]
        )):
            raise RuntimeError("shared topology still embeds duplicate local descriptors")

    # Use only explicit installed prefix and vcpkg closure, never the source
    # build tree, system Salts or ambient runner LD_LIBRARY_PATH.
    direct = [
        prefix / "lib",
        installed_vcpkg / "x64-linux" / "lib",
    ]
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = ":".join(str(directory) for directory in direct)
    env["ASAN_OPTIONS"] = (
        "detect_odr_violation=2:detect_leaks=1:halt_on_error=1:abort_on_error=1"
    )
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    result = run([str(loader), str(core), str(provider), str(other_provider)], env=env,
                 allow_failure=True)
    log = evidence / "asan-loader.log"
    log.write_text(result.stdout, encoding="utf-8")
    odr_failure = (result.returncode != 0 and
                   EXPECTED_ODR in result.stdout and
                   SYMBOL in result.stdout)
    if result.returncode == 0:
        if ("CSTL_GENERIC_SEMANTIC_EQUAL=1" not in result.stdout or
                "CSTL_LIVE_PROVIDER_SURVIVED_OTHER_UNLOAD" not in result.stdout):
            raise RuntimeError(
                "two independent Providers did not prove stable generic IDs "
                "and retained one live metadata owner after sibling unload"
            )
        required_alias = ("CSTL_GENERIC_POINTER_EQUAL=1"
                          if mode == "shared-single"
                          else "CSTL_GENERIC_POINTER_EQUAL=0")
        if required_alias not in result.stdout:
            raise RuntimeError(
                "wrong descriptor identity alias topology: "
                f"expected {required_alias}; got:\n{result.stdout[-5000:]}"
            )
    if mode == "static-default":
        if not odr_failure:
            raise RuntimeError(
                "default duplicate-static topology was not rejected by ASan ODR; "
                f"exit={result.returncode}\n{result.stdout[-8000:]}"
            )
        outcome = "EXPECTED_ASAN_ODR_REJECTION"
    elif mode == "shared-single":
        if result.returncode != 0 or "CSTL_DSO_LOAD_UNLOAD_OK" not in result.stdout:
            raise RuntimeError(
                "shared single owner failed real ASan native load/unload: "
                f"{result.returncode}\n{result.stdout[-8000:]}"
            )
        if EXPECTED_ODR in result.stdout:
            raise RuntimeError("shared CSTL emitted an ASan ODR diagnostic")
        outcome = "SHARED_SINGLE_OWNER_ACCEPTED"
    elif odr_failure:
        outcome = "HIDDEN_STATIC_REJECTED_BY_ASAN_ODR"
    elif result.returncode == 0 and "CSTL_DSO_LOAD_UNLOAD_OK" in result.stdout:
        outcome = "HIDDEN_STATIC_ASAN_ACCEPTED"
    else:
        raise RuntimeError(
            "hidden-static returned unrelated failure; cannot classify as "
            f"ODR rejection or success: {result.returncode}\n{result.stdout[-8000:]}"
        )

    report = {
        "mode": mode,
        "outcome": outcome,
        "source_commit": actual_sha,
        "source_head": os.environ.get("SOURCE_HEAD_SHA", "unknown"),
        "tested_checkout": os.environ.get("GITHUB_SHA", "local"),
        "run_id": os.environ.get("GITHUB_RUN_ID", "local"),
        "installed_prefix": str(prefix),
        "meta_object_count": len(meta_objects),
        "meta_object_paths": meta_objects,
        "asan_odr_detection_enabled": True,
        "asan_odr_diagnostic_observed": odr_failure,
        "loader_exit_code": result.returncode,
        "images": images,
        "vcpkg_toolchain": str(toolchain),
        "vcpkg_revision": os.environ.get("VCPKG_CACHE_REVISION", "unknown"),
    }
    (evidence / "topology-result.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    for source_path, name in [
        (consumer / "provider.map", "provider.map"),
        (consumer / "provider_other.map", "provider_other.map"),
        (consumer / "loader.map", "loader.map"),
        (build / "compile_commands.json", "salts-compile_commands.json"),
        (consumer / "compile_commands.json", "consumer-compile_commands.json"),
    ]:
        if source_path.is_file():
            import shutil
            shutil.copyfile(source_path, evidence / name)
    print("CSTL_TOPOLOGY_OUTCOME=" + outcome, flush=True)
    print("EVIDENCE_DIRECTORY=" + str(evidence), flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError,
            ValueError, KeyError) as exc:
        print(f"CSTL topology conformance FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
