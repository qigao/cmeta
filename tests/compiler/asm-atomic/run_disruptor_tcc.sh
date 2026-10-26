#!/usr/bin/env bash
# Build exactly one unmodified Disruptor implementation per executable.
# Compile that implementation under TinyCC using test-only private ASM slots.
# External .S is assembled by native GAS, then linked by TinyCC without C11
# atomic feature overrides, production headers, or SDK installation.
set -euo pipefail
cd "$(dirname "$0")/../../.."
: "${TCC:=tcc}"
: "${CC_ASM:=gcc}"
: "${BUILD_ROOT:=build/asm-disruptor-tcc}"
: "${SALTS_TEST_ASM_ORDERED:=0}"
case "$SALTS_TEST_ASM_ORDERED" in
  0) define_order=(); order_label=seq_cst ;;
  1) define_order=(-DSALTS_TEST_ASM_ORDERED=1); order_label=loadstore_ordered ;;
  *) echo "Invalid ASM_ORDERED mode" >&2; exit 2 ;;
esac
mkdir -p "$BUILD_ROOT"
out="$(cd "$BUILD_ROOT" && pwd)"

case "$(uname -m)" in
  x86_64) arch=x86_64 ;;
  aarch64|arm64) arch=aarch64 ;;
  *) echo "Unsupported atomic ASM target architecture" >&2; exit 2 ;;
esac
"$CC_ASM" -c -x assembler-with-cpp \
    "tests/compiler/asm-atomic/asm_atomic_${arch}.S" \
    -o "$out/asm_atomic_backend.o"

for variant in real_source_conformance mpmc_real_source parked_worker_conformance; do
  echo "ASSEMBLY DISRUPTOR: TCC ($arch) backend=$order_label / $variant"
  "$TCC" -std=c11 "${define_order[@]}" -D_DEFAULT_SOURCE=1 -D_POSIX_C_SOURCE=200809L \
    -D_XOPEN_SOURCE=700 -DSALTS_THREAD_LOCAL=_Thread_local \
    -Iconcurrency/include -Iutils/include -Iplatform/include \
    tests/compiler/asm-atomic/disruptor_asm_inclusion.c \
    platform/src/thread_posix.c \
    "tests/compiler/disruptor_${variant}.c" \
    "$out/asm_atomic_backend.o" -lpthread \
    -o "$out/${variant}"
  timeout 40s "$out/${variant}"
done
echo "ASM Disruptor real-source parity: 3/3 PASS ($order_label; TCC diagnostic, NOT admission)"
