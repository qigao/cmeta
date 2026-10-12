# CNet Packet I/O / R2 — Software Research Closeout

**Status as of 2026-10-12:** software-only research checkpoint accepted; **hardware qualification explicitly deferred** at user request. This is a **non-installed experiment, not a production release**.

**Integration:** [Draft PR #1149](https://github.com/qigao/salts/pull/1149) on `research/cnet-dpdk-packet-plane`, targeting `master`. **DO NOT MERGE** without a later explicit instruction. The source/release boundaries remain untouched: no modifications to production `cnet/src`, `cnet/include`, `native-io/src`, `native-io/include`, root CMake, or the Salts 2.3.0 installed ABI.

## Frozen software checkpoint and evidence

The last **functional** implementation baseline is commit [06be8fd](https://github.com/qigao/salts/commit/06be8fd7aacec981af99e5e532091fa7fcd22a46). Its independent [CNet Packet I/O Research CI #38243924327](https://github.com/qigao/salts/actions/runs/38243924327) finished **6/6 SUCCESS**; the [main Salts scope CI #38244478006](https://github.com/qigao/salts/actions/runs/38244478006) finished SUCCESS, with unrelated Native, formal, benchmark, package and publication jobs correctly skipped. A subsequent documentation/deferral commit must obtain its own CI result before referring to itself as qualified.

| Software research deliverable | What is actually proven | Tracker |
| --- | --- | --- |
| Bounded Packet I/O Owner / fake PMD | Fixed Owner, bounded capacity, partial bursts, move-owned RX/TX leases, Generation fences, explicit abort and drain | [#1146](https://github.com/qigao/salts/issues/1146) |
| Linux DPDK `rte_ethdev net_ring` | Real software PMD TX/RX and virtual-loopback mbuf reclaim, **not physical DMA** | [#1146](https://github.com/qigao/salts/issues/1146) |
| Authenticated R2 experimental reliable message engine | 1–8 fragments, 512-byte limit, SACK/ACK, bounded retry/deadline, cancel/exact logical terminal, mandatory HMAC-SHA256, no CRC-only fallback | [#1147](https://github.com/qigao/salts/issues/1147) |
| CMeta Crypto / GmSSL interoperability | Pinned GmSSL source check and **published Salts.Native 2.3.0-rc.14** C11/C++17 `Salts::Core/cmeta_hmac_sha256` ABI; exact NuGet digest and old 2.2 rejection | [#1151](https://github.com/qigao/salts/issues/1151) |
| Bounded R2 Session multiplexer | One Owner, up to eight peers, round-robin budget, per-peer HMAC/epoch isolation, 16×8 DATA/ACK cycles, two-descriptor fairness, loss/reorder/ACK replay oracles | [#1152](https://github.com/qigao/salts/issues/1152) |
| Off-Owner physical-completion *model* | Pre-reserved 32-ticket mailbox, bounded cross-thread post, duplicate/old-Generation rejection, Owner-only finalization, fairness on reused slots, sanitizer and TSan | [#1146](https://github.com/qigao/salts/issues/1146), [#1152](https://github.com/qigao/salts/issues/1152) |
| Physical PCI NIC *preparation* | Explicit offline VFIO/IOMMU/hugepage preflight with negative tests; 2-port DPDK test **builds**, only manual NIC runner may execute it | [#1148](https://github.com/qigao/salts/issues/1148) |

The latest code baseline ran GCC/Clang ASan+UBSan **6/6 CTests each**, GCC TSan **6/6**, pinned GmSSL **6/6**, the published Salts.Native rc.14 installed consumer **8/8**, and DPDK 23.11 `net_ring` success. The physical NIC executable was **compiled but not run**.

## Explicitly deferred, not quietly waived

**Do not run physical NIC tests during this software closeout.** A separate, dedicated self-hosted hardware lab with two already-offline PCI Ethernet PFs remains required. The existing test probe and [NIC_LAB.md](NIC_LAB.md) may be retained as future preparatory material, but the manual hardware workflow is intentionally **disabled** until the user explicitly reauthorizes this work. A label alone must not consume any NIC or self-hosted runner.

The following gates remain **OPEN** and must not inherit a software-green status:

1. **Physical PMD / DMA lifetime**: actual PCI NIC TX-reference reclaim, IOVA/IOMMU safety, driver cleanup, port stop/hot reset/remove and off-Owner notifications; a software `net_ring` and synthetic completion producer are insufficient.
2. **Trusted R2 identity and keys**: authenticated peer provisioning/handshake, non-test key rotation, replay after process restart, DSO hot-replacement lifetime and denial-of-service review. A keyed HMAC test is not complete transport security.
3. **Physical NIC comparison [#1148](https://github.com/qigao/salts/issues/1148)**: real, matched CNet/NativeIO UDP, CNet KCP and DPDK/R2 workload, payload size/window/batching, p50/p95/p99/p99.9, Mpps/Gbps, CPU cycles and dedicated polling-core opportunity cost. **No performance win/loss conclusion can be drawn** from this software checkpoint.
4. **Product promotion**: no installed provider API, ABI change, fallback/legacy branch, master merge, production traffic routing or release of a new CNet implementation. A separate explicit authorization and design review are mandatory.

Keep [#1148](https://github.com/qigao/salts/issues/1148) **Open / Deferred**; keep [#1145](https://github.com/qigao/salts/issues/1145), [#1146](https://github.com/qigao/salts/issues/1146), [#1147](https://github.com/qigao/salts/issues/1147), [#1151](https://github.com/qigao/salts/issues/1151) and [#1152](https://github.com/qigao/salts/issues/1152) open as unmerged research/integration tracking. Individual **software-only gates can be marked accepted** without claiming their unverified production/hardware objectives complete.

## Reproducing the software-only qualification

```sh
cmake -S experiments/cnet-packet-io -B build/r2-software \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPACKET_IO_ENABLE_DPDK=OFF \
  -DPACKET_IO_ENABLE_GMSSL=OFF \
  -DPACKET_IO_ENABLE_CMETA_SDK=OFF
cmake --build build/r2-software --parallel 2
ctest --test-dir build/r2-software --output-on-failure
```

This default research CTest does not need DPDK, a physical NIC, a self-hosted runner or a live service. Its test-only HMAC provider uses system OpenSSL and is **not a production key authority**. Independent CI jobs separately exercise the explicitly opted-in GmSSL and immutable rc.14 SDK consumers plus the DPDK `net_ring` loopback; the hardware probe must never be interpreted as executed because the target compiled.

Use [RELIABLE_PROTOCOL.md](RELIABLE_PROTOCOL.md), [GMSSL_PROVIDER.md](GMSSL_PROVIDER.md), [SDK_RC14_CONFORMANCE.md](SDK_RC14_CONFORMANCE.md), [MULTI_SESSION.md](MULTI_SESSION.md) and [DMA_COMPLETION_HANDOFF.md](DMA_COMPLETION_HANDOFF.md) as the detailed source-of-truth contracts, with [NIC_LAB.md](NIC_LAB.md) parked until the hardware phase is resumed.

**Hand-off recommendation:** preserve the exact Git SHA and test evidence, stop changing this research branch unless a regression is discovered, and resume physical NIC work only after explicit future approval and a dedicated qualified runner. No asynchronous testing or automatic resumption is scheduled.
