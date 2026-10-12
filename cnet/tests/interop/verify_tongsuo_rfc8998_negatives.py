#!/usr/bin/env python3
"""Fail-closed independent Tongsuo <-> public CNet NativeIO RFC8998 cases.

Every case starts from fresh ephemeral SM2 identity and real loopback TCP.
Positive interop is a separate mandatory CI step. A negative is NOT a pass
just because TCP failed or a process exited: require peer verification failure
on the rejecting side and absence of authenticated application plaintext.
"""
import argparse
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time

from verify_tongsuo_rfc8998 import fixture, free_port, launch, require_file, PASS, SUITE


def close_server(server):
    if server.poll() is None:
        server.terminate()
    try:
        server.wait(timeout=3)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait(timeout=3)
    for pipe in (server.stdin, server.stdout):
        if pipe:
            pipe.close()


def reject_cnet_client(tongsuo, cnet_client, cwd, mode):
    port = free_port()
    cmd = [tongsuo, "s_server", "-accept", f"127.0.0.1:{port}",
           "-cert", "server-cert.pem", "-key", "server-key-tongsuo.pem",
           "-tls1_3", "-ciphersuites", SUITE, "-groups", "SM2",
           "-enable_sm_tls13_strict", "-rev", "-naccept", "1"]
    log = Path(cwd, f"reject-cnet-client-{mode}.log")
    with log.open("wb") as errors:
        server = launch(cmd, cwd, errors)
        try:
            # Do not consume the one-accept listener with a readiness probe.
            time.sleep(0.8)
            if server.poll() is not None:
                raise RuntimeError("Tongsuo test server failed before TCP admission")
            trust = "untrusted-cert.pem" if mode == "wrong-ca" else "root-cert.pem"
            trial = subprocess.run([cnet_client, trust, str(port), mode],
                                   cwd=cwd, capture_output=True, timeout=16,
                                   check=False)
            if trial.returncode != 0 or b"rejected independent untrusted" not in trial.stdout:
                raise RuntimeError(
                    "Public CNet client failed strict peer rejection: " + mode +
                    "; stderr=" + trial.stderr.decode(errors="replace")[-1900:])
            print("Public CNet NativeIO client rejects Tongsuo " + mode + ": PASS")
        finally:
            close_server(server)


def port_from_server(server):
    if server.stdout is None:
        raise RuntimeError("Public CNet server did not expose port channel")
    selector = selectors.DefaultSelector()
    try:
        selector.register(server.stdout, selectors.EVENT_READ)
        if not selector.select(timeout=6):
            raise RuntimeError("CNet listener did not publish a port")
        raw = server.stdout.readline()
    finally:
        selector.close()
    try:
        port = int(raw.strip())
    except ValueError as exc:
        raise RuntimeError("CNet listener published malformed port") from exc
    if port < 1 or port > 65535 or server.poll() is not None:
        raise RuntimeError("CNet listener exited without binding")
    return port


def reject_tongsuo_client(tongsuo, cnet_server, cwd, mode):
    log = Path(cwd, f"reject-tongsuo-client-{mode}.server.log")
    with log.open("wb") as errors:
        server = launch([cnet_server, "server-cert.pem",
                         "server-key.pem", "reject"], cwd, errors)
        try:
            port = port_from_server(server)
            trust = "untrusted-cert.pem" if mode == "wrong-ca" else "root-cert.pem"
            name = "wrong.example" if mode == "wrong-san" else "localhost"
            cmd = [tongsuo, "s_client", "-connect", f"127.0.0.1:{port}",
                   "-servername", name, "-CAfile", trust,
                   "-verify_return_error", "-verify_hostname", name,
                   "-tls1_3", "-ciphersuites", SUITE, "-groups", "SM2",
                   "-enable_sm_tls13_strict", "-brief"]
            client = subprocess.run(cmd, cwd=cwd, input=b"",
                                    capture_output=True, timeout=12, check=False)
            response = client.stdout + b"\n" + client.stderr
            detail = response.decode(errors="replace").lower()
            # The precise wording is Tongsuo-defined, but a *verification*
            # failure must be observed. Network/readiness/exec failure alone
            # is not acceptable evidence.
            verification_failed = (
                ("verify" in detail or "certificate" in detail)
                and any(term in detail for term in
                        ("error", "fail", "mismatch", "unable")))
            if (client.returncode == 0 or not verification_failed
                    or b"tongsuo-cnet-rfc8998-proof" in client.stdout
                    or "verification: ok" in detail):
                raise RuntimeError(
                    "Tongsuo did not reject CNet invalid identity ("
                    + mode + "): " + detail[-2300:])
            try:
                code = server.wait(timeout=15)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError("CNet server did not terminate rejected TLS peer") from exc
            server_log = log.read_text(errors="replace")
            if code != 0 or "observed strict Tongsuo peer rejection: PASS" not in (
                    server.stdout.read().decode(errors="replace")
                    if server.stdout else ""):
                raise RuntimeError(
                    "CNet server did not prove accepted TCP / zero authenticated "
                    "application data (" + mode + "): " + server_log[-2000:])
            print("Tongsuo strict client rejects CNet " + mode + ": PASS")
        finally:
            close_server(server)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tongsuo", required=True)
    parser.add_argument("--gmssl", required=True)
    parser.add_argument("--fixture-key-converter", required=True)
    parser.add_argument("--cnet-client", required=True)
    parser.add_argument("--cnet-server", required=True)
    args = parser.parse_args()
    tong = require_file(args.tongsuo)
    gmssl = require_file(args.gmssl)
    converter = require_file(args.fixture_key_converter)
    cnet_client = require_file(args.cnet_client)
    cnet_server = require_file(args.cnet_server)
    with tempfile.TemporaryDirectory(prefix="tongsuo-rfc8998-rejections-") as cwd:
        fixture(gmssl, cwd)
        converted = subprocess.run(
            [converter, "server-key.pem", PASS, "server-key-tongsuo.pem"],
            cwd=cwd, capture_output=True, timeout=12, check=False)
        if converted.returncode:
            raise RuntimeError(
                "Could not normalize ephemeral test-only PKCS8 SM2 key: "
                + converted.stderr.decode(errors="replace")[-1000:])
        Path(cwd, "server-key-tongsuo.pem").chmod(0o600)
        for mode in ("wrong-ca", "wrong-san"):
            reject_cnet_client(tong, cnet_client, cwd, mode)
        for mode in ("wrong-ca", "wrong-san"):
            reject_tongsuo_client(tong, cnet_server, cwd, mode)
    print("Independent strict RFC8998 CNet↔Tongsuo negative peer validation: PASS")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
        print("Independent RFC8998 strict peer negative FAILED: " + str(exc),
              file=sys.stderr)
        sys.exit(1)
