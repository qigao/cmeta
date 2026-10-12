#!/usr/bin/env python3
"""Independent RFC8998 qualification: pinned Tongsuo versus pinned GmSSL CLI.

This is a TEST-ONLY cross-provider preflight, not yet a CNet NativeIO test.
Tongsuo MUST enable its nondefault strict-SM2 TLS1.3 mode. Never accept
successful TLS over STANDARD/AES suites or a generic handshake without echo.
Private SM2 keys are generated in a temporary workspace, never committed.
"""
import argparse
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from verify_gmssl_shangmi_tls13 import fixture, PASS  # noqa: E402

MESSAGE = b"tongsuo-gmssl-rfc8998-proof\n"
SUITE = "TLS_SM4_GCM_SM3"


def launch(cmd, cwd, log):
    return subprocess.Popen(
        [str(a) for a in cmd], cwd=cwd, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=log, bufsize=0)


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def require_file(path):
    p = Path(path).resolve()
    if not p.is_file():
        raise RuntimeError("Pinned executable missing: " + str(p))
    return str(p)


def test_tongsuo_strict_server(tongsuo, gmssl, cwd):
    port = free_port()
    args = [tongsuo, "s_server", "-accept", "127.0.0.1:" + str(port),
            "-cert", "server-cert.pem", "-key", "server-key-tongsuo.pem",
            "-tls1_3", "-ciphersuites", SUITE,
            "-groups", "SM2", "-enable_sm_tls13_strict",
            "-rev", "-naccept", "1"]
    log = Path(cwd, "tongsuo-server.log")
    with log.open("wb") as out:
        server = launch(args, cwd, out)
        try:
            # s_server -naccept 1: probing its socket would consume admission.
            time.sleep(1)
            if server.poll() is not None:
                raise RuntimeError("Tongsuo strict server exited: " +
                                   log.read_text(errors="replace")[-2000:])
            Path(cwd, "message.txt").write_bytes(MESSAGE)
            client = subprocess.run([
                gmssl, "tls13_client", "-host", "127.0.0.1",
                "-port", str(port), "-server_name", "localhost",
                "-cacert", "root-cert.pem", "-cipher_suite", SUITE,
                "-supported_group", "sm2p256v1", "-sig_alg", "sm2sig_sm3",
                "-in", "message.txt"
            ], cwd=cwd, capture_output=True, timeout=20, check=False)
            # -rev reverses each line of plaintext; authenticated application
            # traffic and correct SM2 trust are required for this observation.
            response = client.stdout
            if client.returncode != 0 or MESSAGE.rstrip(b"\n")[::-1] not in response:
                raise RuntimeError(
                    "GmSSL client ↔ Tongsuo strict server interop failed: " +
                    client.stderr.decode(errors="replace")[-2000:] +
                    " | response=" + repr(response[-250:]))
            if b"Connection established" not in client.stderr:
                raise RuntimeError("GmSSL client did not confirm TLS handshake")
            print("Pinned GmSSL client -> Tongsuo strict SM2 TLS1.3 server: PASS")
        finally:
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


def test_tongsuo_strict_client(tongsuo, gmssl, cwd):
    port = free_port()
    server_cmd = [gmssl, "tls13_server", "-port", str(port),
                  "-cert", "server-cert.pem", "-key", "server-key.pem",
                  "-pass", PASS, "-cipher_suite", SUITE,
                  "-supported_group", "sm2p256v1", "-sig_alg", "sm2sig_sm3"]
    server_log = Path(cwd, "gmssl-server.log")
    with server_log.open("wb") as out:
        server = launch(server_cmd, cwd, out)
        try:
            time.sleep(1)
            if server.poll() is not None:
                raise RuntimeError("GmSSL strict server exited early")
            client_log = Path(cwd, "tongsuo-client.log")
            cmd = [tongsuo, "s_client", "-connect", "127.0.0.1:" + str(port),
                   "-servername", "localhost", "-CAfile", "root-cert.pem",
                   "-verify_return_error", "-verify_hostname", "localhost",
                   "-tls1_3", "-ciphersuites", SUITE, "-groups", "SM2",
                   "-enable_sm_tls13_strict", "-brief"]
            with client_log.open("wb") as err:
                client = launch(cmd, cwd, err)
                try:
                    if client.stdin is None or client.stdout is None:
                        raise RuntimeError("Tongsuo client stdio unavailable")
                    client.stdin.write(MESSAGE)
                    client.stdin.flush()
                    # Do not close stdin before receiving the echo: s_client
                    # might otherwise initiate an early TLS shutdown.
                    selector = selectors.DefaultSelector()
                    selector.register(client.stdout, selectors.EVENT_READ)
                    received = b""
                    deadline = time.monotonic() + 12
                    while time.monotonic() < deadline and MESSAGE not in received:
                        if client.poll() is not None:
                            break
                        for key, _ in selector.select(timeout=0.2):
                            received += key.fileobj.read(4096)
                    selector.close()
                    result_log = client_log.read_text(errors="replace")
                    if MESSAGE not in received:
                        raise RuntimeError(
                            "Tongsuo strict client did not get GmSSL TLS echo: "
                            + result_log[-2500:] + " | data=" + repr(received[-200:]))
                    if not ("TLSv1.3" in result_log and SUITE in result_log):
                        raise RuntimeError(
                            "Tongsuo strict client did not confirm exact version/suite: "
                            + result_log[-2500:])
                    if "Verification: OK" not in result_log:
                        raise RuntimeError(
                            "Tongsuo strict peer verification unconfirmed: "
                            + result_log[-2500:])
                    print("Tongsuo strict SM2 TLS1.3 client -> pinned GmSSL server: PASS")
                finally:
                    if client.poll() is None:
                        client.terminate()
                    try:
                        client.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        client.kill()
                        client.wait(timeout=3)
                    if client.stdin:
                        client.stdin.close()
                    if client.stdout:
                        client.stdout.close()
        finally:
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tongsuo", required=True)
    parser.add_argument("--gmssl", required=True)
    parser.add_argument("--fixture-key-converter", required=True)
    args = parser.parse_args()
    tong = require_file(args.tongsuo)
    gmssl = require_file(args.gmssl)
    with tempfile.TemporaryDirectory(prefix="tongsuo-rfc8998-") as cwd:
        fixture(gmssl, cwd)
        converter = require_file(args.fixture_key_converter)
        unwrap = subprocess.run(
            [converter, "server-key.pem", PASS, "server-key-tongsuo.pem"],
            cwd=cwd, capture_output=True, timeout=10, check=False)
        if unwrap.returncode:
            raise RuntimeError(
                "ephemeral GmSSL PKCS8 fixture conversion failed: "
                + unwrap.stderr.decode(errors="replace")[-1500:])
        Path(cwd, "server-key-tongsuo.pem").chmod(0o600)
        test_tongsuo_strict_server(tong, gmssl, cwd)
        test_tongsuo_strict_client(tong, gmssl, cwd)
    print("Independent strict RFC8998 provider interop preflight: PASS")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
        print("Independent RFC8998 provider interop FAILED: " + str(exc),
              file=sys.stderr)
        sys.exit(1)
