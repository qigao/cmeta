"""Policy tests for the curl-only GitHub Packages binary-cache path."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[3]


class CurlCachePolicyTests(unittest.TestCase):
    def test_main_native_workflow_uses_local_curl_action(self):
        workflow = (ROOT / ".github/workflows/native-build.yml").read_text(encoding="utf-8")
        self.assertRegex(workflow, r"uses:\s+\./\.github/actions/setup-vcpkg-curl-cache")
        for forbidden in (
            "setup-vcpkg-cache@",
            "setup-re2c-tools@",
            "mono-complete",
            "brew install mono",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, workflow)
        self.assertNotRegex(workflow, r"(?i)dotnet\s+(?:restore|nuget)")

    def test_local_action_does_not_invoke_nuget_or_install_mono(self):
        action = (ROOT / ".github/actions/setup-vcpkg-curl-cache/action.yml").read_text(encoding="utf-8")
        self.assertIn("setup-vcpkg-curl-cache.py", action)
        self.assertIn("github-packages.py re2c", action)
        self.assertNotRegex(action, r"(?i)(mono-complete|brew install mono|dotnet\s+(?:restore|nuget)|setup-vcpkg-cache@)")

    def test_vcpkg_uses_only_local_read_only_files_provider(self):
        setup = (ROOT / "cmake/ci/setup-vcpkg-curl-cache.py").read_text(encoding="utf-8")
        self.assertRegex(setup, r'VCPKG_BINARY_SOURCES":\s*f"clear;files,\{cache\.as_posix\(\)\},read"')
        self.assertNotIn("nugetconfig,", setup)
        self.assertIn("cache-contract-revision.py", setup)
        self.assertIn("vcpkg-required-commits.py", setup)

    def test_restore_uses_exact_abi_refs_and_dry_run_preflight(self):
        restore = (ROOT / "cmake/ci/restore-vcpkg-cache.py").read_text(encoding="utf-8")
        self.assertIn("--dry-run;--x-write-nuget-packages-config=", restore)
        self.assertRegex(restore, r"vcpkg\[0-9a-f\]\{64\}")
        self.assertIn("validate_packages_config", restore)

    def test_transport_uses_curl_and_rejects_unsafe_or_incomplete_packages(self):
        transport = (ROOT / "cmake/ci/github-packages.py").read_text(encoding="utf-8")
        for required in (
            '"curl"', '"--fail"', '"--proto-redir"', "https://nuget.pkg.github.com/qigao/index.json",
            "package_metadata", "CONTROL", "BUILD_INFO", "sha256",
        ):
            with self.subTest(required=required):
                self.assertIn(required, transport)
        self.assertNotIn("--location-trusted", transport)
        self.assertNotRegex(transport, r"(?i)(mono-complete|dotnet\s+(?:restore|nuget))")


if __name__ == "__main__":
    unittest.main()
