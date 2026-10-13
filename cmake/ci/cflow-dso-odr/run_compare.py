#!/usr/bin/env python3
"""Compare CFlow CMeta descriptor ownership in two real independent DSOs.

Each mode compiles unchanged, pinned Salts release sources. Only test-specific
CMake target visibility or BUILD_SHARED_LIBS differs. No ASan ODR suppression.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

SALTS_RELEASE = "21bdb1145fbb2b39ee3fa644173ec037adba7b87"
SYMBOL = "cflow_type_executor_protocol_stats_ptr"
MODES = ("static-default", "static-hidden", "shared-single")
ODR_MARKER = "ERROR: AddressSanitizer: odr-violation"


def run(args, *, env=None, allow_failure=False):
    print("$ " + " ".join(shlex.quote(str(x)) for x in args), flush=True)
    p = subprocess.run(args, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True,
                       encoding="utf-8", errors="replace", check=False)
    print(p.stdout[-2200:], flush=True)
    if p.returncode and not allow_failure:
        raise RuntimeError(f"command failed ({p.returncode}): {args}\n{p.stdout[-12000:]}")
    return p


def checksum(path: Path):
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1048576), b""):
            digest.update(chunk)
    return digest.hexdigest()


def symbols(path: Path):
    if not path.is_file():
        raise RuntimeError(f"native image does not exist: {path}")
    defined = run(["nm", "--defined-only", str(path)]).stdout
    exported = run(["nm", "-D", "--defined-only", str(path)]).stdout
    needed = run(["readelf", "-d", str(path)]).stdout
    all_symbols = run(["nm", "-a", str(path)]).stdout
    return {
        "path": str(path),
        "sha256": checksum(path),
        "defines_descriptor": bool(re.search(rf"(?m)^.*\b{SYMBOL}$", defined)),
        "exports_descriptor": bool(re.search(rf"(?m)^.*\b{SYMBOL}$", exported)),
        "needs_cflow_shared": bool(re.search(r"\(NEEDED\).*libcmeta_cflow\.so", needed)),
        "instrumented_image": "__asan_" in all_symbols and "libasan.so" in needed,
        "needed": [line.strip() for line in needed.splitlines()
                   if "(NEEDED)" in line],
    }


def require(test: bool, msg: str):
    if not test:
        raise RuntimeError(msg)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", required=True, choices=MODES)
    mode = parser.parse_args().mode
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    runner_temp = Path(os.environ["RUNNER_TEMP"]).resolve()
    source = workspace / "salts-source"
    root = workspace / "cmake/ci/cflow-dso-odr"
    build = runner_temp / f"salts-cflow-{mode}-build"
    prefix = runner_temp / f"salts-cflow-{mode}-sdk"
    consumer = runner_temp / f"salts-cflow-{mode}-consumer"
    evidence = runner_temp / f"salts-cflow-evidence-{mode}"
    evidence.mkdir(parents=True, exist_ok=True)

    actual = run(["git", "-C", str(source), "rev-parse", "HEAD"]).stdout.strip()
    require(actual == SALTS_RELEASE, f"incorrect pinned Salts source {actual}")
    toolchain = Path(os.environ["QIGAO_VCPKG_TOOLCHAIN_FILE"]).resolve()
    require(toolchain.is_file(), "missing immutable cache toolchain")
    installed_vcpkg = workspace / "vcpkg_installed" / f"cflow-odr-{mode}"
    source_flags = "-fsanitize=address -fsanitize=undefined -g -fno-omit-frame-pointer"
    config = [
        "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        "-DCMAKE_C_COMPILER=/usr/bin/gcc", "-DCMAKE_CXX_COMPILER=/usr/bin/g++",
        "-DCMAKE_C_FLAGS=" + source_flags, "-DCMAKE_CXX_FLAGS=" + source_flags,
        "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address -fsanitize=undefined",
        "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address -fsanitize=undefined",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DENABLE_SANITIZER_ADDRESS=ON", "-DENABLE_SANITIZER_UNDEFINED=ON",
        "-DENABLE_SANITIZER_THREAD=OFF",
        "-DBUILD_TESTS=OFF", "-DENABLE_TESTS=OFF",
        "-DBUILD_EXAMPLES=OFF", "-DBUILD_BENCHMARKS=OFF",
        "-DCMETA_BUILD_BENCHMARKS=OFF",
        "-DNATIVE_IO_BUILD_BENCHMARKS=OFF",
        "-DCNET_BUILD_BENCHMARKS=OFF", "-DCFLOW_BUILD_BENCHMARKS=OFF",
        "-DCONCURRENCY_BUILD_BENCHMARKS=OFF",
        "-DCMAKE_SUPPRESS_REGENERATION=ON",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DBUILD_SHARED_LIBS=" + ("ON" if mode == "shared-single" else "OFF"),
        "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain),
        "-DCMAKE_INSTALL_PREFIX=" + str(prefix),
        "-DVCPKG_TARGET_TRIPLET=x64-linux", "-DVCPKG_HOST_TRIPLET=x64-linux",
        "-DVCPKG_INSTALLED_DIR=" + str(installed_vcpkg),
    ]
    if mode != "shared-single":
        config.extend([
            "-DCMAKE_PROJECT_Salts_INCLUDE=" + str(root / "hidden_static.cmake"),
            "-DCFLOW_DSO_HIDE_CFLOW=" + ("ON" if mode == "static-hidden" else "OFF")
        ])
    run(config)
    run(["cmake", "--build", str(build), "--parallel", "2"])
    run(["cmake", "--install", str(build)])

    # Count the actually built source object, not just the configure database.
    database = json.loads((build / "compile_commands.json").read_text("utf-8"))
    expected_source = (source / "cflow/src/executor.c").resolve()
    rows = [r for r in database if Path(r["file"]).resolve() == expected_source]
    require(len(rows) == 1, "expected exactly one real compiled CFlow executor.c")
    row = rows[0]
    args = row.get("arguments") or shlex.split(row["command"])
    require(all(f in args for f in ("-fsanitize=address",
                                   "-fsanitize=undefined",
                                   "-fno-omit-frame-pointer")),
            "CFlow executor actual compiler object is not fully sanitized")
    if mode == "static-hidden":
        require("-fvisibility=hidden" in args,
                "static-hidden mode did not compile executor with hidden symbols")
    if mode == "static-default":
        require("-fvisibility=hidden" not in args,
                "default static CFlow was unexpectedly hidden")
    require("-o" in args, "actual CFlow object path not in compiler command")
    output = Path(args[args.index("-o") + 1])
    if not output.is_absolute():
        output = Path(row["directory"]) / output
    require(output.is_file(), f"actual CFlow object missing: {output}")

    consumer_config = [
        "cmake", "-S", str(root / "consumer"), "-B", str(consumer),
        "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain),
        "-DVCPKG_MANIFEST_MODE=OFF",
        "-DVCPKG_TARGET_TRIPLET=x64-linux",
        "-DVCPKG_HOST_TRIPLET=x64-linux",
        "-DVCPKG_INSTALLED_DIR=" + str(installed_vcpkg),
        "-DCMAKE_PREFIX_PATH=" + str(installed_vcpkg / "x64-linux"),
        "-DEXACT_SALTS_PREFIX=" + str(prefix),
    ]
    run(consumer_config)
    run(["cmake", "--build", str(consumer), "--parallel", "2"])
    client = consumer / "libcflow_probe_client.so"
    server = consumer / "libcflow_probe_server.so"
    loader = consumer / "cflow_probe_loader"
    require(all(p.is_file() for p in (client, server, loader)),
            "one real native CFlow DSO or loader was not built")

    images = {"client": symbols(client), "server": symbols(server)}
    require(all(x["instrumented_image"] for x in images.values()),
            "one native CFlow DSO lacks ASan image evidence")
    if mode == "shared-single":
        shared = sorted((prefix / "lib").glob("libcmeta_cflow.so.*"))
        require(shared, "shared-single CFlow does not publish shared SONAME")
        images["shared_cflow"] = symbols(shared[0])
        require(images["shared_cflow"]["defines_descriptor"],
                "shared CFlow does not define canonical descriptor")
        for name in ("client", "server"):
            require(images[name]["needs_cflow_shared"],
                    f"{name} lacks DT_NEEDED on single CFlow library")
            require(not images[name]["defines_descriptor"],
                    f"{name} unexpectedly embeds private CFlow descriptor")
    else:
        require(all(images[n]["defines_descriptor"] for n in ("client", "server")),
                "static mode did not actually embed executor descriptor in both images")
        for name in ("client", "server"):
            require(not images[name]["needs_cflow_shared"],
                    f"{name} unexpectedly links a shared CFlow library")
            if mode == "static-default":
                require(images[name]["exports_descriptor"],
                        f"{name} lacks intended default-visible duplicate")
            else:
                require(not images[name]["exports_descriptor"],
                        f"{name} illegally exports private CFlow metadata")

    runtime = dict(os.environ)
    runtime["LD_LIBRARY_PATH"] = ":".join([
        str(prefix / "lib"), str(installed_vcpkg / "x64-linux/lib")])
    runtime["ASAN_OPTIONS"] = "detect_odr_violation=2:detect_leaks=1:halt_on_error=1:abort_on_error=1"
    runtime["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    outcomes = []
    for name, first, second in (
        ("client-first", client, server),
        ("server-first", server, client),
    ):
        expected_identity = "shared" if mode == "shared-single" else "isolated"
        test = run([str(loader), str(first), str(second), expected_identity],
                   env=runtime, allow_failure=True)
        (evidence / f"asan-{name}.log").write_text(test.stdout, encoding="utf-8")
        odr = test.returncode != 0 and ODR_MARKER in test.stdout and (
            "cflow_type_" in test.stdout or "cflow/src/executor.c" in test.stdout)
        if mode == "static-default":
            require(odr, f"default visible duplicate not rejected by exact CFlow ASan ODR: {name}\n"
                         + test.stdout[-9000:])
            outcome = "EXPECTED_CFLOW_ASAN_ODR_REJECTION"
        else:
            require(test.returncode == 0 and
                    "CFLOW_DSO_LOAD_QUERY_UNLOAD_OK" in test.stdout and
                    "CFLOW_DESCRIPTOR_SHAPE_MATCH=1" in test.stdout and
                    "CFLOW_SEMANTIC_EQUAL_BOTH_DIRECTIONS=1" in test.stdout and
                    "CFLOW_LIVE_OWNER_SURVIVED_OTHER_UNLOAD" in test.stdout,
                    f"positive CFlow topology failed native loader: {name}\n"
                    + test.stdout[-9000:])
            require(ODR_MARKER not in test.stdout,
                    "positive mode emitted forbidden ASan ODR diagnostic")
            outcome = ("SHARED_SINGLE_CFLOW_ACCEPTED" if mode == "shared-single"
                       else "HIDDEN_STATIC_CFLOW_ACCEPTED")
        outcomes.append({"order": name, "outcome": outcome,
                         "asan_odr_observed": odr, "process_exit": test.returncode})

    report = {
        "mode": mode, "source_commit": actual,
        "comparison_source_commit": os.environ.get("SOURCE_HEAD_SHA", "unknown"),
        "asan_odr_disabled": False,
        "cstl_static_owner_baseline": ("test-only static-hidden"
                                       if mode != "shared-single" else "shared-single"),
        "actual_cflow_executor_object": str(output),
        "actual_cflow_object_count": len(rows),
        "images": images,
        "outcomes": outcomes,
        "vcpkg_toolchain": str(toolchain),
        "vcpkg_revision": os.environ.get("VCPKG_CACHE_REVISION", "unknown"),
    }
    (evidence / "topology-result.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    for path, dest in [
        (build / "compile_commands.json", "salts-compile_commands.json"),
        (consumer / "compile_commands.json", "consumer-compile_commands.json"),
        (consumer / "client.map", "client.map"),
        (consumer / "server.map", "server.map"),
        (consumer / "loader.map", "loader.map"),
    ]:
        if path.is_file():
            shutil.copyfile(path, evidence / dest)
    print("CFLOW_TOPOLOGY=" + mode, flush=True)
    for item in outcomes:
        print(f"CFLOW_TOPOLOGY_OUTCOME={item['order']}:{item['outcome']}", flush=True)
    print("EVIDENCE_DIRECTORY=" + str(evidence), flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, KeyError,
            subprocess.CalledProcessError) as exc:
        print(f"CFlow DSO topology comparison FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
