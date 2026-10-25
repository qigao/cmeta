#!/usr/bin/env bash
# Full CMeta/CSTL object + static archive + consumer probe.
# SOURCE OF TRUTH: canonical add_library(${TARGET_NAME} src/*.c ...) blocks.
# This is an isolated build; no Salts production CMake targets or ABI are changed.
set -euo pipefail
cd "$(dirname "$0")/../.."

: "${CC:=gcc}"
: "${AR:=ar}"
: "${BUILD_ROOT:=build/compiler-cmeta-cstl}"
: "${C11_FLAGS:=-std=c11}"

mkdir -p "$BUILD_ROOT"
out="$(cd "$BUILD_ROOT" && pwd)"
mkdir -p "$out/cmeta" "$out/cstl"
echo "Compiler: $CC; archive tool: $AR; output: $out"

sources_from_cmake() {
    local list_file="$1"
    # Fail rather than silently omit new/complex CMake source expressions.
    awk '
        /^add_library\(\$\{TARGET_NAME\}[[:space:]]*$/ { inside=1; next }
        inside && /^[[:space:]]*\)[[:space:]]*$/ { finished=1; exit }
        inside {
            item=$0
            gsub(/[[:space:]]/, "", item)
            if (item ~ /^src\/[A-Za-z0-9_-]+\.c\)$/) {
                sub(/\)$/, "", item)
                print item
                finished=1
                exit
            } else if (item ~ /^src\/[A-Za-z0-9_-]+\.c$/) print item
            else if (length(item)) {
                print "Unsupported production source list entry: " item > "/dev/stderr"
                exit 2
            }
        }
        END {
            if (!inside || !finished) {
                print "Could not read complete production library source list" > "/dev/stderr"
                exit 3
            }
        }
    ' "$list_file"
}
sources_from_cmake cmeta/CMakeLists.txt >"$out/cmeta_sources.txt"
sources_from_cmake cstl/CMakeLists.txt >"$out/cstl_sources.txt"
mapfile -t cmeta_sources <"$out/cmeta_sources.txt"
mapfile -t cstl_sources <"$out/cstl_sources.txt"
if (( ${#cmeta_sources[@]} != 19 || ${#cstl_sources[@]} != 18 )); then
    echo "Unexpected source counts: CMeta=${#cmeta_sources[@]} CSTL=${#cstl_sources[@]}" >&2
    exit 1
fi
printf 'Canonical CMeta/CSTL source units: %d/%d\n' \
    "${#cmeta_sources[@]}" "${#cstl_sources[@]}"

common=(-Icmeta/include -Icstl/include -Iplatform/include)
bad=0
compile_group() {
    local group="$1"
    shift
    local rel src dest
    for rel in "$@"; do
        src="$group/$rel"
        dest="$out/$group/$(basename "${rel%.c}").o"
        if "$CC" $C11_FLAGS "${common[@]}" -c "$src" -o "$dest" \
            >"$dest.log" 2>&1; then
            echo "$group/$rel : COMPILE PASS"
        else
            echo "$group/$rel : COMPILE FAILED"
            cat "$dest.log"
            bad=$((bad + 1))
        fi
    done
}
compile_group cmeta "${cmeta_sources[@]}"
compile_group cstl "${cstl_sources[@]}"
echo "CMeta/CSTL compilation failures: $bad"
if (( bad > 0 )); then
    echo "Archive and consumer tests not attempted due to compile failures"
    exit 1
fi

cmeta_objects=()
cstl_objects=()
for rel in "${cmeta_sources[@]}"; do
    cmeta_objects+=("$out/cmeta/$(basename "${rel%.c}").o")
done
for rel in "${cstl_sources[@]}"; do
    cstl_objects+=("$out/cstl/$(basename "${rel%.c}").o")
done
rm -f "$out/libcmeta_compiler.a" "$out/libcstl_compiler.a"
"$AR" rcs "$out/libcmeta_compiler.a" "${cmeta_objects[@]}"
"$AR" rcs "$out/libcstl_compiler.a" "${cstl_objects[@]}"
echo "Full canonical static archives: PASS"

"$CC" $C11_FLAGS "${common[@]}" tests/compiler/cmeta_cstl_consumer.c \
    "$out/libcstl_compiler.a" "$out/libcmeta_compiler.a" -lm \
    -o "$out/cmeta_cstl_consumer"
timeout 30s "$out/cmeta_cstl_consumer"
