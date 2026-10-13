#!/usr/bin/env python3
"""Qualify the ACTUAL Salts producer static owner candidate, not test overlays.

Run only on the candidate source HEAD after the normal release native build.
CFlow and CSTL external C11 fixtures are fixed by immutable review SHAs.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def require(ok, why):
    if not ok:
        raise RuntimeError(why)


def command(args, *, env=None):
    args = [str(a) for a in args]
    print("$ " + subprocess.list2cmdline(args), flush=True)
    p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace", env=env)
    print(p.stdout[-3200:], flush=True)
    if p.returncode != 0:
        raise RuntimeError(f"exit={p.returncode}: {args}\n{p.stdout[-12000:]}")
    return p.stdout


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", choices=("windows", "macos"), required=True)
    parser.add_argument("--build", required=True)
    a = parser.parse_args()
    root = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    tmp = Path(os.environ["RUNNER_TEMP"]).resolve()
    build = (root / a.build).resolve()
    evidence = tmp / f"salts-private-owner-candidate-{a.platform}"
    sdk = tmp / f"salts-private-owner-candidate-sdk-{a.platform}"
    evidence.mkdir(parents=True, exist_ok=True)
    sha = command(["git", "rev-parse", "HEAD"]).strip()
    require(sha == os.environ["EXPECTED_PRIVATE_OWNER_SHA"],
            "candidate source HEAD changed or wrong release producer")
    tc = Path(os.environ["QIGAO_VCPKG_TOOLCHAIN_FILE"]).resolve()
    require(tc.is_file(), "pinned vcpkg-cache toolchain unavailable")
    triplet = ("arm64-osx" if a.platform == "macos" else
               os.environ.get("QIGAO_VCPKG_WINDOWS_TRIPLET", ""))
    require(triplet, "target vcpkg triplet missing")
    installed = root / "vcpkg_installed"
    compdb = build / "compile_commands.json"
    require(compdb.is_file(), "producer's actual native compile_commands.json missing")
    commands = json.loads(compdb.read_text(encoding="utf-8"))
    objects = []
    for source, target, suffix in (
        ("cflow/src/executor.c", "cmeta_cflow", "executor.c"),
        ("cstl/src/associative_meta.c", "cmeta_cstl", "associative_meta.c"),
    ):
        matches = [row for row in commands if
                   row.get("file", "").replace("\\", "/").endswith("/" + source)]
        require(len(matches) == 1, f"original producer source compiled zero/multiple times: {source}")
        compile_cmd = matches[0].get("command", "")
        if a.platform == "macos":
            require("-fvisibility=hidden" in compile_cmd,
                    f"candidate source object is not genuinely image-local: {source}")
        extension = suffix + (".obj" if a.platform == "windows" else ".o")
        built = [x for x in build.rglob(extension) if x.is_file() and
                 f"{target}.dir" in str(x)]
        require(len(built) == 1, f"expected actual built producer object: {source}")
        objects.append({"source": matches[0]["file"], "command": compile_cmd,
                        "object": str(built[0]), "sha256": digest(built[0])})
    command(["cmake", "--install", build, "--prefix", sdk])
    require((sdk / "lib/cmake/Salts/SaltsConfig.cmake").is_file(),
            "fresh installed Salts SDK absent")
    vcpkg_prefix = installed / triplet
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([str(sdk / "bin"), str(sdk / "lib"),
                                   str(vcpkg_prefix / "bin"), env.get("PATH", "")])
    if a.platform == "macos":
        env["DYLD_LIBRARY_PATH"] = os.pathsep.join([
            str(sdk / "lib"), str(vcpkg_prefix / "lib"),
            env.get("DYLD_LIBRARY_PATH", "")])
    records = []
    for component in ("cflow", "cstl"):
        fixtures = root / "build/private-owner-fixtures" / component
        expected_fixture = (
            "d938a9843d9db4cbbb1d6db3e1546b82df4a602b" if component == "cflow"
            else "579de3d85050799c5f93d19b92e3dbda7d58181f")
        actual_fixture = command(["git", "-C", fixtures, "rev-parse", "HEAD"]).strip()
        require(actual_fixture == expected_fixture, "source fixture provenance mismatch")
        fixture_src = fixtures / f"cmake/ci/{component}-dso-odr/cross"
        consumer = tmp / f"salts-private-{component}-consumer-{a.platform}"
        opts = [
            "cmake", "-S", fixture_src, "-B", consumer, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=" + str(tc),
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DVCPKG_MANIFEST_MODE=OFF",
            "-DVCPKG_TARGET_TRIPLET=" + triplet, "-DVCPKG_HOST_TRIPLET=" + triplet,
            "-DVCPKG_INSTALLED_DIR=" + str(installed),
            "-DCMAKE_PREFIX_PATH=" + str(vcpkg_prefix),
            "-DEXACT_SALTS_PREFIX=" + str(sdk)]
        if component == "cflow":
            opts += ["-DCROSS_CFLOW_EXPECTED_IDENTITY=isolated"]
        command(opts, env=env)
        command(["cmake", "--build", consumer, "--parallel", "2"], env=env)
        cache = (consumer / "CMakeCache.txt").read_text(encoding="utf-8").replace("\\", "/")
        actual_package = re.findall(r"(?m)^Salts_DIR:[^=]+=(.*)$", cache)
        require(len(actual_package) == 1 and
                Path(actual_package[0]).resolve() == (sdk / "lib/cmake/Salts").resolve(),
                "standalone native C11 consumer imported a foreign SDK")
        test_output = command([
            "ctest", "--test-dir", consumer, "-R", f"^{component}_cross_",
            "--repeat", "until-fail:3", "--no-tests=error",
            "--output-on-failure", "-V", "--timeout", "120"], env=env)
        require(re.search(r"100% tests passed(?:, 0 tests failed)? out of 2",
                          test_output) is not None,
                "two independent native CTest cases were not both successful")
        markers = (
            ("CFLOW_CROSS_TYPE_SHAPE_EQUAL=1",
             "CFLOW_CROSS_PUBLIC_SEMANTIC_EQUAL_BOTH=1",
             "CFLOW_CROSS_LIVE_FIRST_OWNER_OK",
             "CFLOW_CROSS_TWO_DSO_LOAD_UNLOAD_OK",
             "CFLOW_CROSS_OWNER=image-local")
            if component == "cflow" else
            ("CSTL_CROSS_PUBLIC_GENERIC_SEMANTIC_EQUAL=1",
             "CSTL_CROSS_IMAGE_LOCAL_OWNER=1",
             "CSTL_CROSS_LIVE_OWNER_SURVIVED_UNLOAD=1",
             "CSTL_CROSS_LOAD_UNLOAD_OK")
        )
        for marker in markers:
            require(test_output.count(marker) == 6,
                    f"{component}: expected six native processes for {marker}")
        images = []
        roles = ("client", "server") if component == "cflow" else ("first", "second")
        for role in roles:
            name = (f"{component}_cross_{role}.dll" if a.platform == "windows" else
                    f"lib{component}_cross_{role}.dylib")
            lib = consumer / name
            require(lib.is_file(), f"actual C11 test DSO missing: {lib}")
            exported = command(["dumpbin", "/exports", lib] if a.platform == "windows"
                               else ["nm", "-gU", lib])
            forbidden = ("cflow_type_executor_protocol_stats_ptr"
                         if component == "cflow" else "stl_bplus_tree_generic_desc")
            require(forbidden not in exported,
                    f"{component} C11 client leaked duplicate native metadata symbol")
            images.append({"path": str(lib), "sha256": digest(lib)})
        records.append({
            "component": component,
            "pinned_fixture_sha": actual_fixture,
            "load_orders": 2, "repetitions_each": 3,
            "native_processes": 6, "images": images, "result": "PASS"})
    result = {
        "policy": "ACTUAL_SOURCE_STATIC_PRIVATE_CSTL_CFLOW",
        "source_commit": sha, "actual_compiled_objects": objects,
        "platform": a.platform, "installed_sdk_prefix": str(sdk),
        "independent_native_consumers": records,
        "published_release_unchanged": True, "pr_draft_only": True,
        "production_policy_approved": False, "sanitizer_claimed": False}
    (evidence / "source-policy-result.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("SALTS_SOURCE_CFLOW_CSTL_PRIVATE_OWNER_CANDIDATE_PASS", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError,
            subprocess.CalledProcessError) as exc:
        print("FAILED SOURCE PRIVATE OWNER: " + str(exc), file=sys.stderr)
        raise SystemExit(1)
