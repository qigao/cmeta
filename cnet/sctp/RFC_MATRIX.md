# RFC-first SCTP conformance matrix — Salts #1181

Evidence is scoped narrowly: **M1 wire = tested framing**, **M2 research = synthetic two-engine handshake with a non-cryptographic test MAC**. Neither milestone establishes secure RFC compatibility or WebRTC interoperability.

| RFC requirement | State | Still required for conformance |
| --- | --- | --- |
| RFC 9260 §3/§6.8/App A packet header, CRC32C, Chunk lengths/padding | **M1 local and CI-tested** | Independent wire oracle, malformed-input fuzz and explicit unknown-chunk handling |
| RFC 9260 §§3.2.1–3.2.2 INIT / INIT ACK optional TLVs | **C11 00/01/10/11 actions + bounded Unrecognized Parameter/ERROR 8 wire responses**, 8 reports / 512 bytes, validated COOKIE ECHO+ERROR bundle | Full reporting beyond capacity, ERROR-cause notifications to ULP, IPv4/IPv6/ECN/cookie preservation and independent browser negotiation |
| RFC 9260 §§3.3.7,5.1.2 Host Name Address | **Private C11 ABORT (type 6), correct peer Initiate Tag, T=0**, no optional cause and capacity-before-transition tests | Other mandatory-parameter ABORT causes, full inbound ABORT semantics and independent wire conformance |
| RFC 9260 §5.2.1 COOKIE-WAIT / COOKIE-ECHOED simultaneous INIT | **Private C11 restricted handshake path**: original Tag/TSN, T1/saved handshake kept unchanged, no duplicate TCB | Authenticated Tie-Tags in COOKIE-ECHOED; address admission, actual CNet/DTLS owner and independent peer qualification |
| RFC 9260 §5.2.4 Table 12 case B/D simultaneous COOKIE ECHO | **Private Owner-only case B for unknown peer and case D for matching Tag/TSN**; full ACK-before-state admission, deterministic dual-sided and forged-MAC tests | Different-peer case B, restart A, source binding, Generation transaction/lease and ULP notifications still blocked |
| RFC 9260 §5.2.4 Table 12 collision/restart decisions | **Pure C11 classifier only** for cases A/B/C/D, nonzero Tie-Tag gating, exhaustive adversarial 3⁴ input combinations | Commit transactions, authenticated generation-bound Tie-Tags, T1 migration, restart ULP events, simultaneous-open integration and independent interop |
| RFC 9260 §5.2.4 expired duplicate Cookie | **Authenticated same-Association re-ACK** after TTL, preserving Peer Tag/TSN and rejecting identity changes | Stale Cookie ERROR cause for unrelated associations, complete TCB collision/restart state, overlap key lifetime |
| RFC 9260 §5 INIT / INIT ACK / COOKIE ECHO / COOKIE ACK and T1 | **M2 isolated state machine + Owner-injected CSPRNG/Tag/TSN** | Complete INIT parameters/negotiation, provider lifecycle, restart/tie tags, source binding, independent browser peer |
| RFC 9260 §5.1.3/§5.1.5 cookie authentication | **Private GmSSL HMAC-SM3 + two bounded key epochs**; prior-key in-flight Cookie test | Integrate real CNet Owner/DTLS Generation drain, enforce previous-key retirement deadline, replay/collision security and independent browser interoperability |
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

1. Complete RFC 9260 state machine, robust authenticated cookies and full parameter/unknown-chunk policy.
2. RFC 8261 GmSSL/DTLS integration with independent browser/libwebrtc wire interoperability.
3. Bounded DATA, TSN, SACK, congestion, fragmentation, PR-SCTP, reset and WebRTC DCEP.
4. Independent malformed-input fuzz, actual cryptographic oracle, effective-source ASan/UBSan/TSan.
5. Windows IOCP, macOS, Linux x64/arm64, Android, iOS full installed SDK qualification with immutable source graph.
6. Separate TurboMedia #227 exclusive usrsctp cutover; **no fallback**, no production merge/release before approval.
