#!/usr/bin/env bash
# Mix two complete, independently compiled CMeta/CSTL static libraries.
# Uses diagnostic-only GNU ld options only if explicitly supplied. This does
# NOT qualify DSO unwind metadata, installed SDK ABI or Salts concurrency.
set -euo pipefail
cd "$(dirname "$0")/../.."

: "${BUILD_A:?set BUILD_A to a full compiler archive output directory}"
: "${BUILD_B:?set BUILD_B to another compiler archive output directory}"
: "${CC_A:=gcc}"
: "${CC_B:=clang}"
: "${LINKER:=gcc}"
: "${BUILD_ROOT:=build/compiler-mixed-libs}"

mkdir -p "$BUILD_ROOT"
out="$(cd "$BUILD_ROOT" && pwd)"
A="$(cd "$BUILD_A" && pwd)"
B="$(cd "$BUILD_B" && pwd)"
for base in "$A" "$B"; do
    for archive in libcstl_compiler.a libcmeta_compiler.a; do
        if [[ ! -f "$base/$archive" ]]; then
            echo "Missing complete archive: $base/$archive" >&2
            exit 1
        fi
    done
done

flags=()
if [[ -n "${EXTRA_LINK_FLAGS:-}" ]]; then
    read -r -a flags <<< "$EXTRA_LINK_FLAGS"
fi

echo "Mix A=$CC_A ($A) with B=$CC_B ($B) using $LINKER"
if (( ${#flags[@]} )); then
    echo "Diagnostic linker overrides in effect: ${flags[*]} (NOT SDK admission)"
else
    echo "Default linker semantics; no diagnostic unwind overrides"
fi

bad=0
passed=0
mix_one() {
    local name="$1" consumer_cc="$2" cstl_archive="$3" cmeta_archive="$4"
    local object="$out/$name.o" executable="$out/$name"
    local fail_log="$out/$name.log"

    if ! "$consumer_cc" -std=c11 -Icmeta/include -Icstl/include \
         -Iplatform/include -c tests/compiler/cmeta_cstl_consumer.c \
         -o "$object" >"$fail_log" 2>&1; then
        echo "$name: consumer compile FAILED"
        cat "$fail_log"; bad=$((bad + 1)); return 0
    fi
    if ! "$LINKER" "${flags[@]}" "$object" "$cstl_archive" "$cmeta_archive" \
         -lm -o "$executable" >"$fail_log" 2>&1; then
        echo "$name: mixed archive link FAILED"
        cat "$fail_log"; bad=$((bad + 1)); return 0
    fi
    if ! timeout 30s "$executable" >"$fail_log" 2>&1; then
        echo "$name: linked consumer execution FAILED"
        cat "$fail_log"; bad=$((bad + 1)); return 0
    fi
    echo "$name: MIXED ABI PASS"
    cat "$fail_log"
    passed=$((passed + 1))
}

# Each archive direction is tested with the compiler that built each side.
mix_one "a_consumer_aCstl_bMeta" "$CC_A" \
    "$A/libcstl_compiler.a" "$B/libcmeta_compiler.a"
mix_one "b_consumer_bCstl_aMeta" "$CC_B" \
    "$B/libcstl_compiler.a" "$A/libcmeta_compiler.a"
mix_one "a_consumer_bCstl_aMeta" "$CC_A" \
    "$B/libcstl_compiler.a" "$A/libcmeta_compiler.a"
mix_one "b_consumer_aCstl_bMeta" "$CC_B" \
    "$A/libcstl_compiler.a" "$B/libcmeta_compiler.a"

echo "Mixed CMeta/CSTL diagnostic summary: $passed/4 passed, $bad failed"
if (( bad > 0 )); then
    exit 1
fi
