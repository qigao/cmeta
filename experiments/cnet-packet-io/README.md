# CNet packet-I/O research (not an installed Salts component)

Tracking: [#1145](https://github.com/qigao/salts/issues/1145), [#1146](https://github.com/qigao/salts/issues/1146), [Draft PR #1149](https://github.com/qigao/salts/pull/1149).

This standalone **Linux-only C11** experiment is intentionally outside `cnet/` and the top-level CMake graph. It neither links into `Salts::CNet` nor enables DPDK for production code. The normal Salts NativeIO/SDK and mobile CI paths remain unchanged. The independent workflow `cnet-packet-io-research.yml` owns these checks.

## Fake PMD conformance (requires no DPDK)

```sh
cmake -S experiments/cnet-packet-io -B build/pio-fake \
  -DCMAKE_C_COMPILER=gcc \
  '-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined' \
  -DPACKET_IO_ENABLE_DPDK=OFF
cmake --build build/pio-fake --parallel 2
ctest --test-dir build/pio-fake --output-on-failure
```

The test uses a synchronous fake queue with six deterministic cases: bounded admission and rejection, partial bursts, physical-frame drain, abort, stale Generation, cross-thread Owner rejection, RX borrowed callback lifetime, reentrancy, FIFO wraparound and zero-budget progression. It does **not** emulate hardware DMA or protocol ACK semantics.

## Optional real DPDK net_ring integration

Install a system DPDK SDK with `pkg-config libdpdk` and the `rte_net_ring` driver (Ubuntu 24.04: `libdpdk-dev`). These are opt-in only; missing dependencies cause CMake configuration failure rather than fallback to UDP.

```sh
cmake -S experiments/cnet-packet-io -B build/pio-dpdk \
  -DPACKET_IO_ENABLE_DPDK=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/pio-dpdk --parallel 2
# Must exit 0; exit 77 means EAL unavailable, NOT test success.
./build/pio-dpdk/packet_io_dpdk_ring_test
```

The smoke instantiates `rte_eth_from_rings()`, configures an actual Ethernet PMD queue and passes a 64-byte test frame through TX and RX. It **copies** the immutable application payload into a separately owned `rte_mbuf`, so the original frame lease can be released after PMD admission; no zero-copy claim is made. The ring adapter's quiescence check is valid only because this test owns both ends of its own loopback ring and releases every received mbuf. It does **not** qualify real-NIC DMA teardown.

## Ownership and terminal boundaries

- `packet_io_submit` transfers ownership of a frame only on successful admission; all rejection paths keep caller ownership.
- `tx_burst` returns a prefix accepted by a backend; the owner retains the unaccepted suffix. PMD acceptance and mbuf reclaim are **not** remote ACKs.
- RX callback has only a borrowed view. The Owner releases each RX frame immediately after the callback returns; keeping raw pointers is invalid.
- All Owner operations are bound to the thread that started the generation. A fresh Generation is required after a completed stop.
- Sealing rejects new TX admission; abort releases only staged frames. Stop returns busy until staged frames are gone and the driver explicitly confirms quiescence.
- The SPI is internal, not stable, not an alternative to NativeIO, and not a substitute for a reliable message protocol. The latter is separately tracked in #1147 with ACK, retransmission and bounded-congestion semantics.

No Tesla wire compatibility, native GPUDirect/RDMA support, authenticated transport, or physical-NIC performance measurements are claimed. The Salts 2.3.0 installed public ABI remains frozen.

## Research-only authenticated reliable-message engine (#1147)

The canonical [R2 protocol and key-lifetime contract](RELIABLE_PROTOCOL.md) describes **mandatory HMAC-SHA256 authentication** over the experimental 36-byte SRE2 header and DATA (32-byte tag). **CRC-only R1 is retired, without fallback**. The protocol is not an authenticated peer handshake, confidential channel, approved GmSSL runtime Provider, TTPoE interoperability or production service.

The crypto library is **not linked into `packet_reliable_research`** or the installed Salts/CNet SDK. Only the isolated test targets link a real system `libcrypto` HMAC fixture. That fixture validates RFC 4231, forged/wrong-key ACK rejection, MAC-provider error paths, invalid key admission, callback reentrancy, replay across epochs and secure owner-key erasure; the original deterministic loss/reorder and Packet I/O lease tests remain active.

CTest targets: `packet_reliable_conformance`, `packet_reliable_packet_io`, and `packet_io_fake_owner`; optional standalone DPDK `net_ring` smoke is unchanged. Configure `experiments/cnet-packet-io` with `-DPACKET_IO_ENABLE_DPDK=OFF`, build and run `ctest --test-dir <build-directory> --output-on-failure`. The research test target requires a system OpenSSL development package for its HMAC fixture; this does not add a requirement to production Salts.

**Still open:** production GmSSL provider, authenticated peer/key provision and renewal, real Ethernet demux, multi-peer scheduling and NIC hardware completion. Keep Draft PR #1149 unmerged.

## Pinned GmSSL R2 MAC experiment (#1151)

The standalone `-DPACKET_IO_ENABLE_GMSSL=ON` option builds an **explicitly selected**, Owner-bound `GmSSL::GmSSL` HMAC-SHA256 MAC adapter with Generation/peer/epoch lease fencing. Read the [GmSSL Provider contract](GMSSL_PROVIDER.md) for the exact build/credential/teardown boundaries. The isolated Linux GitHub job pins the upstream GmSSL source commit used by Salts' GmSSL vcpkg registry. OpenSSL stays an independent **test-only** comparator; there is no production `Salts::CNet` linkage or automatic fallback.

The provider's MAC context is supplied as the **required** `rl_callbacks.mac_user`, separate from application `callbacks.user`. MAC requests before explicit bind or after lease release fail closed. Successful `rl_stop` wipes R2-owned keys; physical Packet I/O and DMA must be drained independently. **The exact published Salts.Native 2.3.0-rc.14 CMeta Crypto ABI now passes isolated C11/C++17 research conformance; trusted peer authentication, secure key provisioning and production adoption remain open.**

## Official 2.3.0-rc.14 native SDK conformance (#1151)

An opt-in `PACKET_IO_ENABLE_CMETA_SDK=ON` target uses the **published** `Salts.Native.2.3.0-rc.14.nupkg` installed `Salts::Core`, NOT a private GmSSL symbol or a source-build substitute. The research workflow checks the exact package SHA-256, manifest, source commit, `find_package(Salts 2.3.0 EXACT CONFIG)`, 2.2-version rejection, and executable C11/C++17 crypto consumers. The GmSSL reference and CMeta export adapters share **one** Owner/Generation-scoped `rl_mac_lease`. See [reproducible SDK attestation](SDK_RC14_CONFORMANCE.md).

[Exact-head SDK CI #38232436205](https://github.com/qigao/salts/actions/runs/38232436205) completed successfully: 6/6 independent jobs, 5/5 installed-SDK CTests, and the 2.2 rejection. The SDK remains a **prerelease**; the experimental transport is not installed or public, and no trusted peer-key establishment, actual NIC teardown, production DSO hot replacement, or multi-peer fairness is implied.

## Bounded multi-session R2 scheduling (#1152)

The research-only [multi-session contract](MULTI_SESSION.md) adds `r2_mux.{h,c}`: one fixed Owner, at most eight **independently authenticated** R2 sessions, immutable peer/epoch lookup, one grant per peer per round-robin visit, and exact physical-frame reference counts. Failed peers and full queues cannot create a second terminal authority or implicit fallback.

`packet_r2_mux_conformance` runs 1/2/4/8-session ACK/terminal tests, two-slot physical queue backpressure, a slow-peer starvation check, forged ACK isolation, eight-peer inbound incast, callback reentrancy, cancellation with pending physical frame and late Generation/epoch rejection. Each test uses **one actual `packet_io_owner` over a Fake PMD**, with separate HMAC fixtures and crypto leases per peer. The test is enabled in the existing standalone CTest matrix and requires no DPDK device.

This **does not prove** physical NIC driver fairness, DPDK DMA reclamation, authenticated peer establishment or a quantitative Mpps/p99 benchmark. PR #1149 remains unmerged and is not a public CNet ABI.

## Off-Owner physical completion ingress (#1146 / #1152)

The experimental [DMA-like completion handoff](DMA_COMPLETION_HANDOFF.md) introduces a bounded 32-ticket mailbox. A producer thread can publish **only** an already-reserved completion token; it cannot touch `r2_mux`, R2 session keys, or the final frame. The original Owner polls, releases one physical mux reference and executes the finalizer exactly once. A successful logical ACK, cancellation, or Packet I/O TX burst acceptance remains independent of physical buffer completion.

`packet_dma_owner_handoff` uses a real C11/POSIX worker and a fake retained Packet I/O TX backend to test cancel-before-completion, post-after-seal, duplicate/old Generation tickets, explicit joined-producer fence, no early stop/detach, two-record admission capacity and bounded poll progress. Run under GCC/Clang ASan+UBSan and GCC TSan. **This is not physical NIC DMA qualification**, and it adds no installed Salts ABI.

## Manually approved real PCI NIC qualification (#1146 / #1148)

A separate [hardware-only runbook](NIC_LAB.md) and workflow `.github/workflows/cnet-r2-hardware-nic.yml` are available for a **dedicated, offline, self-hosted PCI VFIO Ethernet lab**, triggered *only* when a maintainer manually applies `r2-hardware-nic-approved` to Draft PR #1149. The lab requires two explicitly configured cabled PCI Ethernet PFs, IOMMU and free hugepages. There is no automatic driver binding, privilege escalation, device provisioning, or fallback to `net_ring`.

With `PACKET_IO_ENABLE_DPDK=ON`, the isolated CMake project **builds** `packet_io_dpdk_physical_test`, but ordinary GitHub-hosted CI **never runs** it. Actual execution requires both links UP, 16 real Ethernet frames per direction with exact nonce/sequence/payload, a supported `rte_eth_tx_done_cleanup()`, and observed driver TX mbuf-ref reclamation before caller frees its retained observer refs. Missing hardware or unsupported cleanup **FAIL**, never SKIP or PASS.

The Python PCI/IOMMU/VFIO preflight also has a no-hardware negative contract test. Neither compiling the probe nor a passing `net_ring` test qualifies physical NIC DMA, driver reset/DSO unload safety, or R2-vs-KCP latency/throughput. Those remain distinct open gates.
