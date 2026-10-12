#!/usr/bin/env python3
"""No-hardware, fail-closed contract tests for the real-NIC preflight."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "hardware"))
from preflight import APPROVAL, PreflightError, qualify  # noqa: E402

TX = "0000:81:00.0"
RX = "0000:82:00.0"


class PreflightContractTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        driver = self.root / "bus/pci/drivers/vfio-pci"
        driver.mkdir(parents=True)
        group = self.root / "kernel/iommu_groups/9"
        group.mkdir(parents=True)
        for name in (TX, RX):
            device = self.root / "bus/pci/devices" / name
            device.mkdir(parents=True)
            (device / "driver").symlink_to(driver, target_is_directory=True)
            (device / "iommu_group").symlink_to(group, target_is_directory=True)
            (device / "class").write_text("0x020000\n")
            (device / "vendor").write_text("0x8086\n")
            (device / "device").write_text("0x100e\n")
            (device / "numa_node").write_text("0\n")
        huge = self.root / "kernel/mm/hugepages/hugepages-2048kB"
        huge.mkdir(parents=True)
        (huge / "free_hugepages").write_text("64\n")

    def check(self, **kwargs):
        data = dict(root=self.root, tx=TX, rx=RX, lcore=3, approval=APPROVAL,
                    allowed_cores={2, 3, 4})
        data.update(kwargs)
        return qualify(**data)

    def test_positive_fixture_is_only_precheck(self):
        report = self.check()
        self.assertEqual(report["attestation_type"], "PRECHECK_ONLY_NOT_NIC_DMA_PROOF")
        self.assertEqual(report["tx"]["driver"], "vfio-pci")
        self.assertNotIn("latency", report)
        self.assertNotIn("dma_qualified", report)

    def test_acknowledgement_is_mandatory(self):
        with self.assertRaises(PreflightError):
            self.check(approval="YES")

    def test_two_real_endpoints_required(self):
        with self.assertRaises(PreflightError):
            self.check(rx=TX)

    def test_invalid_bdf_does_not_browse_sysfs(self):
        with self.assertRaises(PreflightError):
            self.check(tx="../../../../etc/passwd")

    def test_wrong_cpu_affinity(self):
        with self.assertRaises(PreflightError):
            self.check(lcore=0)

    def test_kernel_owned_port_rejected(self):
        port = self.root / "bus/pci/devices" / RX
        (port / "driver").unlink()
        kernel = self.root / "bus/pci/drivers/ixgbe"
        kernel.mkdir()
        (port / "driver").symlink_to(kernel, target_is_directory=True)
        with self.assertRaises(PreflightError):
            self.check()

    def test_iommu_is_hard_required(self):
        (self.root / "bus/pci/devices" / TX / "iommu_group").unlink()
        with self.assertRaises(PreflightError):
            self.check()

    def test_virtual_function_refused(self):
        (self.root / "bus/pci/devices" / TX / "physfn").write_text("vf\n")
        with self.assertRaises(PreflightError):
            self.check()

    def test_non_ethernet_refused(self):
        (self.root / "bus/pci/devices" / RX / "class").write_text("0x010000\n")
        with self.assertRaises(PreflightError):
            self.check()

    def test_no_free_hugepages_fails(self):
        (self.root / "kernel/mm/hugepages/hugepages-2048kB/free_hugepages").write_text("0\n")
        with self.assertRaises(PreflightError):
            self.check()


if __name__ == "__main__":
    unittest.main()
