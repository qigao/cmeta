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

## Research-only bounded reliable-message engine (#1147)

The separate [R1 wire and lifecycle contract](RELIABLE_PROTOCOL.md) defines a 36-byte, unauthenticated experimental envelope with a 64-byte fragment payload, up to eight fragments per message, selective ACK bitmap, bounded retransmission and exact protocol-terminal callback. This protocol is **not** a production security profile, Tesla wire compatibility, RDMA or a replacement for KCP.

The standalone CMake project includes `packet_reliable_conformance` (11 named deterministic scenarios plus 40 pseudorandom loss/reorder message trials) and `packet_reliable_packet_io` (two joined Packet I/O / protocol fake-driver lifetime scenarios). The independent CI matrix runs these under GCC/Clang ASan+UBSan and GCC TSan; the optional real DPDK `net_ring` smoke remains separate.

To run all the standalone tests, configure the research project and invoke `ctest --test-dir <build-directory> --output-on-failure`. Neither the new protocol core nor the deterministic tests requires the DPDK SDK or a physical NIC.
