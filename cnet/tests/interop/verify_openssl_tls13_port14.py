#!/usr/bin/env python3
"""Qualify the real public CNet NativeIO owner against independent OpenSSL.

This is NOT a direct GmSSL provider test. The GmSSL 3.2.0#14 provider is
linked privately into the exact-source, ASan/UBSan-instrumented Salts SDK.
CNet alone owns the native listener, TLS handshake, data and terminal state.

Require TLS1.3 + independent OpenSSL verified mTLS with a fresh P-256 CA:
(a) explicit P256 no CCS, (b) P256 default CCS,
(c) OpenSSL X25519-first HRR without CCS, (d) HRR plus default CCS.
Negative peers: absent certificate, wrong issuing CA and wrong EKU.
No plaintext fallback, legacy renegotiation bypass or provider alternative.
"""
from __future__ import annotations

import pathlib
import select
import socket
import ssl
import subprocess
import sys
import tempfile

PAYLOAD = b"salts-cnet-hrr-ccs-mtls-probe\n"
ROUTE_TIMEOUT = 7


def openssl(*args: str) -> None:
    subprocess.run(["openssl", *args], check=True, capture_output=True,
                   timeout=20)


def create_certificates(root: pathlib.Path):
    ca, key = root / "trusted-ca.crt", root / "trusted-ca.key"
    openssl("req", "-x509", "-newkey", "ec",
            "-pkeyopt", "ec_paramgen_curve:prime256v1",
            "-sha256", "-nodes", "-days", "2",
            "-subj", "/CN=CNet Port14 Test CA",
            "-keyout", str(key), "-out", str(ca),
            "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")

    other_ca, other_key = root / "other-ca.crt", root / "other-ca.key"
    openssl("req", "-x509", "-newkey", "ec",
            "-pkeyopt", "ec_paramgen_curve:prime256v1",
            "-sha256", "-nodes", "-days", "2",
            "-subj", "/CN=Untrusted CA", "-keyout", str(other_key),
            "-out", str(other_ca),
            "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")

    def leaf(label: str, issuer: pathlib.Path, issuer_key: pathlib.Path,
             purpose: str):
        cert, pkey = root / (label + ".crt"), root / (label + ".key")
        csr, ext = root / (label + ".csr"), root / (label + ".ext")
        ext.write_text("basicConstraints=critical,CA:FALSE\n"
                       "keyUsage=critical,digitalSignature\n"
                       "extendedKeyUsage=" + purpose + "\n" +
                       ("subjectAltName=DNS:localhost,IP:127.0.0.1\n"
                        if label == "server" else ""), encoding="ascii")
        openssl("req", "-newkey", "ec",
                "-pkeyopt", "ec_paramgen_curve:prime256v1",
                "-sha256", "-nodes", "-subj", "/CN=" + label,
                "-keyout", str(pkey), "-out", str(csr))
        openssl("x509", "-req", "-in", str(csr),
                "-CA", str(issuer), "-CAkey", str(issuer_key),
                "-CAcreateserial", "-sha256", "-days", "2",
                "-extfile", str(ext), "-out", str(cert))
        return cert, pkey

    server = leaf("server", ca, key, "serverAuth")
    good = leaf("consumer", ca, key, "clientAuth")
    untrusted = leaf("bad-issuer", other_ca, other_key, "clientAuth")
    wrong_eku = leaf("bad-purpose", ca, key, "serverAuth")
    openssl("verify", "-CAfile", str(ca),
            "-purpose", "sslserver", "-verify_hostname", "localhost",
            str(server[0]))
    openssl("verify", "-CAfile", str(ca), "-purpose", "sslclient",
            str(good[0]))
    return ca, server, good, untrusted, wrong_eku


