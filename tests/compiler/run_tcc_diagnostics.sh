#!/usr/bin/env bash
# Diagnostic-only: do not enable C11 capability flags or modify production ABI.
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/atomic-probe
out="$PWD/build/atomic-probe"
source="$PWD/tests/compiler"

: "${TCC:=tcc}"
: "${CC_NATIVE:=gcc}"

"$TCC" -std=c11 "$source/tcc_atomic_probe.c" -o "$out/tcc_atomic_probe"
timeout 30s "$out/tcc_atomic_probe"

# Build and run in both directions. Use GCC as the system linker and link the
# native atomic helper if one is needed. No Salts or TurboWasm libraries.
"$TCC" -std=c11 -c "$source/atomic_abi_peer.c" -o "$out/peer_tcc.o"
"$CC_NATIVE" -std=c11 -O2 -pthread -c "$source/atomic_abi_driver.c" -o "$out/driver_native.o"
"$CC_NATIVE" -pthread "$out/driver_native.o" "$out/peer_tcc.o" -latomic \
    -o "$out/native_driver_tcc_peer"
timeout 30s "$out/native_driver_tcc_peer"

"$CC_NATIVE" -std=c11 -O2 -pthread -c "$source/atomic_abi_peer.c" -o "$out/peer_native.o"
"$TCC" -std=c11 -c "$source/atomic_abi_driver.c" -o "$out/driver_tcc.o"
"$CC_NATIVE" -pthread "$out/driver_tcc.o" "$out/peer_native.o" -latomic \
    -o "$out/tcc_driver_native_peer"
timeout 30s "$out/tcc_driver_native_peer"

# Independently test TCC-generated atomics in both communicating TUs.
"$CC_NATIVE" -pthread "$out/driver_tcc.o" "$out/peer_tcc.o" -latomic \
    -o "$out/tcc_driver_tcc_peer"
timeout 30s "$out/tcc_driver_tcc_peer"
