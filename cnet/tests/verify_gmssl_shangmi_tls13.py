#!/usr/bin/env python3
"""Private GmSSL provider's real RFC 8998 loopback: handshake, echo, negatives."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

PASS = "fixture-only-password"
MSG = "salts-sm4-gcm-sm3-rfc8998-message\n"
CIPHER = "TLS_SM4_GCM_SM3"


def call(argv, cwd, timeout=25):
    p = subprocess.run([str(v) for v in argv], cwd=cwd, capture_output=True,
                       text=True, timeout=timeout, check=False)
    if p.returncode:
        raise RuntimeError("provider fixture command failed: {} ({}) {}".format(
            " ".join(str(v) for v in argv[:2]), p.returncode, p.stderr[-1500:]))
    return p


def fixture(gmssl, cwd):
    for prefix in ("root", "untrusted"):
        call([gmssl, "sm2keygen", "-pass", PASS,
              "-out", prefix + "-key.pem"], cwd)
        call([gmssl, "certgen", "-C", "CN", "-ST", "Beijing",
              "-L", "Haidian", "-O", "Salts", "-OU", "Test",
              "-CN", prefix, "-days", "1",
              "-key", prefix + "-key.pem", "-pass", PASS,
              "-out", prefix + "-cert.pem",
              "-key_usage", "keyCertSign", "-key_usage", "cRLSign",
              "-ca"], cwd)
    call([gmssl, "sm2keygen", "-pass", PASS, "-out", "server-key.pem"], cwd)
    call([gmssl, "reqgen", "-C", "CN", "-ST", "Beijing", "-L", "Haidian",
          "-O", "Salts", "-OU", "Test", "-CN", "localhost",
          "-key", "server-key.pem", "-pass", PASS,
          "-out", "server.req"], cwd)
    call([gmssl, "reqsign", "-in", "server.req", "-days", "1",
          "-key_usage", "digitalSignature", "-ext_key_usage", "serverAuth",
          "-subject_dns_name", "localhost", "-cacert", "root-cert.pem",
          "-key", "root-key.pem", "-pass", PASS,
          "-out", "server-cert.pem"], cwd)

    # Both roots are independent. Private-key files never enter the repo.
    for name, issuer in (("client", "root"),
                         ("untrusted-client", "untrusted"),
                         ("wrong-eku-client", "root")):
        call([gmssl, "sm2keygen", "-pass", PASS,
              "-out", name + "-key.pem"], cwd)
        call([gmssl, "reqgen", "-C", "CN", "-ST", "Beijing",
              "-L", "Haidian", "-O", "Salts", "-OU", "Test",
              "-CN", name, "-key", name + "-key.pem", "-pass", PASS,
              "-out", name + ".req"], cwd)
        purpose = "serverAuth" if name == "wrong-eku-client" else "clientAuth"
        call([gmssl, "reqsign", "-in", name + ".req", "-days", "1",
              "-key_usage", "digitalSignature", "-ext_key_usage", purpose,
              "-cacert", issuer + "-cert.pem", "-key", issuer + "-key.pem",
              "-pass", PASS, "-out", name + "-cert.pem"], cwd)

    Path(cwd, "message.txt").write_text(MSG, encoding="utf-8")


def port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def handshake(gmssl, cwd, name, expected, ca="root-cert.pem",
              hostname="localhost", cipher=CIPHER):
    p = port()
    server = [gmssl, "tls13_server", "-port", str(p),
              "-cert", "server-cert.pem", "-key", "server-key.pem",
              "-pass", PASS, "-cipher_suite", CIPHER,
              "-supported_group", "sm2p256v1", "-sig_alg", "sm2sig_sm3"]
    client = [gmssl, "tls13_client", "-host", "127.0.0.1", "-port", str(p),
              "-server_name", hostname, "-cacert", ca,
              "-cipher_suite", cipher, "-supported_group", "sm2p256v1",
              "-sig_alg", "sm2sig_sm3", "-in", "message.txt"]
    with open(Path(cwd, name + ".server.out"), "wb") as sout, \
         open(Path(cwd, name + ".server.err"), "wb") as serr:
        srv = subprocess.Popen(server, cwd=cwd, stdout=sout, stderr=serr)
        try:
            # TLS server accepts one peer; probing its port would steal it.
            time.sleep(0.8)
            if srv.poll() is not None:
                raise RuntimeError(name + ": server died before handshake")
            result = subprocess.run(client, cwd=cwd, capture_output=True,
                                    text=True, timeout=20, check=False)
        finally:
            if srv.poll() is None:
                srv.terminate()
                try:
                    srv.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    srv.kill()
                    srv.wait(timeout=3)
    established = "Connection established" in (result.stdout + result.stderr)
    echoed = MSG in result.stdout
    if expected:
        if result.returncode or not established or not echoed:
            raise RuntimeError("{}: no verified RFC8998 echo; status={} echo={} "
                               "client={} server={}".format(
                name, result.returncode, echoed, result.stderr[-1800:],
                Path(cwd, name + ".server.err").read_text(
                    errors="replace")[-1800:]))
    elif established or echoed:
        raise RuntimeError(name + ": forbidden negotiation was accepted")
    print(name + ": PASS")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gmssl", required=True)
    ap.add_argument("--cnet-probe")
    ap.add_argument("--cnet-owner-fatal")
    ap.add_argument("--cnet-owner-fatal-cancel")
    ap.add_argument("--cnet-nativeio")
    ap.add_argument("--cnet-nativeio-negatives")
    ap.add_argument("--cnet-nativeio-mtls")
    a = ap.parse_args()
    gmssl = str(Path(a.gmssl).resolve())
    if not Path(gmssl).is_file():
        raise RuntimeError("missing pinned GmSSL CLI; no fallback")
    with tempfile.TemporaryDirectory(prefix="salts-rfc8998-") as cwd:
        fixture(gmssl, cwd)
        if a.cnet_nativeio_mtls:
            probe = str(Path(a.cnet_nativeio_mtls).resolve())
            if not Path(probe).is_file():
                raise RuntimeError("missing CNet NativeIO mTLS executable")
            for case in ("verified-mtls", "missing-client-cert",
                         "untrusted-client-ca", "wrong-client-eku"):
                env = dict(os.environ)
                env.update({
                    "SALTS_SHANGMI_MTLS": "required",
                    "SALTS_SHANGMI_CA": str(Path(cwd, "root-cert.pem")),
                    "SALTS_SHANGMI_CERT": str(Path(cwd, "server-cert.pem")),
                    "SALTS_SHANGMI_KEY": str(Path(cwd, "server-key.pem")),
                    "SALTS_SHANGMI_CLIENT_CERT": str(Path(cwd, "client-cert.pem")),
                    "SALTS_SHANGMI_CLIENT_KEY": str(Path(cwd, "client-key.pem")),
                    "SALTS_SHANGMI_UNTRUSTED_CLIENT_CERT":
                        str(Path(cwd, "untrusted-client-cert.pem")),
                    "SALTS_SHANGMI_UNTRUSTED_CLIENT_KEY":
                        str(Path(cwd, "untrusted-client-key.pem")),
                    "SALTS_SHANGMI_WRONG_EKU_CLIENT_CERT":
                        str(Path(cwd, "wrong-eku-client-cert.pem")),
                    "SALTS_SHANGMI_WRONG_EKU_CLIENT_KEY":
                        str(Path(cwd, "wrong-eku-client-key.pem"))
                })
                if case != "verified-mtls":
                    env["SALTS_SHANGMI_FAILURE_CASE"] = case
                trial = subprocess.run([probe], cwd=cwd, env=env,
                                       capture_output=True, text=True,
                                       timeout=30, check=False)
                if trial.returncode:
                    raise RuntimeError(
                        "CNet NativeIO SM2 mTLS scenario failed (" + case
                        + "): " + trial.stdout[-2000:] + trial.stderr[-2000:])
                print("CNet NativeIO SM2 " + case + ": PASS")
            return
        if a.cnet_nativeio_negatives:
            probe = str(Path(a.cnet_nativeio_negatives).resolve())
            if not Path(probe).is_file():
                raise RuntimeError("missing CNet NativeIO negative probe executable")
            for case in ("wrong-ca", "wrong-san", "standard-peer"):
                env = dict(os.environ)
                env.update({
                    "SALTS_SHANGMI_CA": str(Path(cwd, "root-cert.pem")),
                    "SALTS_SHANGMI_UNTRUSTED_CA":
                        str(Path(cwd, "untrusted-cert.pem")),
                    "SALTS_SHANGMI_CERT": str(Path(cwd, "server-cert.pem")),
                    "SALTS_SHANGMI_KEY": str(Path(cwd, "server-key.pem")),
                    "SALTS_SHANGMI_FAILURE_CASE": case
                })
                trial = subprocess.run([probe], cwd=cwd, env=env,
                                       capture_output=True, text=True,
                                       timeout=30, check=False)
                if trial.returncode:
                    raise RuntimeError(
                        "CNet NativeIO security negative failed (" + case
                        + "): " + trial.stdout[-1600:] + trial.stderr[-1600:])
                print("CNet NativeIO rejected " + case + ": PASS")
            return
        if a.cnet_nativeio:
            probe = str(Path(a.cnet_nativeio).resolve())
            if not Path(probe).is_file():
                raise RuntimeError("missing CNet NativeIO RFC8998 executable")
            env = dict(os.environ)
            env.update({
                "SALTS_SHANGMI_CA": str(Path(cwd, "root-cert.pem")),
                "SALTS_SHANGMI_CERT": str(Path(cwd, "server-cert.pem")),
                "SALTS_SHANGMI_KEY": str(Path(cwd, "server-key.pem"))
            })
            native = subprocess.run([probe], cwd=cwd, env=env,
                                    capture_output=True, text=True,
                                    timeout=30, check=False)
            if native.returncode:
                raise RuntimeError(
                    "CNet real NativeIO RFC8998 failed: "
                    + native.stdout[-1600:] + native.stderr[-1600:])
            print("CNet NativeIO RFC8998 owner/listener path: PASS")
            return
        if a.cnet_owner_fatal_cancel:
            probe = str(Path(a.cnet_owner_fatal_cancel).resolve())
            if not Path(probe).is_file():
                raise RuntimeError("missing private CNet cancellation-race executable")
            run = call([probe, "root-cert.pem", "server-cert.pem",
                        "server-key.pem", "cancel-after-partial"], cwd, timeout=25)
            if "CNet TLS fatal-alert cancel-before-partial-completion: PASS" not in run.stdout:
                raise RuntimeError("CNet Owner did not prove cancel/write ordering")
            print("CNet NativeIO TLS fatal-alert cancellation/completion: PASS")
            return
        if a.cnet_owner_fatal:
            probe = str(Path(a.cnet_owner_fatal).resolve())
            if not Path(probe).is_file():
                raise RuntimeError("missing private CNet fatal-alert Owner executable")
            run = call([probe, "root-cert.pem", "server-cert.pem",
                        "server-key.pem"], cwd, timeout=25)
            if "CNet TLS fatal-alert partial NativeIO owner: PASS" not in run.stdout:
                raise RuntimeError("CNet Owner did not prove one-byte alert drain")
            print("CNet NativeIO partial fatal-alert write/terminal: PASS")
            return
        if a.cnet_probe:
            probe = str(Path(a.cnet_probe).resolve())
            if not Path(probe).is_file():
                raise RuntimeError("missing CNet RFC8998 fixture executable")
            call([probe, "root-cert.pem", "server-cert.pem",
                  "server-key.pem", "untrusted-cert.pem"], cwd)
            print("CNet RFC8998 native profile: PASS")
            return
        handshake(gmssl, cwd, "verified-rfc8998", True)
        handshake(gmssl, cwd, "wrong-ca", False, ca="untrusted-cert.pem")
        handshake(gmssl, cwd, "wrong-san", False, hostname="wrong.example")
        handshake(gmssl, cwd, "cipher-mismatch", False,
                  cipher="TLS_AES_128_GCM_SHA256")
    print("RFC8998 private provider loopback acceptance: PASS")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
        print("RFC8998 private-provider conformance FAILED: " + str(exc),
              file=sys.stderr)
        sys.exit(1)
