# RFC-first SCTP conformance matrix — Salts #1182

Evidence is scoped narrowly: **M1 wire = tested framing**; **M2 private = Owner-driven handshake plus bounded DATA/SACK/T3/Fast Recovery and receive-window tests**; **M3 private subset = DATA fragments and DCEP/PPID**. Unit tests use noncryptographic handshake fixtures except the separate real GmSSL HMAC/CSPRNG job. None establishes production security, full RFC conformance or browser interoperability.

| RFC requirement | State | Still required for conformance |
| --- | --- | --- |
| RFC 9260 §3/§6.8/App A packet header, CRC32C, Chunk lengths/padding | **M1 framing + RFC §3.2 action-bit classifier tested; exhaustive 256-type classifier assertion and mixed unknown-type packet-order test added (latest CI pending)** | Association now preflights all Chunk ordering before dispatch and rejects unsupported multi-control bundles before changing the time watermark; tests cover illegal duplicate COOKIE ACK bundles, prove a malformed COOKIE-ECHO + ERROR bundle does not advance the association clock watermark, and ensure a packet-triggered T5 transition occurs only after bundle members pass structural checks. Unknown-type action integration (stop/discard, stop/report, skip, skip/report), the full RFC legal-combination matrix, and atomic state plus ERROR-output commit remain pending; independent wire oracle and malformed-input fuzz remain pending. Latest source commits changed Association input and regression tests after the previously green checkpoint; exact-HEAD CI must be rechecked before treating these changes as validated. |
| RFC 9260 §§3.2.1–3.2.2 INIT / INIT ACK optional TLVs | **C11 00/01/10/11 actions + bounded Unrecognized Parameter/ERROR 8 wire responses**, 8 reports / 512 bytes, validated COOKIE ECHO+ERROR bundle | Full reporting beyond capacity, ERROR-cause notifications to ULP, IPv4/IPv6/ECN/cookie preservation and independent browser negotiation |
| RFC 9260 §§3.3.7,5.1.2 Host Name Address | **Private C11 ABORT (type 6), correct peer Initiate Tag, T=0**, no optional cause and capacity-before-transition tests | Other mandatory-parameter ABORT causes, full inbound ABORT semantics and independent wire conformance |
| RFC 9260 §5.2.1 COOKIE-WAIT / COOKIE-ECHOED simultaneous INIT | **Private C11 restricted handshake path**: original Tag/TSN, T1/saved handshake kept unchanged, no duplicate TCB | Authenticated Tie-Tags in COOKIE-ECHOED; address admission, actual CNet/DTLS owner and independent peer qualification |
| RFC 9260 §5.2.4 Table 12 case B/D simultaneous COOKIE ECHO | **Private Owner-only case B for unknown peer and case D for matching Tag/TSN**; full ACK-before-state admission, deterministic dual-sided and forged-MAC tests | Different-peer case B, restart A, source binding, Generation transaction/lease and ULP notifications still blocked |
| RFC 9260 §5.2.4 Table 12 collision/restart decisions | **Pure C11 classifier only** for cases A/B/C/D, nonzero Tie-Tag gating, exhaustive adversarial 3⁴ input combinations | Commit transactions, authenticated generation-bound Tie-Tags, T1 migration, restart ULP events, simultaneous-open integration and independent interop |
| RFC 9260 §5.2.4 expired duplicate Cookie | **Authenticated same-Association re-ACK** after TTL, preserving Peer Tag/TSN and rejecting identity changes | Stale Cookie ERROR cause for unrelated associations, complete TCB collision/restart state, overlap key lifetime |
| RFC 9260 §5 INIT / INIT ACK / COOKIE ECHO / COOKIE ACK and T1 | **M2 isolated state machine + Owner-injected CSPRNG/Tag/TSN** | Complete INIT parameters/negotiation, provider lifecycle, restart/tie tags, source binding, independent browser peer |
| RFC 9260 §5.1.3/§5.1.5 cookie authentication | **Private GmSSL HMAC-SM3 + two bounded key epochs**; prior-key in-flight Cookie test | Integrate real CNet Owner/DTLS Generation drain, enforce previous-key retirement deadline, replay/collision security and independent browser interoperability |
| RFC 9260 §§3.3,6,7 DATA, TSN/SACK, congestion, retransmission, RTO | **Private DATA/Gap-SACK/T3/Fast Recovery**, 16 retained TX/32 RX, TSN wrap, ordered/unordered, multi-TSN loss, actual flight vs pending intent, bounded RTT/backoff, exact TSN priority, zero-window DATA probe/reopened-window SACK with virtual-clock tests | Full RFC congestion/PMTU/multipath, broader receive-window and shutdown edge cases, PR-SCTP and independent peer |
| RFC 9260 §9 SHUTDOWN/ABORT | **Private SHUTDOWN-PENDING/SENT/RECEIVED/ACK-SENT, T2 retransmit, cumulative peer ACK, simultaneous shutdown and ULP RX preservation; optional absolute T5 guard; T5 expiry emits ABORT; late SHUTDOWN ACK cannot bypass an expired T5** | Independent close interoperability, complete ABORT/T-bit/error causes and all peer-race paths |
| RFC 9260 §§6.9–6.10 Chunk bundling | **Private bounded multi-DATA and leading-SACK receive; up to four outbound DATA chunks**, all-or-nothing input/send, one cumulative SACK, per-TSN retransmit tickets | General control bundling, independent peer and negotiated PMTU |
| RFC 8261 SCTP over DTLS | **Not implemented** | Existing GmSSL DTLS plaintext ingress/egress with real browser |
| RFC 3758 / RFC 7496 PR-SCTP | **Not implemented** | Timed/max-retransmits, FORWARD-TSN, independent peer |
| RFC 6525 / RFC 5061 stream reset | **Not implemented** | Reset/reuse and collisions |
| RFC 9260 DATA B/E fragmentation/reassembly | **Private 24 KiB queued message**, 1,024-byte fragments, common SID/SSN/PPID/U, strictly sequential TSNs; 20 KiB lost-first-fragment recovery via Fast Retransmit or T3 | Full browser-negotiated message limits, partial delivery, reassembly congestion/rwnd and independent peer validation |
| RFC 8260 I-DATA interleaving | **Not implemented** | Negotiation, FSN/MID and I-FORWARD-TSN; fragmented legacy DATA cannot interleave messages |
| RFC 8831 / RFC 8832 DataChannel/DCEP | **Private RFC 8832 DCEP OPEN/ACK** transmitted on SCTP ordered PPID 50 with DTLS-role SID parity; RFC 8831 PPID 51/53 and one-zero-byte PPID 56/57 empty messages | RFC 3758 partial reliability, DCEP channel close/reuse, full UTF-8/datachannel lifetime and independent browser peer |
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
  `Salts::CNet`. The isolated test also proves required GmSSL CSPRNG
  Tag/TSN generation and **at most two** simultaneously retained Cookie keys:
  rotation preserves valid in-flight Cookie ECHO; another rotation before
  prior-key retirement is rejected; retirement wipes the previous key and
  rejects its MAC. No implicit fallback or background timer exists.
  Replay, bounded key-overlap retirement timing, cross-Generation uniqueness
  and actual CNet/DTLS Provider Owner lifetime remain **OPEN**.
