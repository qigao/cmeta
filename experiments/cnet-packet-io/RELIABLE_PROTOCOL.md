# Experimental reliable-message wire protocol R1

**Status: research-only, fixed-capacity conformance.** Tracked by [#1147](https://github.com/qigao/salts/issues/1147) under [#1145](https://github.com/qigao/salts/issues/1145) in [Draft PR #1149](https://github.com/qigao/salts/pull/1149). NOT a Tesla TTPoE-compatible or installed Salts protocol.

## Frozen R1 scope

One caller-owned, address-stable session per Owner thread, with one active logical TX and one in-progress RX message per direction. The caller provides an out-of-band local participant ID, peer ID, a nonzero epoch and a first message ID. There is NO discovery, session handshake, dynamic routing, MTU negotiation or persistence. Restart requires a greater generation and a *different epoch*. Message-ID rollover is rejected; use a new epoch.

- Each logical message has 1–512 payload bytes, fragmented into 1–8 pieces of at most 64 bytes. TX admission copies into a bounded internal buffer; RX reassembly has its own fixed 512-byte store. No zero-copy claim.
- The send callback synchronously copies or independently retains the complete frame before returning success (1). Backpressure (0) transfers nothing; negative or unexpected return is a failure. The protocol never borrows caller bytes after admitted send returns.
- Exactly one delivery callback occurs for each accepted inbound message ID in the configured live epoch. Delivery is a borrowed view and means callback invocation, NOT durable business commit.
- Exactly one logical send terminal follows a complete peer ACK bitmap, cancellation, expiry, or fatal emit. Packet I/O TX acceptance is not peer ACK. Logical cancellation does NOT release accepted PMD/DMA frames.
- The caller supplies monotonic millisecond time and a bounded per-call transmit budget. No hidden threads, timers, automatic transaction replay or raw runtime-state persistence.

## Wire payload schema (network byte order)

This 36-byte header is an **experimental payload envelope**, not a complete Ethernet frame. A future real Ethernet adapter must prepend Ethernet addressing/protocol metadata and demultiplex before parsing. All integer fields use network byte order.

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 4 | ASCII SRE1 magic |
| 4 | 1 | Version 1 |
| 5 | 1 | DATA=1, ACK=2 |
| 6 | 1 | Fragment index; ACK must be zero |
| 7 | 1 | Fragment count 1–8 |
| 8 | 4 | Source participant ID |
| 12 | 4 | Destination participant ID |
| 16 | 4 | Session epoch |
| 20 | 4 | Message ID, nonzero |
| 24 | 2 | Total logical message bytes 1–512 |
| 26 | 2 | Frame payload bytes 0–64; ACK must be zero |
| 28 | 4 | Selective ACK bitmap; DATA must be zero |
| 32 | 4 | IEEE CRC32 of header bytes 0–31 concatenated with payload |
| 36 | 0–64 | Fragment payload |

Fragment count equals ceil(message length / 64). Nonfinal fragments carry exactly 64 bytes; final fragment carries exact remainder. An ACK repeats message ID, original fragment count and logical length, and sends a bitmap of received fragments. Corrupt, truncated, wrong participant/epoch and inconsistent frames are rejected; ACK bits for never-sent fragments are ignored. Duplicate data after completed delivery triggers a new ACK, not another delivery.

**Security boundary:** CRC32 is a corruption detector, NOT authentication or encryption. An active adversary can forge a packet or successful ACK with a valid CRC. Participant IDs and epoch are only configured routing assertions, not cryptographic authority. Do not expose this R1 to untrusted networks. A separate authenticated handshake, AEAD/keys, anti-replay, peer admission and denial-of-service review are required.

## Bounded reliability

- Sender emits only within a configured packet window (1–8); one selective ACK bitmap tracks acknowledged fragments, and only missing fragments are resent after RTO. Per-fragment retry count and a whole-message absolute deadline impose hard bounds.
- One single-session AIMD-like credit decreases by half on retransmit (minimum one) and increases by one on newly acknowledged fragments (maximum configured window). This does NOT prove multi-session fairness, ECN behavior or safe incast handling.
- ACK delay is configured in milliseconds. Duplicate packets do not indefinitely postpone an identical pending ACK. All resource counters/buffers remain bounded.
- Failed admission makes no terminal callback. Successfully admitted send creates exactly one final callback: ACKED, CANCELLED, TIMEOUT or EMIT_FAILED. Seal prevents new send and permits existing ACK/TX progress; drain returns BUSY until logical TX and ACK staging settle. **Physical Packet I/O must be sealed and drained independently** before device or DMA resources are destroyed.

## Executable oracles

`tests/reliable_test.c` has 11 named tests: correct full-message ACK, out-of-order selective ACK, dropped/delayed ACK, duplicate suppression, malformed/wrong ID/epoch frames, queue backpressure and absolute timeout, retry exhaustion/congestion credit, fatal emit, CRC-valid bogus ACK for unsent fragments, cancellation/Generation/Owner/reentrancy, and a 40-message deterministic pseudorandom loss/reordering replay.

`tests/reliable_packet_io_test.c` has two integration tests using the existing `packet_io_owner` fake-driver SPI. They prove that physical TX admission does not settle reliable TX, payload storage and DMA-style frame ownership have different terminal boundaries, cancel does not prematurely free an in-flight frame, and a late ACK does not publish a duplicate terminal. They are not NIC-DMA proofs.

Use the independent CMake project with DPDK disabled for these oracles, then run CTest. This leaves production CNet and NativeIO entirely unchanged.

## Remaining #1147 / #1148 gates

Still open: actual Ethernet encapsulation/demux; authenticated peer handshake and replay keying; multi-session admission, incast/fairness, route/MTU discovery, richer congestion and RTT control, fuzz/negative fault matrices, real NIC teardown/driver quiescence, and equal-semantics comparison against CNet KCP (#1148). No public ABI or master merge from this R1 checkpoint.
