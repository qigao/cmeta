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
            raise subprocess.CalledProcessError(22, ["curl"])
        with patch.object(pkg.subprocess, "run", side_effect=download), self.assertRaises(subprocess.CalledProcessError):
            pkg.Curl().download("https://api.github.com/test", self.nupkg)
        self.assertFalse(self.nupkg.exists())
        self.assertEqual(list(self.root.iterdir()), [])

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

    def test_missing_exact_cache_package_propagates_failure(self):
        class Feed:
            def package(self, *args):
                raise RuntimeError("404 missing exact package")
        config = self.root / "packages.config"
        config.write_text(f'<packages><package id="{IDENTITY}" version="{VERSION}" /></packages>')
        with self.assertRaisesRegex(RuntimeError, "404"):
            pkg.restore_cache(Feed(), config, self.root / "cache")
        self.assertFalse((self.root / "cache").exists())


if __name__ == "__main__":
    unittest.main()
