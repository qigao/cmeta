# Physical NIC qualification: gated self-hosted lab only

**DEFERRED / NOT AUTHORIZED TO RUN (2026-10-12):** the software-only closeout has explicitly paused physical NIC tests. The hardware workflow contains a literal `false &&` job guard; applying any label, even `r2-hardware-nic-approved`, **cannot start the NIC job**. Do not remove the guard unless the user explicitly asks to resume hardware qualification and a qualified offline runner has been approved. See [SOFTWARE_CLOSEOUT.md](SOFTWARE_CLOSEOUT.md).

**Tracking:** [#1146](https://github.com/qigao/salts/issues/1146), [#1152](https://github.com/qigao/salts/issues/1152), [#1148](https://github.com/qigao/salts/issues/1148). This is explicitly **not** part of default Salts CI or its installed ABI. [Research PR #1149](https://github.com/qigao/salts/pull/1149) remains Draft / DO NOT MERGE.

## Evidence tiers that must not be conflated

| Tier | Evidence | Current qualification |
| --- | --- | --- |
| A | Fake PMD and R2 Owner/Generation/sanitizer tests | Completed on ordinary GitHub Linux CI |
| B | Real DPDK `net_ring` virtual ethdev | Completed on ordinary GitHub Linux CI; **NO physical DMA** |
| C | Two explicitly cabled physical PCI Ethernet PFs with real DPDK TX/RX and PMD `tx_done_cleanup` | **Hardware-only**, not run by ordinary CI |
| D | Real driver's safe port removal/reset, DSO unload, off-Owner callbacks and IOMMU lifetime | **Open even if C passes** |
| E | Matched R2-vs-CNet NativeIO UDP/KCP p50/p95/p99.9, throughput and dedicated CPU cost | **Open until independent fair hardware runs** |

CI must never mark tier C successful merely because the *hardware probe executable compiled*.

## Operator approval / strict hardware preflight

A dedicated, **offline** Linux x64 NIC lab with two different, directly cabled physical **PCI Ethernet PFs** is required. They must have already been bound to `vfio-pci` under separate or documented IOMMU group assignment. No active kernel interfaces, normal production traffic, packet capture secrets, shared hypervisor NIC, `net_ring`/vdev shortcut or unqualified SR-IOV VF is allowed.

The isolated workflow `.github/workflows/cnet-r2-hardware-nic.yml` has ONLY the `pull_request.labeled` trigger, but its physical job is **hard-disabled** for this software-only closeout. In the *future*, after separate explicit reauthorization and removal of the `false` guard, it must still require Draft PR #1149, head branch `research/cnet-dpdk-packet-plane` and a deliberate `r2-hardware-nic-approved` label. It needs a dedicated self-hosted runner carrying all labels: `self-hosted`, `linux`, `x64`, `dpdk-nic-lab`.

Before manually approving, configure repository Actions **variables** with:
- `DPDK_NIC_LAB_TX_BDF`: exact canonical PCI BDF of the first **offline** PF, e.g. `0000:81:00.0`.
- `DPDK_NIC_LAB_RX_BDF`: the distinct physically cabled peer PF's BDF, e.g. `0000:82:00.0`.
- `DPDK_NIC_LAB_LCORE`: a CPU in the dedicated runner's affinity set.
- `DPDK_NIC_LAB_ACK`: the exact explicit acknowledgement `I_ACKNOWLEDGE_DEDICATED_OFFLINE_NICS`.

Examples are documentation placeholders; **never assume these PCI addresses correspond to real hardware**. A NIC is a privileged, disruptive resource; the workflow never silently unbinds kernel drivers, changes hugepages, elevates privileges, updates a release, changes production network config or auto-installs DPDK. Operators must manually provision vfio/IOMMU, ensure at least 64 MiB of *free* hugepages, provide a valid system `pkg-config libdpdk`, ensure NIC queues and link are supported and prevent other apps touching these ports. Removing and re-adding the label constitutes a separate new manual test request.

The Python preflight validates the BDF pattern, PCI Ethernet class, existing `vfio-pci` driver binding, actual IOMMU-group sysfs symlink, PF identity, separate ports, lcore affinity and free hugepages. Failing ANY check aborts without opening the NIC. This is a host-state observation **only**. The preflight's negative tests run in ordinary CI against a synthetic sysfs fixture and **do not** qualify real hardware.

## Physical probe — what is tested (if lab workflow runs)

`packet_io_dpdk_physical_test` (compiled whenever `PACKET_IO_ENABLE_DPDK=ON`, but **never executed by standard GitHub-hosted CI**) starts DPDK EAL with an explicit `-a` allow-list of exactly the two pre-approved BDFs and refuses an unexpected third device. It requires real PCI ethdev names, configures one RX/TX queue on both ports, demands both hardware links UP, then sends 16 uniquely sequenced EtherType `0x88B5` test Ethernet frames in **each direction**. The receiver must get exactly the expected nonce, sequence, payload and destination MAC once.

The transmitter keeps an additional mbuf *observer reference* separate from the reference passed to `rte_eth_tx_burst()`. Driver admission **transfers only the driver's mbuf ownership**, never protocol reliability ACK or on-wire confirmation. A mandatory, supported `rte_eth_tx_done_cleanup()` is polled by the same DPDK queue Owner. A successful cleanup check requires the driver to have dropped its TX reference: the mbuf refcount falls from 2 to **1 (observer only)** for every accepted packet. Only then may the Owner free its observer. PMDs that return `-ENOTSUP`, removed-device/port error, packet loss, unexpected duplicate/corruption, link down, reclaim timeout or mempool mismatch **FAIL**, not SKIP. This follows the documented ethdev semantics: [DPDK 23.11 ethdev API](https://doc.dpdk.org/api-23.11/rte__ethdev_8h.html).

The probe uses a **bounded 16-packet control workload**, not calibrated performance benchmarking, and it does not use `rte_eth_tx_burst()` as an ACK oracle. It checks port stop/close and allocated pool restoration before reporting success. The workflow creates raw `precheck.json`, build metadata and a probe log with source SHA/checksums, uploaded as short-retention evidence. A missing device or runner is **not** evidence of a test pass.

## Still missing after a successful physical smoke

- Specific PMD/driver firmware teardown and IOVA/IOMMU safety under device hot removal/reset, TX cleanup concurrent with shutdown and foreign off-Owner notifications.
- A **real hardware origin** for the cross-thread completion tickets in `dma_completion_gate`. Its prior tests deliberately used fake completion producers; no driver IRQ or vendor asynchronous callback has been validated.
- A production R2 Ethernet frame adapter with secure credentialed peer admission/key refresh. The hardware smoke sends **only unauthenticated test EtherType 0x88B5 frames** and must never be reported as secure R2.
- Two-machine or matched NIC setup comparing CNet/NativeIO UDP vs CNet KCP vs authenticated R2, accounting for all data/ACK sizes, batching, polling core, queue topology and payload/window controls. See #1148 for JSON/CSV, p50/p95/p99/p99.9, Mpps/Gbps, cycles/byte, queue and shutdown metrics.
- A separately explicit go/no-go decision and release/ABI review. No branch merge or silent product migration.

### Why stop/detach is particularly sensitive

`rte_eth_tx_done_cleanup()` is a request to free driver-held mbufs **only after they are no longer used**, and may be unsupported. A driver can free a cached TX mbuf after ordinary TX bursts; without the caller's extra observer reference there is no safe object to examine afterward. The probe's observer lifetime verifies this specific PMD reclamation pathway on the tested hardware. It **does not** prove generic device-reset safety, DMA isolation under IOMMU removal or that a remote application received authenticated R2 messages.

A driver that does not implement an observable safe reclamation pathway is **unsupported by this research hardware gate**, rather than handled by speculative completion callbacks or a fallback transport.