- The isolated Linux GmSSL binary is `3.2.0#9` from the SHA-pinned
  project vcpkg registry; the action used to provision it is also SHA-pinned.
  The existing Rust/C++-free design and unchanged production SDK remain intact.

## Release gates (all still open)

The exact-HEAD checkpoint `a98b50526a8d4133a7a2b5fc7786e638a05a3dab` was verified on 2026-10-20: [CNet SCTP C11 prototype #39427751448](https://github.com/qigao/salts/actions/runs/39427751448) passed all five jobs (Linux/macOS/Windows wire, sanitizer-source, pinned-GmSSL Cookie-MAC); [Salts CI #39427751574](https://github.com/qigao/salts/actions/runs/39427751574) passed; immutable build provenance #39427751331 passed. This source/test checkpoint adds `malformed_shutdown_bundle_does_not_preempt_t5`, asserting malformed SHUTDOWN+undersized SACK at the T5 deadline produces no output and leaves state/deadline/time watermark unchanged. The earlier `78ec9c5a177a92e10d4143e81fd4e1a106f399fd` checkpoint remains historical only. Green CI validates this bounded implementation and source graph, not independent shutdown interoperability or release readiness.

1. Complete RFC 9260 state machine, robust authenticated cookies and full parameter/unknown-chunk policy.
2. RFC 8261 GmSSL/DTLS integration with independent browser/libwebrtc wire interoperability.
3. Bounded DATA, TSN, SACK, congestion, fragmentation, PR-SCTP, reset and WebRTC DCEP.
4. Independent malformed-input fuzz, actual cryptographic oracle, effective-source ASan/UBSan/TSan.
5. Windows IOCP, macOS, Linux x64/arm64, Android, iOS full installed SDK qualification with immutable source graph.
6. Separate TurboMedia #227 exclusive usrsctp cutover; **no fallback**, no production merge/release before approval.
