# RFC-first C11 SCTP protocol matrix — Salts #1181

Status is deliberately conservative. `M1 wire` is **not** SCTP association
compatibility and **not** evidence of a functioning WebRTC DataChannel.

| Normative source | Required feature for WebRTC | Status | Evidence needed |
| --- | --- | --- | --- |
| RFC 9260 §3, §6.8, Appendix A | Common header, CRC32C, chunk length/padding | **M1 implemented** | C11 wire/installed consumer tests, independent wire vectors, fuzz + sanitizers |
| RFC 9260 §5 | INIT / INIT-ACK / COOKIE-ECHO / COOKIE-ACK | Not implemented | Independent association handshake, cookie security, state-machine negative tests |
| RFC 9260 §§3.3,6,7 | DATA, TSN/SACK, retransmission, RTO, flow/congestion control | Not implemented | Loss/reorder/duplicate, wraparound, PMTU and deterministic clock |
| RFC 9260 §9 | SHUTDOWN / ABORT and resource retirement | Not implemented | Cancellation precedence, no callback after drained |
| RFC 8261 | SCTP packet transport over DTLS | Not implemented | Existing GmSSL DTLS plaintext packet bridge, browser interoperability |
| RFC 3758 + RFC 7496 | PR-SCTP FORWARD-TSN, per-message lifetime/retransmits | Not implemented | Timed/maxRetransmits independent browser tests |
| RFC 6525 + RFC 5061 | Stream reset/reconfiguration | Not implemented | Stream reset, stream reuse, pending reset collisions |
| RFC 8260 | I-DATA / I-FORWARD-TSN interleaving when negotiated | Not implemented | Negotiation and concurrent fragmented messages |
| RFC 8831 / RFC 8832 | WebRTC multistream DataChannel + DCEP | Not implemented | OPEN/ACK, SID, PPID, ordered/unordered, empty/text/binary |
| RFC 8841 | SDP association configuration | Not implemented | Browser handshake/SDP trace and m-line constraints |
| RFC 6455 | WebSocket framing | **Out of scope** | May inform CNet API architecture; cannot replace WebRTC SCTP wire protocol |

## License and toolchain audit

- This M1 module is original C11 implementation from published RFC behavior;
  no SCTP source code was copied or translated from usrsctp, dcSCTP, libwebrtc,
  or Rust/C++ implementations.
- Build language: **C** only; installed consumer: **C11** only.
- Dependencies: C11 headers/library only, CMake host build tool.
- Non-production standalone package: `CNetSctpPrototype::Wire`, not added to
  `Salts::CNet` source list, export set, or released SDK.
- Independent SCTP implementation permitted only as an *external black-box
  interoperability oracle*, not linked into or distributed with this module.
- License review of test vectors, external fixtures and all dependencies is
  mandatory prior to any published SDK export.

## Next gates

1. M1: checksum oracle vectors and malformed packet fuzz/sanitizer matrix.
2. M2: bounded owner-local association, secure cookie generation strategy,
   independent timer progression and deterministic INIT/COOKIE state tests.
3. M3: data, fragmentation, SACK/retransmission, PR-SCTP, reset + interleaving.
4. M4: GmSSL DTLS/browser independent wire interop; no self-loopback substitute.
5. M5: ASan/UBSan/effective-source TSan and Windows/macOS/Linux arm64/Android/iOS.
6. M6: reviewed production cutover and removal of legacy usrsctp; never fallback.
