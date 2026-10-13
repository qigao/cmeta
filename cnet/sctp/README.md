# Experimental CNet SCTP Wire (pure C11)

Independent, standalone RFC 9260 packet-wire building block for [Salts #1181](https://github.com/qigao/salts/issues/1181).
**This is not a complete SCTP protocol engine**. It deliberately does not
perform INIT/COOKIE handshakes, SACK/retransmission, partial reliability,
stream reconfiguration, DTLS/ICE or WebRTC DCEP. Nothing in production CNet or
TurboMedia links against it. No fallback or Rust/C++ protocol implementation.

This subdirectory is a separate C-only CMake project. In the repository root:

```sh
cmake -S cnet/sctp -B build-cnet-sctp -DBUILD_TESTING=ON
cmake --build build-cnet-sctp --config Release
ctest --test-dir build-cnet-sctp -C Release --output-on-failure
cmake --install build-cnet-sctp --config Release --prefix "$PWD/sdk-cnet-sctp"
cmake -S cnet/sctp/tests/installed -B build-cnet-sctp-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/sdk-cnet-sctp"
cmake --build build-cnet-sctp-consumer --config Release
ctest --test-dir build-cnet-sctp-consumer -C Release --output-on-failure
```

The installed package name is **CNetSctpPrototype**, not Salts. Its exported
`CNetSctpPrototype::Wire` target is a research/test target, not an API stability
promise. This is intentionally absent from `SaltsTargets` and existing CNet
source lists, so it cannot change current shipping behavior.

Borrowed views are valid only for the lifetime of the caller's packet bytes.
Every call is synchronous and allocation-free. Decoders validate the full packet
before yielding views; upper-layer association semantics and unknown-chunk
handling remain unimplemented. No Owner/runtime timers have been added at M1.

See [RFC_MATRIX.md](RFC_MATRIX.md) for the required capabilities and evidence.