def client_context(ca, identity, explicit_p256: bool, compat: bool):
    ctx = ssl.create_default_context(cafile=str(ca))
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    ctx.set_alpn_protocols(["http/1.1"])
    if explicit_p256:
        ctx.set_ecdh_curve("prime256v1")
    if not compat:
        ctx.options &= ~ssl.OP_ENABLE_MIDDLEBOX_COMPAT
    if identity is not None:
        cert, key = identity
        ctx.load_cert_chain(str(cert), str(key))
    return ctx


def server_run(binary, material, identity, groups_p256: bool,
               compat: bool, expected_ok: bool, description: str):
    ca, server, _, _, _ = material
    args = [str(binary), str(ca), str(server[0]), str(server[1])]
    if not expected_ok:
        args.append("reject")
    proc = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            text=True, bufsize=1)
    observed_error = None
    try:
        assert proc.stdout is not None
        ready, _, _ = select.select([proc.stdout], [], [], ROUTE_TIMEOUT)
        if not ready:
            raise AssertionError(description + ": listener never ready")
        line = proc.stdout.readline().strip()
        if not line.startswith("CNET_PORT14_TLS_PORT="):
            raise AssertionError(description + ": invalid listener ready line")
        port = int(line.partition("=")[2])
        if port < 1 or port > 65535:
            raise AssertionError(description + ": invalid port")
        ctx = client_context(ca, identity, groups_p256, compat)
        try:
            with socket.create_connection(("127.0.0.1", port),
                                          timeout=ROUTE_TIMEOUT) as tcp:
                with ctx.wrap_socket(tcp, server_hostname="localhost") as tls:
                    if tls.version() != "TLSv1.3":
                        raise AssertionError(description + ": protocol downgrade")
                    if tls.cipher()[0] != "TLS_AES_128_GCM_SHA256":
                        raise AssertionError(description + ": cipher downgrade")
                    tls.settimeout(ROUTE_TIMEOUT)
                    tls.sendall(PAYLOAD)
                    received = bytearray()
                    while len(received) < len(PAYLOAD):
                        piece = tls.recv(len(PAYLOAD) - len(received))
                        if not piece:
                            break
                        received.extend(piece)
                    if expected_ok:
                        if bytes(received) != PAYLOAD:
                            raise AssertionError(
                                description + ": no verified application echo")
                    elif bytes(received) == PAYLOAD:
                        raise AssertionError(
                            description + ": unauthorized application echo")
        except (ssl.SSLError, ConnectionError, OSError) as exc:
            if expected_ok:
                raise AssertionError(
                    description + ": TLS/transport rejection " +
                    type(exc).__name__) from exc
        proc.wait(timeout=ROUTE_TIMEOUT + 4)
        if proc.returncode != 0:
            raise AssertionError(
                description + ": CNet owner rejected contract exit=" +
                str(proc.returncode))
        print("cnet.port14.openssl: PASS " + description, flush=True)
    except Exception as exc:
        observed_error = exc
        raise
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=4)
        logs = proc.stderr.read()[-1000:] if proc.stderr else ""
        if observed_error is not None:
            print("CNet owner test diagnostics: " + logs, file=sys.stderr)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_openssl_tls13_port14.py cnet-server-probe")
    binary = pathlib.Path(sys.argv[1]).resolve()
    if not binary.is_file():
        raise SystemExit("CNet native probe not built; no implicit fallback")
    with tempfile.TemporaryDirectory(prefix="cnet-port14-tls13-") as tmp:
        material = create_certificates(pathlib.Path(tmp))
        _, _, good, untrusted, wrong_eku = material
        cases = [
            ("P256 no CCS", good, True, False, True),
            ("P256 default CCS", good, True, True, True),
            ("default X25519 HRR no CCS", good, False, False, True),
            ("default X25519 HRR plus CCS", good, False, True, True),
            ("missing client certificate", None, False, True, False),
            ("untrusted client CA", untrusted, False, True, False),
            ("wrong client EKU", wrong_eku, False, True, False),
        ]
        for label, identity, p256, compat, expected in cases:
            server_run(binary, material, identity, p256, compat,
                       expected, label)


if __name__ == "__main__":
    main()
