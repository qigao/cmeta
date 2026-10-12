#!/usr/bin/env python3
"""Fail-closed *preflight*, NOT a NIC-DMA certificate or performance result.

This module inspects kernel-reported PCI/driver/IOMMU state and requires an
explicit acknowledgement of exclusive NIC control. Unit tests inject a fake
sysfs tree, but the real CLI always evaluates /sys on the executing runner.
No automatic vfio binding, device reset, privilege escalation or hugepage
modification is performed.
"""
import argparse
import json
import os
import platform
import re
import sys
from pathlib import Path

APPROVAL = "I_ACKNOWLEDGE_DEDICATED_OFFLINE_NICS"
BDF_PATTERN = re.compile(r"^[0-9a-f]{4}:[0-9a-f]{2}:[01][0-9a-f]\.[0-7]$")


class PreflightError(Exception):
    pass


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="ascii").strip()
    except (FileNotFoundError, PermissionError, OSError) as error:
        raise PreflightError(f"required sysfs metadata unreadable: {path}: {error}") from error


def inspect_nic(root: Path, bdf: str) -> dict:
    bdf = bdf.lower()
    if not BDF_PATTERN.fullmatch(bdf):
        raise PreflightError(f"invalid canonical PCI BDF: {bdf!r}")
    path = root / "bus" / "pci" / "devices" / bdf
    if not path.is_dir():
        raise PreflightError(f"PCI endpoint does not exist: {bdf}")
    driver = path / "driver"
    if not driver.is_symlink() or driver.resolve().name != "vfio-pci":
        raise PreflightError(f"{bdf}: must already be bound to vfio-pci (never bind automatically)")
    group = path / "iommu_group"
    if not group.is_symlink() or not group.resolve().name.isdigit():
        raise PreflightError(f"{bdf}: no independent IOMMU group; DMA isolation unknown")
    if (path / "physfn").exists():
        raise PreflightError(f"{bdf}: SR-IOV VF not admitted in physical-PF lab profile")
    cls = _read(path / "class").lower()
    if not cls.startswith("0x0200"):
        raise PreflightError(f"{bdf}: class {cls} is not PCI Ethernet")
    if (path / "net").is_dir() and any((path / "net").iterdir()):
        raise PreflightError(f"{bdf}: still exposes a kernel network interface")
    vendor = _read(path / "vendor").lower()
    device = _read(path / "device").lower()
    numa = _read(path / "numa_node")
    return {
        "bdf": bdf,
        "driver": "vfio-pci",
        "iommu_group": int(group.resolve().name),
        "class": cls,
        "vendor": vendor,
        "device": device,
        "numa_node": numa,
    }


def inspect_hugepages(root: Path) -> dict:
    prefix = root / "kernel" / "mm" / "hugepages"
    total_free_bytes = 0
    kinds = {}
    if prefix.is_dir():
        for directory in prefix.glob("hugepages-*kB"):
            try:
                size_kb = int(directory.name.split("-")[1].removesuffix("kB"))
                count = int(_read(directory / "free_hugepages"))
            except (ValueError, IndexError) as error:
                raise PreflightError(f"invalid hugepage metadata: {directory}") from error
            if count < 0:
                raise PreflightError("negative free hugepage count")
            kinds[directory.name] = count
            total_free_bytes += size_kb * 1024 * count
    if total_free_bytes < 64 * 1024 * 1024:
        raise PreflightError("at least 64 MiB of FREE hugepages required for the dedicated VFIO lab")
    return {"free_bytes": total_free_bytes, "page_counts": kinds}


def qualify(root: Path, tx: str, rx: str, lcore: int, approval: str,
            allowed_cores: set[int]) -> dict:
    if approval != APPROVAL:
        raise PreflightError("exclusive-NIC acknowledgement missing or incorrect")
    if tx.lower() == rx.lower():
        raise PreflightError("two DISTINCT physically connected Ethernet PFs are required")
    if lcore not in allowed_cores:
        raise PreflightError(f"requested EAL lcore {lcore} not in host CPU affinity")
    tx_info = inspect_nic(root, tx)
    rx_info = inspect_nic(root, rx)
    huge = inspect_hugepages(root)
    return {
        "attestation_type": "PRECHECK_ONLY_NOT_NIC_DMA_PROOF",
        "exclusive_network_disruption_ack": True,
        "tx": tx_info,
        "rx": rx_info,
        "lcore": lcore,
        "hugepages": huge,
        "os": platform.system(),
        "kernel": platform.release(),
        "runner_source_sha": os.environ.get("GITHUB_SHA", "manual-unattested"),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tx-bdf", required=True)
    parser.add_argument("--rx-bdf", required=True)
    parser.add_argument("--lcore", type=int, required=True)
    parser.add_argument("--ack", required=True)
    parser.add_argument("--out", required=True, help="JSON PRECHECK, not a hardware success record")
    args = parser.parse_args()
    try:
        report = qualify(Path("/sys"), args.tx_bdf, args.rx_bdf, args.lcore,
                         args.ack, set(os.sched_getaffinity(0)))
        dest = Path(args.out)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print("PRECHECK PASSED: dedicated vfio-pci Ethernet endpoints and host metadata only")
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    except (PreflightError, OSError, ValueError) as error:
        print(f"NIC HARDWARE PRECHECK FAILED CLOSED: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
