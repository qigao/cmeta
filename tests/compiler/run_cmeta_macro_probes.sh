#!/usr/bin/env bash
# Header/DSL probes with the selected compiler. No feature overrides or DSOs.
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${CC:=gcc}"
: "${BUILD_ROOT:=build/compiler-macros}"
mkdir -p "$BUILD_ROOT"
out="$(cd "$BUILD_ROOT" && pwd)"
common=(-std=c11 -Icmeta/include -Iplugin/include -Iplatform/include)
failures=0

for name in cmeta_macro_probe plugin_decl_probe; do
    if "$CC" "${common[@]}" "tests/compiler/$name.c" -o "$out/$name" \
            >"$out/$name.log" 2>&1; then
        if timeout 30s "$out/$name" >>"$out/$name.log" 2>&1; then
            echo "$name: COMPILE/LINK/RUN PASS ($CC)"
            cat "$out/$name.log"
        else
            echo "$name: runtime FAILED ($CC)"
            cat "$out/$name.log"
            failures=$((failures + 1))
        fi
    else
        echo "$name: compile/link FAILED ($CC)"
        cat "$out/$name.log"
        failures=$((failures + 1))
    fi
done
echo "CMeta/Plugin public macro failures: $failures"
if (( failures > 0 )); then exit 1; fi
