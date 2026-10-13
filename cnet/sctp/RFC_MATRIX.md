# RFC-first SCTP conformance matrix — Salts #1181

Evidence is scoped narrowly: **M1 wire = tested framing**, **M2 research = synthetic two-engine handshake with a non-cryptographic test MAC**. Neither milestone establishes secure RFC compatibility or WebRTC interoperability.

| RFC requirement | State | Still required for conformance |
| --- | --- | --- |
| RFC 9260 §3/§6.8/App A packet header, CRC32C, Chunk lengths/padding | **M1 local and CI-tested** | Independent wire oracle, malformed-input fuzz and explicit unknown-chunk handling |
| RFC 9260 §5 INIT / INIT ACK / COOKIE ECHO / COOKIE ACK and T1 | **M2 research prototype** | Real cryptographic GmSSL MAC, cookie key rotation/expiry, unpredictable tags and TSNs, INIT parameters, collision/restart/tie tags, independent peer |
| RFC 9260 §5.1.3/§5.1.5 cookie authentication | **Private GmSSL HMAC-SM3 isolated test PASS**; deterministic state-machine fixture remains non-crypto | Production key lifecycle/rotation, authenticated DTLS generation binding, replay handling, independent browser interoperability and concurrency gates |
| RFC 9260 §§3.3,6,7 DATA, TSN/SACK, congestion, retransmission, RTO | **Not implemented** | Loss/reorder/duplicate/wraparound, PMTU, flow and congestion tests |
| RFC 9260 §9 SHUTDOWN/ABORT | **Not implemented** | Full close states, pending DATA, simultaneous shutdown, correct ABORT |
| RFC 8261 SCTP over DTLS | **Not implemented** | Existing GmSSL DTLS plaintext ingress/egress with real browser |
| RFC 3758 / RFC 7496 PR-SCTP | **Not implemented** | Timed/max-retransmits, FORWARD-TSN, independent peer |
| RFC 6525 / RFC 5061 stream reset | **Not implemented** | Reset/reuse and collisions |
| RFC 8260 I-DATA interleaving | **Not implemented** | Negotiation, fragmentation, I-FORWARD-TSN |
| RFC 8831 / RFC 8832 DataChannel/DCEP | **Not implemented** | OPEN/ACK, PPID, SID parity, reliability semantics |
| RFC 8841 SDP | **Not implemented** | Browser-facing SDP matrix |
| RFC 6455 WebSocket | **Out of scope** | Useful CNet design reference only, not an SCTP wire replacement |

## Source, license, language and ownership

- Original **C11-only** RFC-first code. No third-party SCTP library source was copied/translated, no Rust/C++ protocol engine, FFI, Cargo, GN, cxx or new runtime.
- License/source provenance of any future external fixture or optional secure-MAC provider must be reviewed separately. Independent browser/libwebrtc implementations are black-box test oracles only.
- Private M2 experiment uses one callback-verified Owner, no thread/worker, bounded fixed packet memory (1200 bytes per retained handshake packet), external monotonic clock, no hidden retransmission, explicit stop/drain/destroy. **Concurrent Owner cancellation and effective-source TSan are still pending**.
- `cnet_sctp_assoc_proto` is not installed/exported. Existing `Salts::CNet`, `SaltsTargets`, CNet runtime, TurboMedia and public SDK remain unchanged.
- M2 unit tests use **intentionally insecure deterministic MAC stubs** only to test callback/auth failure/control flow. Never promote their PASS status to production cookie security.
- An optional **private GmSSL HMAC-SM3 C11 adapter/test** uses real keyed SM3-HMAC
  with a separate Cookie domain, a known cross-implementation HMAC vector, fresh
  GmSSL Linux test-key material, tamper/wrong-key/wrong-Generation/retired-key
  rejection, source-instrumented ASan+UBSan and 10 repeated CTest runs.
  The adapter is not included in the installed C11 Wire package or in
  `Salts::CNet`. Only the MAC primitive/boundary is qualified; replay,
  key rotation and true CNet/DTLS Provider Owner lifetime are still **OPEN**.
- The isolated Linux GmSSL binary is `3.2.0#9` from the SHA-pinned
  project vcpkg registry; the action used to provision it is also SHA-pinned.
  The existing Rust/C++-free design and unchanged production SDK remain intact.

## Release gates (all still open)

1. Complete RFC 9260 state machine, robust authenticated cookies and full parameter/unknown-chunk policy.
2. RFC 8261 GmSSL/DTLS integration with independent browser/libwebrtc wire interoperability.
3. Bounded DATA, TSN, SACK, congestion, fragmentation, PR-SCTP, reset and WebRTC DCEP.
4. Independent malformed-input fuzz, actual cryptographic oracle, effective-source ASan/UBSan/TSan.
5. Windows IOCP, macOS, Linux x64/arm64, Android, iOS full installed SDK qualification with immutable source graph.
6. Separate TurboMedia #227 exclusive usrsctp cutover; **no fallback**, no production merge/release before approval.
