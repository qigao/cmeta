# Immutable Salts.Native 2.3.0-rc.14 CMeta MAC consumer

**Tracker:** [#1151](https://github.com/qigao/salts/issues/1151), [Draft PR #1149](https://github.com/qigao/salts/pull/1149). **Installed Linux x64 prerelease SDK**, not a released stable API for the experimental packet transport.

## Source and package identity (strict)

The standalone [CNet Packet IO Research workflow](https://github.com/qigao/salts/blob/research/cnet-dpdk-packet-plane/.github/workflows/cnet-packet-io-research.yml) downloads the **actual published** [Salts v2.3.0-rc.14 GitHub Release](https://github.com/qigao/salts/releases/tag/v2.3.0-rc.14); it does not substitute a source checkout or rebuild a mock SDK.

- Package: `Salts.Native.2.3.0-rc.14.nupkg`
- Annotated tag's peeled source commit: `c8a1a3d5e259a89ad42e75f1e4c3142e6b106ee1`
- Package SHA-256: `fe8f5915adc0c7a0f5c52af75fa8cd188d6364dabda483013d842de242eb7a35`
- Installed RID path: `sdk/linux-x64/`
- Exact release manifest: `version=2.3.0-rc.14`, `commit=<source SHA>`, `os=linux`, `arch=x64`, `profile=release`.
- Both `SHA256SUMS` and hard-coded release digest must verify. A wrong/missing manifest, SHA, exported header, package or CMake configuration fails the job without fallback.

## Real installed consumer

`PACKET_IO_ENABLE_CMETA_SDK=ON` is opt-in and performs `find_package(Salts 2.3.0 EXACT CONFIG REQUIRED)`, then links `packet_cmeta_mac_research` and two native test programs against **the installed `Salts::Core` export**.

- The C11 consumer shares the **single** `rl_mac_lease` Owner/Generation/peer/epoch lifecycle used by the GmSSL reference implementation. Only the SHA256 calculation differs: `cmeta_hmac_sha256()`, with cleanup via `cmeta_crypto_clear()`. No GmSSL header, type or direct static library reaches this adapter.
- `packet_cmeta_sdk_conformance` checks real HMAC against independent OpenSSL test-only fixture, a full authenticated R2 DATA/ACK round-trip, wrong MAC rejection, Owner thread fencing, N→N+1 generation change and stop/release zeroization.
- `packet_cmeta_sdk_cpp` compiles and **runs** a C++17 consumer of the same installed `cmeta_crypto.h` export.
- An explicit negative CMake configure requests `Salts 2.2` against the 2.3 SDK and must fail due to SameMinorVersion admission. No legacy compatibility shim or automatic package selection.

This experiment retains two separate terminal authorities: authenticated protocol ACK/abort for logical delivery, and Packet I/O's real driver/frame completion for storage reclamation. The SDK test does not prove DMA safety or hot DSO unload.

## Reproduction

```bash
gh release download v2.3.0-rc.14 --repo qigao/salts \
  --pattern 'Salts.Native.2.3.0-rc.14.nupkg' \
  --pattern SHA256SUMS --dir dist
sha256sum --check dist/SHA256SUMS
# Verify the pinned SHA-256 and manifest before extraction.
# Extract the npm-style ZIP payload (NuGet .nupkg) to ./_sdk.
cmake -S experiments/cnet-packet-io -B build/pio-sdk \
  -DPACKET_IO_ENABLE_CMETA_SDK=ON \
  -DPACKET_IO_ENABLE_DPDK=OFF -DPACKET_IO_ENABLE_GMSSL=OFF \
  -DCMAKE_PREFIX_PATH="$PWD/_sdk/sdk/linux-x64"
cmake --build build/pio-sdk --parallel 2
ctest --test-dir build/pio-sdk --output-on-failure
```

The official workflow is authoritative for the complete SHA/manifest checks and runtime linker setup. A local unverified installation is **not** equivalent to this gate.

## Limitations

- This validates the **published prerelease** and its CMeta crypto ABI only, not stable `Salts 2.3.0`, complete patched GmSSL registry internals or downstream Windows/macOS ABI.
- Keys in the research test are fixed deterministic test vectors. A production host still needs authenticated key provisioning/rotation, independent peer authority, cross-restart anti-replay, bounded multi-session scheduling and physical NIC teardown conformance.
- None of these tests authorizes merging PR #1149, promoting a release, or replacing NativeIO.
