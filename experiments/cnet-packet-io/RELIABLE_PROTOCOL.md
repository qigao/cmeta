# Experimental reliable-message wire protocol R2

**Status: research-only, fixed-capacity authenticated envelope; NOT production-ready.** Tracking [#1147](https://github.com/qigao/salts/issues/1147), [#1145](https://github.com/qigao/salts/issues/1145), [Draft PR #1149](https://github.com/qigao/salts/pull/1149). This is neither Tesla TTPoE-compatible nor a stable Salts/CNet ABI. **R1 is retired; there is no CRC-only fallback.**

## Contract and trust boundary

One fixed Owner thread per address-stable session, one active TX logical message and one partial RX message. The host configures nonzero, distinct participant IDs, a nonzero session epoch, first message ID, and **independent directional 32-byte TX/RX MAC keys**. Session keys must be generated/provisioned by a trusted application authority, not learned from a received frame; the protocol has no key exchange or handshake. Keys must be rotated when starting a fresh epoch. Host-provided epoch uniqueness across process restarts is essential; no raw runtime state is persisted.

The `rl_start` call requires a non-null MAC provider callback and two nonzero, nonidentical directional keys. It copies the keys into owner-local state and `rl_stop` explicitly wipes those copies. The host owns its original key material and must clear it independently. Failed crypto operations reject frames/abort corresponding send admission without a permissive fallback. The provider-neutral protocol engine never links a crypto library; base tests use a real HMAC-SHA256 system-`libcrypto` fixture only in test targets. A separate, explicitly enabled [pinned GmSSL research adapter](GMSSL_PROVIDER.md) now exercises an Owner-bound Generation/peer/epoch lease. Production `Salts::Core`/CMeta installed SDK validation, secure key injection and trusted peer establishment **remain open**.

HMAC-SHA256 authenticates possession of the configured directional keys and covers the wire header, participant IDs, epoch, sequence/fragment metadata and payload. It **does not provide encryption**, verify key provenance, perform peer handshakes, rate-limit forged traffic, or guarantee business commit. Successful CRC or apparent participant IDs are not authentication. Message-ID monotonicity and epoch binding reject stale/incomplete/replayed messages within configured state; protecting against replay after process restart requires fresh independently provisioned epoch and keys.

## Message limits and API boundary

- TX copies 1–512 bytes into an owned 512-byte bounded buffer on successful admission; RX reassembly owns another 512-byte bounded buffer. One message is 1–8 fragments, 64 payload bytes maximum each.
- Emission callback consumes a borrowed complete packet only during invocation and returns **1 only after independent copy/retention**. Return 0 means bounded backpressure and no transfer; error never becomes peer ACK.
- Received delivery is a borrowed view, published once per message ID within the live epoch, **not** an exactly-once durable business transaction. ACKed TX completes exactly once only on matching authenticated SACK bitmap.
- No independent thread, worker, retrying application transactions, hidden allocations during progress, negotiation, discovery, routing, MTU adaptation, or automatic fallback. Caller advances monotonic millisecond time with a bounded emission budget.
- Packet I/O retains authority for physical frame leases and DMA safety; a successful logical ACK or cancellation does **not** release its outstanding TX mbufs.

## R2 packet envelope (network byte order)

The R2 envelope consists of a 36-byte header + 0–64 bytes of DATA + **32-byte full HMAC-SHA256 tag**. Its payload range is 68–132 bytes. This is **not** an Ethernet frame: a real adapter must supply an Ethernet header, EtherType, peer routing and demultiplexing.

| Offset | Width | Meaning |
|---:|---:|---|
| 0 | 4 | ASCII `SRE2` magic |
| 4 | 1 | Version `2` (R1 refused) |
| 5 | 1 | DATA=1, ACK=2 |
| 6 | 1 | DATA fragment index; ACK must be zero |
| 7 | 1 | Fragment count 1–8 |
| 8 | 4 | Source participant ID |
| 12 | 4 | Destination participant ID |
| 16 | 4 | Session epoch |
| 20 | 4 | Nonzero logical message ID |
| 24 | 2 | Original message length, 1–512 |
| 26 | 2 | Fragment payload length, 0–64 (ACK=0) |
| 28 | 4 | Selective ACK bitmap (DATA=0) |
| 32 | 4 | CRC32 of header bytes 0–31 + DATA payload, for quick corruption detection only |
| 36 | 0–64 | DATA payload; ACK has none |
| 36 + payload length | 32 | HMAC-SHA256 over **exact bytes 0 through 35 + payload** using sender's directional TX key |

On receipt, size/version/shape/source/target/epoch and CRC are checked before the full keyed MAC. The verifier computes the expected MAC using the local RX key and compares all 32 bytes with a branch-free accumulator; neither invalid MAC nor provider error mutates ACK/delivery state. Even a correctly authenticated ACK cannot acknowledge never-sent fragments. Corrupt, wrong-key, wrong-epoch, replayed or forged frames must never publish a successful logical terminal.

## Bounded reliability and congestion

- One-bit-per-fragment SACK bitmap (up to eight fragments), bounded send window, explicit RTO per fragment, maximum retries, and an absolute message deadline. Retransmit only unacknowledged fragments.
- The current experimental per-session congestion credit halves on retransmission and increases on new ACKs up to the configured window. This is **not** fair multi-peer congestion management and does not prove DCQCN, ECN, pacing, incast protection, rate limiting or loss-free properties.
- Duplicate valid DATA after delivery triggers an ACK, never a second delivery. Duplicate/stale ACKs cannot re-settle an already-terminal logical send.
- `seal` rejects new send admission; `step` can still progress previously admitted messages and ACKs; `drain` returns BUSY until protocol terminal and ACK staging are quiescent. On persistent crypto failure or forced shutdown, the host may explicitly call `rl_abort` **only after `rl_seal`**: it discards pending local ACKs without inventing an ACK success and terminally cancels active logical TX exactly once. This provides a bounded logical-drain escape but **does not reclaim driver/DMA buffers**. Explicitly drain the independent Packet I/O Owner before releasing physical frames.
- Successful `stop` wipes owner-copied authentication keys and message payload buffers; caller-owned input config and external crypto provider state remain under their own authority.

## Executable evidence and remaining gates

Base standalone tests link real HMAC-SHA256 through system `libcrypto` **only in the research test targets**; a separately enabled GmSSL provider test links the actual pinned GmSSL SHA256 implementation. They cover RFC 4231 known-answer MAC, missing provider/empty or identical keys, tampered CRC or MAC, wrong directional key, valid-CRC forged ACK, credential-holding bogus ACK for unsent fragments, provider-error fail-closed, callback reentrancy, session epoch replay, fail-closed explicit abort after a permanently failing MAC provider, and key erasure after stop; existing 40-message seeded loss/reorder tests and the two Packet I/O ownership tests also run over R2 frames.

```sh
cmake -S experiments/cnet-packet-io -B build/reliable \
  -DCMAKE_BUILD_TYPE=Debug -DPACKET_IO_ENABLE_DPDK=OFF
cmake --build build/reliable --parallel 2
ctest --test-dir build/reliable --output-on-failure
```

The test fixture uses deterministic static keys and OpenSSL only for independent protocol conformance; **never deploy the fixture keys or test provider as a production key authority**. An opt-in research adapter now uses the **published Salts.Native 2.3.0-rc.14** exported CMeta `cmeta_hmac_sha256` ABI; [SDK_RC14_CONFORMANCE.md](SDK_RC14_CONFORMANCE.md) records the exact digest and installed C11/C++17 tests. This validates an ABI *consumer*, not secure key distribution, peer authentication, production DSO lease handling or public transport admission.

Still open in #1147: GmSSL production provider, secure peer establishment and key rotation authority, rate/DoS controls, a real Ethernet encapsulation and packet demultiplexer, multiple sessions per Owner with bounded fairness, incast/ECN/RTT congestion behavior, fuzzing, and physical-NIC drain/cancel races. #1148 remains the separate equal-semantics comparison against NativeIO UDP/KCP and RDMA-capable hardware. **Do not merge Draft PR #1149 or claim production security on these unit tests.**
