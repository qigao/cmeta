#!/usr/bin/env bash
# ELF-only explicit-manifest shared object ABI smoke (not Plugin/ACE runtime).
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${COMPILER_A:=gcc}"
: "${COMPILER_B:=clang}"
: "${BUILD_ROOT:=build/compiler-plugin-dso}"

mkdir -p "$BUILD_ROOT"
out="$(cd "$BUILD_ROOT" && pwd)"
common=(-std=c11 -Icmeta/include -Iplatform/include -Iplugin/include)
passed=0
failed=0

attempt() {
    local provider_cc="$1" host_cc="$2" id="$3"
    local shared="$out/lib${id}.so" host="$out/${id}_host"
    local provider_log="$out/${id}_provider.log"
    local host_log="$out/${id}_host.log"
    if ! "$provider_cc" "${common[@]}" -fPIC -shared \
        tests/compiler/plugin_dso_provider.c -o "$shared" \
        >"$provider_log" 2>&1; then
        echo "$id: provider compile/link FAILED"
        cat "$provider_log"; failed=$((failed+1)); return 0
    fi
    if ! "$host_cc" "${common[@]}" tests/compiler/plugin_dso_host.c \
        -ldl -o "$host" >"$host_log" 2>&1; then
        echo "$id: host compile/link FAILED"
        cat "$host_log"; failed=$((failed+1)); return 0
    fi
    if ! timeout 30s "$host" "$shared" >>"$host_log" 2>&1; then
        echo "$id: dlopen / dlsym / bootstrap FAILED"
        cat "$host_log"; failed=$((failed+1)); return 0
    fi
    echo "$id: SHARED PROVIDER ABI PASS"
    cat "$host_log"
    passed=$((passed+1))
}

attempt "$COMPILER_A" "$COMPILER_B" "a_provider_b_host"
attempt "$COMPILER_B" "$COMPILER_A" "b_provider_a_host"
echo "Explicit Plugin shared-object bootstrap: $passed/2 passed, $failed failed"
if ((failed > 0)); then exit 1; fi
