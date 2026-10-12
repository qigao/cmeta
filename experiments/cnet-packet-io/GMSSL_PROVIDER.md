# Pinned GmSSL R2 MAC provider: experimental Owner lease

**Tracker:** [#1151](https://github.com/qigao/salts/issues/1151), under [#1147](https://github.com/qigao/salts/issues/1147); development [Draft PR #1149](https://github.com/qigao/salts/pull/1149). This is a **research-only GmSSL implementation adapter**, not an installed CNet public ABI or a shipped secure peer transport.

## Exact source and dependency graph

The standalone option `PACKET_IO_ENABLE_GMSSL=ON` explicitly requires `find_package(GmSSL CONFIG REQUIRED)` and an imported `GmSSL::GmSSL` target. It builds an independent `packet_gmssl_mac_research` library and one real-provider test; it has no dependency path from production Salts/CNet or NativeIO. With the option OFF (default), no GmSSL package or headers are required by the R2 experimental engine. Missing packages **fail configure**, never select OpenSSL as a hidden fallback.

GitHub's dedicated research job builds GmSSL from the exact upstream commit **7c9f02904ef33e59c87b4f16621cc8fd434e7579**, the upstream ref used by the [Salts GmSSL 3.2.0 vcpkg port](https://github.com/qigao/vcpkg-cache/blob/1f9d46f66436e3ce74677f393c25e72c008e0fc7/ports/gmssl/portfile.cmake), and links the test to that library. This qualifies **the upstream HMAC-SHA256 primitive at that SHA and a GmSSL-compatible imported target**; it does **not** run the fully patched vcpkg port. An independent, explicitly pinned published **Salts.Native 2.3.0-rc.14** consumer now exercises the exported `Salts::Core` ABI through the *shared* MAC Lease (see [SDK_RC14_CONFORMANCE.md](SDK_RC14_CONFORMANCE.md)); this is a separate gate from direct upstream GmSSL.

Salts already exports a provider-neutral `cmeta_hmac_sha256` function from `Salts::Core`, whose implementation uses GmSSL; a final production adapter should consume that established exported ABI and be qualified against an **exact immutable Salts.Native SDK**. This experimental direct GmSSL library is a feasibility and low-level implementation test, **not** a second CMeta Crypto registry.

The opt-in GmSSL conformance executable links OpenSSL `libcrypto` **only as an independent test oracle**. Both test libraries are confined to experimental CMake targets; there is no production crypto dependency change.

## Owner, Generation, and key authority

The protocol callback contract separates `callbacks.user` (application events) from **mandatory** `callbacks.mac_user` (authentication Provider). The latter cannot be NULL. Only one authenticated R2 session is associated with a fixed-address, explicitly selected **`rl_mac_lease`** at a time. The lease's Owner, Generation, peer, epoch, callback identity, seal and release logic is shared between the GmSSL source adapter and the installed CMeta SDK adapter: there is **no second lease runtime**.

1. A trusted host supplies directionally distinct 32-byte MAC keys and a peer/epoch/identity snapshot to `rl_start`. The R2 session **copies** the keys. **No handshake, key derivation or certificate validation happens here.**
2. The host configures `rl_callbacks.mac=rl_gmssl_mac` and `mac_user=&lease` and calls `rl_mac_lease_bind` immediately after successful R2 start. Bind asserts same Owner, live Generation, equal peer/local/epoch and exact callback linkage; it retains the reference to the stable session but **no second key copy**. MAC calls before bind fail closed.
3. The MAC function checks Owner thread and bound Generation/peer/epoch on each call and computes full-length HMAC-SHA256 with pinned GmSSL. A temporary digest is wiped, and no tag is published on Provider error. MAC callbacks remain synchronous and are fenced against reentrant R2 access.
4. The host first calls `rl_seal` (closing admission), then `rl_mac_lease_seal` (disallowing new binding but **continuing MAC service for already admitted ACK/TX**). Under persistent Provider errors, `rl_abort` settles logical sends with failure and discards pending ACK without pretending a peer success.
5. Only after successful `rl_stop` and R2 logical drain does `rl_mac_lease_release` detach that Provider Generation. `rl_stop` wipes the session's owned keys and buffers. Stale/older-generation rebind and cross-thread release are rejected; the lease keeps a monotonic generation watermark.
6. Finally, the host must **independently seal and drain Packet I/O**, including real NIC/DMA frame leases. The GmSSL Provider release is *not* proof of physical DMA completion or safety of arbitrary hot DSO unloading.

A leaked key in the caller's original config, weak entropy, reuse of an epoch after restart, or a compromised peer authority cannot be cured by this MAC callback. Dedicated secure identity/handshake, key provisioning/rotation authority, anti-replay across restart, tamper-resistant DSO lease, and denial-of-service controls remain required.

## Deterministic conformance

The standalone test `packet_gmssl_mac_conformance` checks:

- Real GmSSL HMAC-SHA256 output byte-for-byte against an independently linked OpenSSL HMAC implementation.
- Authenticated multi-fragment R2 delivery: first TX/queue admission never settles success, a tampered tag fails, authenticated peer ACK alone yields one success terminal.
- A second thread cannot use or seal a fixed Owner's MAC Provider; early lease release returns BUSY.
- Seal/stop clears Owner-copied keys, then lease release invalidates MAC access; fresh Generation/epoch rebind is permitted while stale tokens and old-epoch packets are rejected.
- Cancel, seal, stop and provider release preserve exactly one logical terminal and cannot bypass the independent Packet I/O DMA-drain contract.

Run with `PACKET_IO_ENABLE_GMSSL=ON` and a trusted explicit `GmSSL_DIR` which exports `GmSSL::GmSSL`, then build and invoke CTest. Never use the deterministic fixture keys outside test-only programs.

## Open gates

- [ ] Independently qualified **Salts.Native 2.3.0-rc.14** installed `Salts::Core/cmeta_hmac_sha256` C11/C++17 consumer with official NuGet SHA-256, source SHA and SameMinorVersion 2.2-negative gate; the new CI workflow is the source of execution evidence.
- [ ] Cryptographically authenticated peer establishment, direction/key derivation, replay after restart and key lifecycle security review.
- [ ] DSO N → N+1 lease callbacks/cancellation under actual concurrent hardware completion/TSan and cross-process restart semantics.
- [ ] Physical NIC PMD DMA quiescence, multi-peer scheduling (#1152), and fair hardware comparison against CNet KCP (#1148).

Do not merge Draft PR #1149 or claim a production-ready secure network transport based on this experiment alone.
