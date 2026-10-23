#!/usr/bin/env python3
"""Read GitHub Release assets and NuGet V3 packages using curl, never NuGet/Mono."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tempfile
from urllib.parse import quote, urlsplit
import xml.etree.ElementTree as ET
import zipfile


class Curl:
    def __init__(self):
        token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
        if not token or any(c in token for c in '\r\n"\\'):
            raise ValueError("a valid GH_TOKEN or GITHUB_TOKEN is required")
        self.token = token

    def download(self, url, destination, accept="application/json"):
        parsed = urlsplit(url)
        if (parsed.scheme != "https" or parsed.hostname not in
                {"api.github.com", "nuget.pkg.github.com"} or parsed.username or
                parsed.password or parsed.port not in (None, 443)):
            raise ValueError("untrusted GitHub download endpoint")
        if parsed.hostname == "nuget.pkg.github.com":
            encoded = base64.b64encode(f"qigao:{self.token}".encode()).decode()
            authorization = f"Basic {encoded}"
        else:
            authorization = f"Bearer {self.token}"
        destination = Path(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
            partial = Path(temporary) / "download"
            # Keep credentials off argv and disk. curl does not forward Authorization
            # to a different redirect host; do not use --location-trusted.
            config = f'header = "Authorization: {authorization}"\n'
            subprocess.run([
                "curl", "--disable", "--config", "-", "--fail", "--silent", "--show-error",
                "--location", "--proto", "=https", "--proto-redir", "=https",
                "--connect-timeout", "30", "--max-time", "900", "--retry", "3",
                "--header", f"Accept: {accept}", "--output", str(partial), url,
            ], input=config, text=True, check=True)
            partial.replace(destination)

    def json(self, url):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "response.json"
            self.download(url, path)
            return json.loads(path.read_text(encoding="utf-8"))


def sha256(path):
    with Path(path).open("rb") as source:
        digest = hashlib.sha256()
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
        return digest.hexdigest()


def release_asset(client, repository, tag, asset, destination):
    if not re.fullmatch(r"(?:qigao/[A-Za-z0-9_.-]+|PowerShell/PowerShell)", repository):
        raise ValueError("invalid release repository")
    if not re.fullmatch(r"v\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?", tag):
        raise ValueError("an exact release tag is required")
    release = client.json(f"https://api.github.com/repos/{repository}/releases/tags/{quote(tag)}")
    if release.get("draft") is not False or not release.get("published_at") or release.get("tag_name") != tag:
        raise ValueError("release is unpublished or has a different tag")
    matches = [item for item in release.get("assets", []) if item.get("name") == asset]
    if len(matches) != 1:
        raise ValueError(f"missing or ambiguous release asset: {repository}@{tag}/{asset}")
    item = matches[0]
    client.download(item["url"], destination, "application/octet-stream")
    digest = item.get("digest")
    if digest and digest != "sha256:" + sha256(destination):
        Path(destination).unlink()
        raise ValueError("GitHub Release asset digest mismatch")


def package_metadata(archive, package_id, version):
    specifications = [name for name in archive.namelist() if "/" not in name and name.lower().endswith(".nuspec")]
    if len(specifications) != 1:
        raise ValueError("package must contain one root nuspec")
    root = ET.fromstring(archive.read(specifications[0]))
    metadata = next((e for e in root if e.tag.rsplit("}", 1)[-1] == "metadata"), None)
    values = {} if metadata is None else {e.tag.rsplit("}", 1)[-1]: e.text for e in metadata}
    if (values.get("id", "").lower() != package_id.lower() or
            values.get("version", "").lower() != version.lower()):
        raise ValueError(f"NuGet identity mismatch: expected {package_id}/{version}")


def archive_members(archive):
    seen = set()
    for entry in archive.infolist():
        name = entry.filename
        path = PurePosixPath(name)
        if ("\\" in name or ":" in name or path.is_absolute() or
                any(p in ("", ".", "..") for p in name.rstrip("/").split("/")) or
                name.casefold().rstrip("/") in seen):
            raise ValueError(f"unsafe or duplicate archive entry: {name}")
        seen.add(name.casefold().rstrip("/"))
        if stat.S_ISLNK(entry.external_attr >> 16):
            raise ValueError(f"symlink archive entry is unsupported: {name}")
        yield entry


class NugetFeed:
    def __init__(self, client):
        self.client = client
        resources = client.json("https://nuget.pkg.github.com/qigao/index.json")["resources"]
        addresses = [r["@id"] for r in resources if r.get("@type") == "PackageBaseAddress/3.0.0"]
        if len(addresses) != 1:
            raise ValueError("GitHub feed must advertise one PackageBaseAddress/3.0.0")
        self.base = addresses[0].rstrip("/")

    def package(self, package_id, version, destination):
        for value in (package_id, version):
            if not re.fullmatch(r"[A-Za-z0-9_.+-]+", value):
                raise ValueError("invalid NuGet identity")
        identity, selected = package_id.lower(), version.lower()
        url = f"{self.base}/{identity}/{selected}/{identity}.{selected}.nupkg"
        self.client.download(url, destination, "application/octet-stream")
        with zipfile.ZipFile(destination) as archive:
            list(archive_members(archive))
            package_metadata(archive, package_id, version)
        print(f"package={package_id} version={version} sha256={sha256(destination)}", flush=True)

    def latest_stable(self, package_id):
        versions = self.client.json(f"{self.base}/{package_id.lower()}/index.json")["versions"]
        stable = [v for v in versions if re.fullmatch(r"\d+\.\d+\.\d+(?:\.\d+)?", v)]
        if not stable:
            raise ValueError("no stable re2c binary package is published")
        return max(stable, key=lambda v: tuple(int(n) for n in v.split(".")) + (0,) * (4-len(v.split("."))))


def cache_package(nupkg, package_id, version, cache):
    match = re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+-vcpkg([0-9a-f]{64})", version)
    if not match:
        raise ValueError("expected vcpkg-generated version with exact ABI")
    abi = match[1]
    destination = Path(cache) / abi[:2] / f"{abi}.zip"
    with zipfile.ZipFile(nupkg) as source:
        members = list(archive_members(source))
        package_metadata(source, package_id, version)
        core = source.read("CONTROL").decode().split("\n\n", 1)[0]
        fields = dict(line.split(": ", 1) for line in core.splitlines() if ": " in line and not line.startswith(" "))
        identity = fields.get("Package", "") + "_" + fields.get("Architecture", "")
        if fields.get("Abi") != abi or package_id not in (identity, "vcpkg-cache_" + identity):
            raise ValueError("vcpkg CONTROL identity/ABI mismatch")
        if "BUILD_INFO" not in source.namelist():
            raise ValueError("vcpkg package lacks BUILD_INFO")
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=destination.parent) as temporary:
            output = Path(temporary) / "payload.zip"
            with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as target:
                for entry in members:
                    name = entry.filename.lower()
                    if (name == "[content_types].xml" or name == ".signature.p7s" or
                            name.endswith(".nuspec") and "/" not in name or
                            name.startswith(("_rels/", "package/"))):
                        continue
                    with source.open(entry) as data, target.open(entry, "w") as sink:
                        shutil.copyfileobj(data, sink)
            output.replace(destination)
    return destination


def extract_package(nupkg, destination):
    with zipfile.ZipFile(nupkg) as archive:
        members = list(archive_members(archive))
        for entry in members:
            archive.extract(entry, destination)
            mode = entry.external_attr >> 16
            if mode and not entry.is_dir():
                (Path(destination) / entry.filename).chmod(mode & 0o777)


def restore_cache(feed, config, cache):
    root = ET.parse(config).getroot()
    if root.tag != "packages":
        raise ValueError("invalid vcpkg packages.config")
    with tempfile.TemporaryDirectory() as temporary:
        for item in root:
            if item.tag != "package":
                raise ValueError("unexpected packages.config element")
            package_id, version = item.attrib["id"], item.attrib["version"]
            path = Path(temporary) / "package.nupkg"
            feed.package(package_id, version, path)
            cache_package(path, package_id, version, cache)


def restore_re2c(feed, destination):
    version = feed.latest_stable("Qigao.Re2c.Binary")
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    nupkg = destination / "re2c.nupkg"
    feed.package("Qigao.Re2c.Binary", version, nupkg)
    extract_package(nupkg, destination / "package")
    hosts = {("Linux", "X64"): "linux-x64", ("Linux", "ARM64"): "linux-arm64",
             ("macOS", "X64"): "macos-x64", ("macOS", "ARM64"): "macos-arm64",
             ("Windows", "X64"): "windows-x64"}
    rid = hosts[(os.environ["RUNNER_OS"], os.environ["RUNNER_ARCH"])]
    root = destination / "package" / "tools" / rid
    exe = root / "bin" / ("re2c.exe" if os.name == "nt" else "re2c")
    for required in (exe, root / "share/re2c/stdlib/unicode_categories.re", root / "share/re2c/stdlib/unicode_properties.re"):
        if not required.is_file():
            raise ValueError(f"missing re2c package file: {required}")
    exe.chmod(exe.stat().st_mode | 0o111)
    subprocess.run([str(exe), "--version"], check=True)
    with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as output:
        output.write(f"RE2C_ROOT={root.as_posix()}\nRE2C_PACKAGE_VERSION={version}\n")
    with open(os.environ["GITHUB_PATH"], "a", encoding="utf-8") as output:
        output.write(f"{(root / 'bin').as_posix()}\n")


def restore_windows_powershell(client, destination):
    # Shared cache warm run 38989476938 used runner image 20261007.264.1.
    # vcpkg includes PowerShell's version in every Windows port ABI; the newer
    # hosted image's 7.6.7 cannot consume the packages built with 7.6.6.
    version = "7.6.6"
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / "powershell.zip"
    release_asset(client, "PowerShell/PowerShell", f"v{version}",
                  f"PowerShell-{version}-win-x64.zip", archive)
    expected_digest = "02fe458be20493fbdf43f61ea20610b811ee6c738ab1676c61b9cfcd1a33c860"
    if sha256(archive) != expected_digest:
        raise ValueError("pinned Windows cache PowerShell archive digest mismatch")
    extract_package(archive, destination / "package")
    root = destination / "package"
    actual = subprocess.check_output([str(root / "pwsh.exe"), "-NoLogo", "-NoProfile",
                                      "-Command", "$PSVersionTable.PSVersion.ToString()"], text=True).strip()
    if actual != version:
        raise ValueError("Windows cache PowerShell version mismatch")
    with open(os.environ["GITHUB_PATH"], "a", encoding="utf-8") as output:
        output.write(root.as_posix() + "\n")
    print(f"Windows cache PowerShell={version} sha256={sha256(archive)}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    release = commands.add_parser("release")
    for name in ("repository", "tag", "asset", "destination"):
        release.add_argument(name)
    cache = commands.add_parser("cache")
    cache.add_argument("config")
    cache.add_argument("destination")
    re2c = commands.add_parser("re2c")
    re2c.add_argument("destination")
    powershell = commands.add_parser("windows-powershell")
    powershell.add_argument("destination")
    args = parser.parse_args()
    client = Curl()
    if args.command == "release":
        release_asset(client, args.repository, args.tag, args.asset, args.destination)
    elif args.command == "cache":
        restore_cache(NugetFeed(client), args.config, args.destination)
    elif args.command == "re2c":
        restore_re2c(NugetFeed(client), args.destination)
    else:
        restore_windows_powershell(client, args.destination)


if __name__ == "__main__":
    main()
