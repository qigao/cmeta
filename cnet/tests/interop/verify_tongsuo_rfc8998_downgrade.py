#!/usr/bin/env python3
"""Independent RFC8998 downgrade rejection on actual public CNet TCP endpoints.

Distinct from CA/SAN validation: both ends use a valid SM2 identity and
trusted root. Only the peer's advertised TLS version or cipher differs from
the explicitly admitted RFC8998 profile. No code path falls back to the
standard CNet security profile.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from verify_tongsuo_rfc8998 import fixture, free_port, launch, require_file, PASS, SUITE
from verify_tongsuo_rfc8998_negatives import close_server, port_from_server

AES = "TLS_AES_128_GCM_SHA256"
TLS12_CIPHER = "ECDHE-ECDSA-AES128-GCM-SHA256"


def policy_options(mode, is_server):
    if mode == "wrong-suite":
        # Advertise TLS1.3 + AES only, i.e. no overlap with RFC8998 SM4-GCM.
        return ["-tls1_3", "-ciphersuites", AES,
                "-groups", "SM2", "-enable_sm_tls13_strict"]
    if mode == "wrong-version":
        # Explicitly exclude TLS1.3; never enable provider auto-negotiation.
        # This is a version exclusion, not a claim that an SM2 TLS1.2
        # certificate/suite combination would independently be valid.
        return ["-tls1_2", "-cipher", TLS12_CIPHER]
    raise ValueError("unknown negative " + mode)


def reject_cnet_peer(tongsuo, probe, cwd, mode):
    port = free_port()
    cmd = [tongsuo, "s_server", "-accept", f"127.0.0.1:{port}",
           "-cert", "server-cert.pem", "-key", "server-key-tongsuo.pem",
           "-rev", "-naccept", "1"] + policy_options(mode, True)
    logfile = Path(cwd, f"forbidden-{mode}-tongsuo-server.log")
    with logfile.open("wb") as log:
        server = launch(cmd, cwd, log)
        try:
            # A one-accept server must not be probed with an extra TCP dial.
            time.sleep(0.8)
            if server.poll() is not None:
                raise RuntimeError(
                    "Independent Tongsuo mismatch server did not start: "
                    + logfile.read_text(errors="replace")[-1500:])
            trial = subprocess.run(
                [probe, "root-cert.pem", str(port), mode], cwd=cwd,
                capture_output=True, timeout=16, check=False)
            if (trial.returncode != 0 or
                    b"rejected independent untrusted" not in trial.stdout):
                raise RuntimeError(
                    "Public CNet client admitted/failed to reject " + mode +
                    ": " + trial.stderr.decode(errors="replace")[-1800:])
            print("Public CNet RFC8998 client rejects Tongsuo " + mode + ": PASS")
        finally:
            close_server(server)


def reject_tongsuo_peer(tongsuo, probe, cwd, mode):
    logpath = Path(cwd, f"forbidden-{mode}-cnet-server.log")
    with logpath.open("wb") as serverlog:
        server = launch(
            [probe, "server-cert.pem", "server-key.pem", "reject"],
            cwd, serverlog)
        try:
            port = port_from_server(server)
            cmd = [tongsuo, "s_client", "-connect", f"127.0.0.1:{port}",
                   "-servername", "localhost", "-CAfile", "root-cert.pem",
                   "-verify_return_error", "-verify_hostname", "localhost",
                   "-brief"] + policy_options(mode, False)
            trial = subprocess.run(
                cmd, cwd=cwd, input=b"", capture_output=True,
                timeout=12, check=False)
            transcript = (trial.stderr + b"\n" + trial.stdout).decode(
                errors="replace").lower()
            # A real TLS protocol rejection is essential; generic TCP
            # connection refusal, missing binary, or early process death
            # must not count. The CNet server separately proves accept.
            tls_rejection = any(word in transcript for word in (
                "alert", "protocol version", "handshake failure",
                "no shared cipher", "unsupported protocol",
                "wrong version", "no suitable signature algorithm"))
            if (trial.returncode == 0 or not tls_rejection or
                    "tls_sm4_gcm_sm3" in transcript or
                    b"tongsuo-cnet-rfc8998-proof" in trial.stdout):
                raise RuntimeError(
                    "Independent Tongsuo did not prove strict " + mode +
                    " TLS rejection: " + transcript[-2200:])
            try:
                code = server.wait(timeout=15)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError(
                    "CNet server did not terminate " + mode + " peer") from exc
            stdout = (server.stdout.read() if server.stdout else b"")
            if (code != 0 or
                    b"observed strict Tongsuo peer rejection: PASS" not in stdout):
                raise RuntimeError(
                    "CNet server did not prove admitted socket / zero "
                    "application data after " + mode + ": "
                    + serverlog.read_text(errors="replace")[-1800:])
            print("Tongsuo strict peer rejects CNet RFC8998 " + mode + ": PASS")
        finally:
            close_server(server)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tongsuo", required=True)
    ap.add_argument("--gmssl", required=True)
    ap.add_argument("--fixture-key-converter", required=True)
    ap.add_argument("--cnet-client", required=True)
    ap.add_argument("--cnet-server", required=True)
    a = ap.parse_args()
    tong = require_file(a.tongsuo)
    gmssl = require_file(a.gmssl)
    converter = require_file(a.fixture_key_converter)
    cnet_client = require_file(a.cnet_client)
    cnet_server = require_file(a.cnet_server)
    with tempfile.TemporaryDirectory(prefix="rfc8998-downgrade-") as cwd:
        fixture(gmssl, cwd)
        ret = subprocess.run(
            [converter, "server-key.pem", PASS, "server-key-tongsuo.pem"],
            cwd=cwd, capture_output=True, timeout=12, check=False)
        if ret.returncode:
            raise RuntimeError(
                "Ephemeral test-only SM2 key wrapper conversion failed: "
                + ret.stderr.decode(errors="replace")[-1000:])
        Path(cwd, "server-key-tongsuo.pem").chmod(0o600)
        failures = []
        # All four independent cases must run even if one fails; preserve
        # the exact observed stage and report each failed direction. Never
        # turn a generic TLS EOF into accepted protocol-alert evidence.
        for mode in ("wrong-suite", "wrong-version"):
            for peer, test in (("cnet-client", reject_cnet_peer),
                               ("tongsuo-client", reject_tongsuo_peer)):
                try:
                    test(tong, cnet_client if peer == "cnet-client"
                         else cnet_server, cwd, mode)
                except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
                    failures.append(f"{peer}/{mode}: {exc}")
                    print("Independent downgrade case NOT QUALIFIED: "
                          + failures[-1], file=sys.stderr)
        if failures:
            raise RuntimeError(
                str(len(failures)) + "/4 downgrade cases not qualified; "
                + "see per-direction diagnostics above")
    print("Independent RFC8998 CNet/Tongsuo TLS downgrade rejection: PASS")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
        print("Independent RFC8998 downgrade qualification FAILED: " + str(exc),
              file=sys.stderr)
        sys.exit(1)
