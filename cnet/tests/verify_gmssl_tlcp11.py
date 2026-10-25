#!/usr/bin/env python3
"""Generate ephemeral TLCP dual identities and run strict CNet/provider tests."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

PASS = "fixture-only-password"
MESSAGE = "salts-tlcp11-sm4-gcm-sm3-message\n"
CIPHER = "TLS_ECC_SM4_GCM_SM3"


def call(argv, cwd, timeout=25):
    result = subprocess.run([str(value) for value in argv], cwd=cwd,
                            capture_output=True, text=True, timeout=timeout,
                            check=False)
    if result.returncode:
        raise RuntimeError("fixture command failed: {} ({}) {}".format(
            " ".join(str(value) for value in argv[:2]), result.returncode,
            result.stderr[-1600:]))
    return result


def make_identity(gmssl, cwd, name, usage, issuer="root"):
    call([gmssl, "sm2keygen", "-pass", PASS, "-out", name + "-key.pem"], cwd)
    call([gmssl, "reqgen", "-C", "CN", "-ST", "Beijing", "-L", "Haidian",
          "-O", "Salts", "-OU", "Test", "-CN", "localhost",
          "-key", name + "-key.pem", "-pass", PASS,
          "-out", name + ".req"], cwd)
    call([gmssl, "reqsign", "-in", name + ".req", "-days", "1",
          "-key_usage", usage, "-ext_key_usage", "serverAuth",
          "-subject_dns_name", "localhost", "-cacert", issuer + "-cert.pem",
          "-key", issuer + "-key.pem", "-pass", PASS,
          "-out", name + "-cert.pem"], cwd)


def fixture(gmssl, cwd):
    for name in ("root", "untrusted"):
        call([gmssl, "sm2keygen", "-pass", PASS, "-out", name + "-key.pem"], cwd)
        call([gmssl, "certgen", "-C", "CN", "-ST", "Beijing", "-L", "Haidian",
              "-O", "Salts", "-OU", "Test", "-CN", name, "-days", "1",
              "-key", name + "-key.pem", "-pass", PASS,
              "-key_usage", "keyCertSign", "-key_usage", "cRLSign", "-ca",
              "-out", name + "-cert.pem"], cwd)
    make_identity(gmssl, cwd, "server-sign", "digitalSignature")
    make_identity(gmssl, cwd, "server-enc", "keyEncipherment")
    make_identity(gmssl, cwd, "unrelated-enc", "keyEncipherment")
    path = Path(cwd)
    path.joinpath("server-certs.pem").write_text(
        path.joinpath("server-sign-cert.pem").read_text() +
        path.joinpath("server-enc-cert.pem").read_text() +
        path.joinpath("root-cert.pem").read_text(), encoding="ascii")
    path.joinpath("server-keys.pem").write_text(
        path.joinpath("server-sign-key.pem").read_text() +
        path.joinpath("server-enc-key.pem").read_text(), encoding="ascii")
    path.joinpath("mismatched-keys.pem").write_text(
        path.joinpath("server-sign-key.pem").read_text() +
        path.joinpath("unrelated-enc-key.pem").read_text(), encoding="ascii")
    path.joinpath("reversed-certs.pem").write_text(
        path.joinpath("server-enc-cert.pem").read_text() +
        path.joinpath("server-sign-cert.pem").read_text() +
        path.joinpath("root-cert.pem").read_text(), encoding="ascii")
    path.joinpath("reversed-keys.pem").write_text(
        path.joinpath("server-enc-key.pem").read_text() +
        path.joinpath("server-sign-key.pem").read_text(), encoding="ascii")
    path.joinpath("malformed-certs.pem").write_text(
        "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n",
        encoding="ascii")
    path.joinpath("message.txt").write_text(MESSAGE, encoding="utf-8")


def available_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def provider_wire(gmssl, cwd):
    port = available_port()
    server = [gmssl, "tlcp_server", "-port", str(port),
              "-cert", "server-certs.pem", "-key", "server-keys.pem",
              "-pass", PASS, "-cipher_suite", CIPHER]
    client = [gmssl, "tlcp_client", "-host", "127.0.0.1", "-port", str(port),
              "-server_name", "localhost", "-cacert", "root-cert.pem",
              "-cipher_suite", CIPHER, "-in", "message.txt"]
    process = subprocess.Popen(server, cwd=cwd, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.8)
        result = subprocess.run(client, cwd=cwd, capture_output=True, text=True,
                                timeout=20, check=False)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
    if result.returncode or "Connection established" not in (result.stdout + result.stderr):
        raise RuntimeError("GmSSL TLCP strict GCM handshake failed: " + result.stderr[-1600:])
    print("GmSSL TLCP strict GCM wire: PASS")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--gmssl", required=True)
    parser.add_argument("--cnet-probe")
    parser.add_argument("--cnet-nativeio")
    parser.add_argument("--cnet-nativeio-negatives")
    args = parser.parse_args()
    gmssl = str(Path(args.gmssl).resolve())
    if not Path(gmssl).is_file():
        raise RuntimeError("missing pinned GmSSL CLI; no fallback")
    with tempfile.TemporaryDirectory(prefix="salts-tlcp11-") as cwd:
        fixture(gmssl, cwd)
        if args.cnet_probe:
            probe = str(Path(args.cnet_probe).resolve())
            call([probe, "root-cert.pem", "server-certs.pem", "server-keys.pem",
                  "untrusted-cert.pem", "mismatched-keys.pem",
                  "malformed-certs.pem", "reversed-certs.pem",
                  "reversed-keys.pem"], cwd)
            print("CNet TLCP strict profile: PASS")
        elif args.cnet_nativeio or args.cnet_nativeio_negatives:
            selected_probe = args.cnet_nativeio or args.cnet_nativeio_negatives
            probe = str(Path(selected_probe).resolve())
            env = dict(os.environ)
            env.update({
                "SALTS_TLCP_CA": str(Path(cwd, "root-cert.pem")),
                "SALTS_TLCP_CERT": str(Path(cwd, "server-certs.pem")),
                "SALTS_TLCP_KEY": str(Path(cwd, "server-keys.pem")),
                "SALTS_SHANGMI_UNTRUSTED_CA": str(Path(cwd, "untrusted-cert.pem")),
                "SALTS_SHANGMI_MALFORMED_CERT": str(Path(cwd, "malformed-certs.pem")),
                "SALTS_SHANGMI_MISMATCHED_KEY": str(Path(cwd, "mismatched-keys.pem")),
            })
            cases = ("verified",)
            if args.cnet_nativeio_negatives:
                cases = ("wrong-ca", "wrong-san", "standard-peer",
                         "malformed-server-cert", "mismatched-server-key")
            for case in cases:
                trial_env = dict(env)
                if case != "verified":
                    trial_env["SALTS_SHANGMI_FAILURE_CASE"] = case
                result = subprocess.run([probe], cwd=cwd, env=trial_env,
                                        capture_output=True, text=True, timeout=30,
                                        check=False)
                if result.returncode:
                    raise RuntimeError("CNet TLCP NativeIO failed (" + case + "): " +
                                       result.stdout[-1200:] + result.stderr[-1200:])
                print("CNet TLCP NativeIO " + case + ": PASS")
        else:
            provider_wire(gmssl, cwd)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
        print("TLCP qualification FAILED: " + str(exc), file=sys.stderr)
        sys.exit(1)
