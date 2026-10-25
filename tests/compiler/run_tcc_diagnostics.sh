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
if "$TCC" "$out/driver_tcc.o" "$out/peer_tcc.o" -lpthread \
      -o "$out/tcc_native_link" >"$out/tcc_native_link.log" 2>&1; then
    timeout 30s "$out/tcc_native_link"
else
    echo 'TCC native linker on multi-TU atomic probe: FAILED (diagnostic)'
    cat "$out/tcc_native_link.log"
fi

echo '== TCC _Thread_local syntax and pthread isolation, without feature macro overrides =='
"$TCC" -std=c11 -D_POSIX_C_SOURCE=200809L \
    "$source/tcc_tls_probe.c" -lpthread -o "$out/tcc_tls_probe"
timeout 30s "$out/tcc_tls_probe"

echo '== independent pthread_once + TSS first use and cleanup (c-threads reference) =='
"$TCC" -std=c11 "$source/posix_tss_semantics_probe.c" -lpthread \
    -o "$out/tcc_posix_tss_probe"
timeout 30s "$out/tcc_posix_tss_probe"

echo '== default Salts::Platform header admission and actual POSIX source =='
if "$TCC" -std=c11 -D_DEFAULT_SOURCE=1 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
       -Iplatform/include platform/src/thread_posix.c \
       tests/compiler/tcc_platform_thread_probe.c -lpthread \
       -o "$out/tcc_platform_strict" 2>"$out/platform_strict.log"; then
    echo 'Salts::Platform default header compiled with TCC'
    timeout 30s "$out/tcc_platform_strict"
else
    echo 'Salts::Platform default header rejects or fails TCC (NOT QUALIFIED)'
    cat "$out/platform_strict.log"
    # Isolate native TLS language capability only. This target is a private
    # diagnostic; it DOES NOT change Salts headers, ABI or release admission.
    echo '== diagnostic-only opt-in to native _Thread_local, NOT Salts admission =='
    "$TCC" -std=c11 -D_DEFAULT_SOURCE=1 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
        -DSALTS_THREAD_LOCAL=_Thread_local \
        -Iplatform/include platform/src/thread_posix.c \
        tests/compiler/tcc_platform_thread_probe.c -lpthread \
        -o "$out/tcc_platform_native_tls_diagnostic"
    timeout 30s "$out/tcc_platform_native_tls_diagnostic"
fi

echo '== TinyCC isolated Salts C11 source object probes (no SDK link) =='
object_failures=0
tcc_compile_object() {
    local name="$1"
    shift
    if "$TCC" -std=c11 "$@" -o "$out/$name.o" >"$out/$name.log" 2>&1; then
        echo "object $name: PASS (compile only, no SDK admission)"
    else
        echo "object $name: FAILED (compiler/source diagnostic)"
        cat "$out/$name.log"
        object_failures=$((object_failures + 1))
    fi
}

tcc_compile_object cmeta_abi -Icmeta/include -Iplatform/include -c cmeta/src/abi.c
tcc_compile_object cmeta_type_identity -Icmeta/include -Iplatform/include -c cmeta/src/type_identity.c
tcc_compile_object cstl_vec -Icmeta/include -Icstl/include -Iplatform/include -c cstl/src/vec.c
tcc_compile_object asn1 -Iasn1/include -c asn1/src/asn1.c

if (( object_failures > 0 )); then
    echo "TinyCC isolated source objects: $object_failures FAILED (NOT QUALIFIED)"
    exit 1
fi
echo 'TinyCC isolated source objects: 4/4 PASS (compile-only)'
