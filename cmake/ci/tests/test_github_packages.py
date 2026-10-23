"""Offline admission tests for the curl transport and vcpkg cache conversion."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location("github_packages", Path(__file__).resolve().parents[1] / "github-packages.py")
pkg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pkg)
ABI = "a" * 64
IDENTITY = "sqlite3_x64-linux"
VERSION = "3.49.0-vcpkg" + ABI


def package(path, identity=IDENTITY, version=VERSION, abi=ABI, extra=()):
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("cache.nuspec", f'<package xmlns="http://schemas.microsoft.com/packaging/2013/05/nuspec.xsd"><metadata><id>{identity}</id><version>{version}</version></metadata></package>')
        archive.writestr("CONTROL", f"Package: sqlite3\nVersion: 3.49.0\nArchitecture: x64-linux\nAbi: {abi}\n\nPackage: sqlite3\nFeature: fts5\nArchitecture: x64-linux\n")
        archive.writestr("BUILD_INFO", "CRTLinkage: dynamic\nLibraryLinkage: static\n")
        archive.writestr("lib/libsqlite3.a", b"actual binary payload")
        archive.writestr("[Content_Types].xml", "envelope")
        archive.writestr("_rels/.rels", "envelope")
        archive.writestr("package/services/metadata/core-properties/test.psmdcp", "envelope")
        for name, data in extra:
            archive.writestr(name, data)


class TransportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.nupkg = self.root / "test.nupkg"

    def test_cache_restores_executable_mode_from_binary_payload(self):
        package(self.nupkg, extra=[("tools/sqlite3/sqlite3", b"\x7fELF" + b"x" * 12)])
        result = pkg.cache_package(self.nupkg, IDENTITY, VERSION, self.root / "cache")
        with zipfile.ZipFile(result) as archive:
            entry = archive.getinfo("tools/sqlite3/sqlite3")
            self.assertTrue((entry.external_attr >> 16) & 0o111)

    def test_cache_keeps_binary_payload_and_removes_nuget_envelope(self):
        package(self.nupkg)
        result = pkg.cache_package(self.nupkg, IDENTITY, VERSION, self.root / "cache")
        self.assertEqual(result.relative_to(self.root / "cache").as_posix(), "aa/" + ABI + ".zip")
        with zipfile.ZipFile(result) as archive:
            self.assertEqual(set(archive.namelist()), {"CONTROL", "BUILD_INFO", "lib/libsqlite3.a"})
            self.assertEqual(archive.read("lib/libsqlite3.a"), b"actual binary payload")

    def test_rejects_wrong_nuspec_identity_version_or_control_abi(self):
        for override in ({"identity": "other"}, {"version": "0.0.0"}, {"abi": "b" * 64}):
            with self.subTest(override=override):
                package(self.nupkg, **override)
                with self.assertRaises(ValueError):
                    pkg.cache_package(self.nupkg, IDENTITY, VERSION, self.root / "cache")
                self.assertFalse((self.root / "cache").exists())

    @patch.dict(os.environ, {"X_VCPKG_NUGET_ID_PREFIX": "vcpkg-cache"})
    def test_cache_accepts_only_the_configured_prefixed_identity(self):
        prefixed = "vcpkg-cache_" + IDENTITY
        package(self.nupkg, identity=prefixed)
        result = pkg.cache_package(self.nupkg, prefixed, VERSION, self.root / "cache")
        self.assertTrue(result.is_file())

    def test_rejects_noncanonical_abi_reference(self):
        package(self.nupkg)
        with self.assertRaises(ValueError):
            pkg.cache_package(self.nupkg, IDENTITY, "latest", self.root / "cache")

    def test_rejects_traversal_absolute_windows_and_duplicate_entries(self):
        for name in ("../escape", "/escape", "C:/escape", "lib\\escape", "lib/./escape", "LIB/LIBSQLITE3.A"):
            with self.subTest(name=name):
                package(self.nupkg, extra=[(name, b"bad")])
                with self.assertRaises(ValueError):
                    pkg.cache_package(self.nupkg, IDENTITY, VERSION, self.root / "cache")
                self.assertFalse((self.root / "cache").exists())

    def test_rejects_symlink_before_extraction(self):
        package(self.nupkg)
        with zipfile.ZipFile(self.nupkg, "a") as archive:
            link = zipfile.ZipInfo("link")
            link.create_system = 3
            link.external_attr = 0o120777 << 16
            archive.writestr(link, "../escape")
        with self.assertRaises(ValueError):
            pkg.extract_package(self.nupkg, self.root / "output")
        self.assertFalse((self.root / "output").exists())

    def test_release_requires_published_exact_tag_and_asset(self):
        class Client:
            def json(self, url):
                return self.release
            def download(self, *args):
                raise AssertionError("invalid release must never download")
        client = Client()
        valid = {"draft": False, "published_at": "2026-10-23", "tag_name": "v1.0.0", "assets": []}
        for override in ({"draft": True}, {"published_at": None}, {"tag_name": "v1.0.1"}, {}):
            client.release = valid | override
            with self.subTest(override=override), self.assertRaises(ValueError):
                pkg.release_asset(client, "qigao/salts", "v1.0.0", "Salts.Native.1.0.0.nupkg", self.nupkg)

    def test_release_digest_is_checked(self):
        class Client:
            def json(self, url):
                return {"draft": False, "published_at": "today", "tag_name": "v1.0.0", "assets": [{"name": "asset", "url": "https://api.github.com/repos/qigao/salts/releases/assets/1", "digest": "sha256:" + "0" * 64}]}
            def download(self, url, destination, accept):
                Path(destination).write_bytes(b"wrong content")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            pkg.release_asset(Client(), "qigao/salts", "v1.0.0", "asset", self.nupkg)
        self.assertFalse(self.nupkg.exists())

    @patch.dict(os.environ, {"GH_TOKEN": "secret-test-token"})
    def test_curl_auth_is_stdin_only_and_redirects_are_https(self):
        def download(args, **kwargs):
            self.assertNotIn("secret-test-token", " ".join(args))
            self.assertNotIn("--location-trusted", args)
            self.assertIn("Authorization: Bearer secret-test-token", kwargs["input"])
            self.assertEqual(args[args.index("--proto-redir") + 1], "=https")
            Path(args[args.index("--output") + 1]).write_bytes(b"payload")
            return subprocess.CompletedProcess(args, 0, stdout="200", stderr="")
        with patch.object(pkg.subprocess, "run", side_effect=download):
            pkg.Curl().download("https://api.github.com/test", self.nupkg)
        self.assertEqual(self.nupkg.read_bytes(), b"payload")

    @patch.dict(os.environ, {"GH_TOKEN": "secret-test-token"})
    def test_rejects_untrusted_endpoint_without_invoking_curl(self):
        with patch.object(pkg.subprocess, "run") as run:
            for url in ("http://api.github.com/a", "https://evil.example/a", "https://api.github.com.evil/a", "https://user@api.github.com/a"):
                with self.subTest(url=url), self.assertRaises(ValueError):
                    pkg.Curl().download(url, self.nupkg)
            run.assert_not_called()

    @patch.dict(os.environ, {"GH_TOKEN": "secret-test-token"})
    def test_failed_curl_never_publishes_partial_download(self):
        def download(args, **kwargs):
            Path(args[args.index("--output") + 1]).write_bytes(b"partial")
            return subprocess.CompletedProcess(args, 22, stdout="500", stderr="server error")
        with patch.object(pkg.subprocess, "run", side_effect=download), self.assertRaises(subprocess.CalledProcessError):
            pkg.Curl().download("https://api.github.com/test", self.nupkg)
        self.assertFalse(self.nupkg.exists())
        self.assertEqual(list(self.root.iterdir()), [])

    def test_semver_floating_keeps_rc_sha_above_numeric_rc(self):
        numeric = pkg.nuget_semver_key("2.3.0-rc.99")
        sha = pkg.nuget_semver_key("2.3.0-rc.sha" + "a" * 40)
        stable = pkg.nuget_semver_key("2.3.0")
        self.assertGreater(sha, numeric)
        self.assertGreater(stable, sha)

    def test_floating_download_uses_feed_ordering_and_records_exact_version(self):
        selected = "2.3.0-rc.sha" + "b" * 40
        class Client:
            def __init__(inner):
                inner.downloaded = None
            def json(inner, url):
                if url.endswith("/index.json") and "salts.native" not in url:
                    return {"resources": [{"@id": "https://nuget.pkg.github.com/qigao/download",
                                           "@type": "PackageBaseAddress/3.0.0"}]}
                return {"versions": ["2.3.0-rc.18", selected, "2.2.0"]}
            def download(inner, url, destination, accept):
                inner.downloaded = url
                with zipfile.ZipFile(destination, "w") as archive:
                    archive.writestr(
                        "Salts.Native.nuspec",
                        f'<package><metadata><id>Salts.Native</id><version>{selected}</version></metadata></package>')
        client = Client()
        version_file = self.root / "version.txt"
        pkg.NugetFeed(client).floating(
            "Salts.Native", "2.3.0-*", self.nupkg, version_file)
        self.assertEqual(version_file.read_text().strip(), selected)
        self.assertTrue(client.downloaded.endswith(
            f"/salts.native/{selected}/salts.native.{selected}.nupkg"))

    def test_feed_uses_advertised_flat_container_and_exact_reference(self):
        class Client:
            def json(self, url):
                return {"resources": [{"@id": "https://nuget.pkg.github.com/qigao/download", "@type": "PackageBaseAddress/3.0.0"}]}
            def download(inner, url, destination, accept):
                self.assertEqual(url, f"https://nuget.pkg.github.com/qigao/download/{IDENTITY}/{VERSION}/{IDENTITY}.{VERSION}.nupkg")
                package(destination)
        pkg.NugetFeed(Client()).package(IDENTITY, VERSION, self.nupkg)

    def test_windows_powershell_requires_pinned_archive_digest_before_execution(self):
        def release(client, repository, tag, asset, destination):
            self.assertEqual((repository, tag, asset), ("PowerShell/PowerShell", "v7.6.6", "PowerShell-7.6.6-win-x64.zip"))
            Path(destination).write_bytes(b"wrong binary")
        with patch.object(pkg, "release_asset", side_effect=release), patch.object(pkg.subprocess, "check_output") as execute:
            with self.assertRaisesRegex(ValueError, "archive digest mismatch"):
                pkg.restore_windows_powershell(object(), self.root / "powershell")
            execute.assert_not_called()
        self.assertFalse((self.root / "powershell/package").exists())

    @patch.dict(os.environ, {"GH_TOKEN": "secret-test-token"})
    def test_exact_cache_404_is_a_cache_miss_but_401_is_fatal(self):
        def response(status, returncode):
            def run(args, **kwargs):
                partial = Path(args[args.index("--output") + 1])
                partial.write_bytes(b"error response")
                return subprocess.CompletedProcess(
                    args, returncode, stdout=status, stderr="http error")
            return run

        with patch.object(pkg.subprocess, "run", side_effect=response("404", 22)):
            self.assertFalse(pkg.Curl().download(
                "https://nuget.pkg.github.com/qigao/download/missing",
                self.nupkg, "application/octet-stream", allow_not_found=True))
            self.assertFalse(self.nupkg.exists())

        with patch.object(pkg.subprocess, "run", side_effect=response("401", 22)):
            with self.assertRaises(subprocess.CalledProcessError):
                pkg.Curl().download(
                    "https://nuget.pkg.github.com/qigao/download/private",
                    self.nupkg, "application/octet-stream", allow_not_found=True)
            self.assertFalse(self.nupkg.exists())

    def test_missing_exact_cache_package_is_not_materialized(self):
        class Feed:
            def package(self, *args, **kwargs):
                self.assertTrue(kwargs.get("allow_not_found"))
                return False
        feed = Feed()
        feed.assertTrue = self.assertTrue
        config = self.root / "packages.config"
        config.write_text(f'<packages><package id="{IDENTITY}" version="{VERSION}" /></packages>')
        pkg.restore_cache(feed, config, self.root / "cache")
        self.assertFalse((self.root / "cache").exists())


if __name__ == "__main__":
    unittest.main()
