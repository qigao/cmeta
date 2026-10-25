#!/usr/bin/env bash
# Diagnostic only: do not override __STDC_NO_ATOMICS__ or modify Salts ABI.
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/atomic-probe
out="$PWD/build/atomic-probe"
source="$PWD/tests/compiler"

: "${TCC:=tcc}"
: "${CC_NATIVE:=gcc}"

"$TCC" -std=c11 "$source/tcc_atomic_probe.c" -o "$out/tcc_atomic_probe"
timeout 30s "$out/tcc_atomic_probe"

"$TCC" -std=c11 -c "$source/atomic_abi_peer.c" -o "$out/peer_tcc.o"
"$CC_NATIVE" -std=c11 -O2 -pthread -c "$source/atomic_abi_driver.c" -o "$out/driver_native.o"
"$CC_NATIVE" -std=c11 -O2 -pthread -c "$source/atomic_abi_peer.c" -o "$out/peer_native.o"
"$TCC" -std=c11 -c "$source/atomic_abi_driver.c" -o "$out/driver_tcc.o"

# Observe the default GNU ld behavior separately. TinyCC 0.9.28rc can produce
# overlapping .eh_frame FDEs rejected by modern GNU ld; do not hide this as an
# atomic test failure, or call the compiler 'admitted' after a workaround.
echo '== default GNU ld interoperability =='
if "$CC_NATIVE" -pthread "$out/driver_native.o" "$out/peer_tcc.o" -latomic \
      -o "$out/default_link_probe" 2>"$out/default_link.log"; then
    echo 'default GNU ld mixed-object link: PASS'
else
    echo 'default GNU ld mixed-object link: FAILED (unwind ABI not qualified)'
    cat "$out/default_link.log"
fi

# Diagnostic-only link switches: avoiding the .eh_frame_hdr construction and
# explicitly marking the test process stack NX does not repair unwind ABI.
# These executables are never installed or published as Salts libraries.
link_flags=(-Wl,--no-eh-frame-hdr -Wl,-z,noexecstack)

echo '== native driver / TCC peer =='
"$CC_NATIVE" -pthread "${link_flags[@]}" "$out/driver_native.o" "$out/peer_tcc.o" \
    -latomic -o "$out/native_driver_tcc_peer"
timeout 30s "$out/native_driver_tcc_peer"

echo '== TCC driver / native peer =='
"$CC_NATIVE" -pthread "${link_flags[@]}" "$out/driver_tcc.o" "$out/peer_native.o" \
    -latomic -o "$out/tcc_driver_native_peer"
timeout 30s "$out/tcc_driver_native_peer"

echo '== TCC driver / TCC peer =='
"$CC_NATIVE" -pthread "${link_flags[@]}" "$out/driver_tcc.o" "$out/peer_tcc.o" \
    -latomic -o "$out/tcc_driver_tcc_peer"
timeout 30s "$out/tcc_driver_tcc_peer"

echo '== TCC native linker / TCC driver and TCC peer =='
if "$TCC" "$out/driver_tcc.o" "$out/peer_tcc.o" -lpthread -latomic \
      -o "$out/tcc_native_link" >"$out/tcc_native_link.log" 2>&1; then
    timeout 30s "$out/tcc_native_link"
else
    echo 'TCC native linker on multi-TU atomic probe: FAILED (diagnostic)'
    cat "$out/tcc_native_link.log"
fi

echo '== TCC compile and run the real Salts::Platform POSIX threading backend =='
"$TCC" -std=c11 -D_DEFAULT_SOURCE=1 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
    -Iplatform/include platform/src/thread_posix.c \
    tests/compiler/tcc_platform_thread_probe.c -lpthread \
    -o "$out/tcc_platform_thread_probe"
timeout 30s "$out/tcc_platform_thread_probe"
