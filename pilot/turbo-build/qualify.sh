#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: qualify.sh /absolute/path/to/turbo_build_cli" >&2
  exit 2
fi

cli="$(realpath "$1")"
repo_root="$(git rev-parse --show-toplevel)"
pilot_dir="$repo_root/pilot/turbo-build"
evidence_dir="$repo_root/build/turbo-build-qualification"
workspace="$evidence_dir/workspace"
generated="$evidence_dir/generated"
summary="$evidence_dir/qualification-summary.txt"
compile_log="$evidence_dir/compiler-invocations.log"
generator_log="$evidence_dir/generator-invocations.log"

rm -rf "$evidence_dir"
mkdir -p "$workspace" "$generated"
: > "$compile_log"
: > "$generator_log"
: > "$summary"
export TB_PROBE_INVOCATION_LOG="$compile_log"
export TB_GENERATOR_INVOCATION_LOG="$generator_log"

record() {
  printf '%s\n' "$*" | tee -a "$summary"
}

compile_count() {
  wc -l < "$compile_log" | tr -d '[:space:]'
}

generator_count() {
  wc -l < "$generator_log" | tr -d '[:space:]'
}

run_workspace_build() {
  local name="$1"
  (
    cd "$workspace"
    "$cli" validate buildfile.json
    "$cli" build --jobs 2 --state state.bin buildfile.json
  ) > "$evidence_dir/$name.log" 2>&1
}

assert_probe_output() {
  local expected="$1"
  local label="$2"
  (
    cd "$workspace"
    ./probe
  ) > "$evidence_dir/$label-output.txt"
  if [[ "$(cat "$evidence_dir/$label-output.txt")" != "$expected" ]]; then
    echo "$label: expected '$expected', got '$(cat "$evidence_dir/$label-output.txt")'" >&2
    exit 1
  fi
  record "$label output: $expected"
}

cp "$pilot_dir/probe.c" "$pilot_dir/probe.h" "$pilot_dir/compiler-wrapper.sh" "$workspace/"
cat > "$workspace/buildfile.json" <<'JSON'
{
  "version": 1,
  "actions": [
    {
      "name": "compile-probe",
      "executable": "bash",
      "args": [
        "compiler-wrapper.sh",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        "-O2",
        "-I.",
        "probe.c",
        "-o",
        "probe"
      ],
      "cwd": ".",
      "inputs": ["probe.c", "probe.h", "compiler-wrapper.sh"],
      "outputs": ["probe"],
      "toolchain": "ubuntu-24.04-cc-c11-qualification",
      "inputs_complete": false,
      "environment_complete": false,
      "toolchain_complete": false,
      "paths_normalized": true
    }
  ]
}
JSON

record "TurboBuild incremental correctness qualification"
record "TurboBuild CLI: $cli"
record "Cache-hit claims: DISABLED; compiler inputs/environment/toolchain remain explicitly incomplete."

run_workspace_build cold-build
assert_probe_output "salts-turbo-build-probe:46" cold-build
count="$(compile_count)"
[[ "$count" -eq 1 ]] || { echo "cold build expected 1 compiler invocation, got $count" >&2; exit 1; }
record "cold build compiler invocations: $count (expected 1)"

run_workspace_build no-op-build
assert_probe_output "salts-turbo-build-probe:46" no-op-build
count="$(compile_count)"
[[ "$count" -eq 2 ]] || { echo "conservative no-op expected a second compiler invocation, got $count" >&2; exit 1; }
record "unchanged second build compiler invocations: $count (expected 2; no skip is allowed while completeness is false)"

sed -i 's/#define SALTS_TURBO_BUILD_PROBE_MULTIPLIER 2/#define SALTS_TURBO_BUILD_PROBE_MULTIPLIER 3/' "$workspace/probe.h"
run_workspace_build header-change
assert_probe_output "salts-turbo-build-probe:69" header-change
count="$(compile_count)"
[[ "$count" -eq 3 ]] || { echo "header mutation expected a third compiler invocation, got $count" >&2; exit 1; }
record "header-change total compiler invocations: $count (expected 3)"

sed -i 's/return 23 \* SALTS_TURBO_BUILD_PROBE_MULTIPLIER;/return 24 * SALTS_TURBO_BUILD_PROBE_MULTIPLIER;/' "$workspace/probe.c"
run_workspace_build source-change
assert_probe_output "salts-turbo-build-probe:72" source-change
count="$(compile_count)"
[[ "$count" -eq 4 ]] || { echo "source mutation expected a fourth compiler invocation, got $count" >&2; exit 1; }
record "source-change total compiler invocations: $count (expected 4)"

# A separate generated-header graph verifies producer-before-consumer ordering
# and that a changed generator input reaches the compiled program.
cat > "$generated/seed.txt" <<'EOF'
10
EOF
cat > "$generated/generate-header.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'generate-header\n' >> "${TB_GENERATOR_INVOCATION_LOG:?}"
value="$(cat seed.txt)"
case "$value" in
  ''|*[!0-9]*) echo "seed.txt must contain a positive integer" >&2; exit 2 ;;
esac
printf '#ifndef GENERATED_PROBE_H\n#define GENERATED_PROBE_H\n#define GENERATED_PROBE_VALUE %s\n#endif\n' "$value" > generated.h
EOF
cat > "$generated/generated_probe.c" <<'EOF'
#include <stdio.h>
#include "generated.h"

int main(void)
{
    printf("generated-probe:%d\n", GENERATED_PROBE_VALUE);
    return 0;
}
EOF
cat > "$generated/buildfile.json" <<'JSON'
{
  "version": 1,
  "actions": [
    {
      "name": "generate-header",
      "executable": "bash",
      "args": ["generate-header.sh"],
      "cwd": ".",
      "inputs": ["seed.txt", "generate-header.sh"],
      "outputs": ["generated.h"],
      "toolchain": "bash-generated-header-pilot",
      "inputs_complete": false,
      "environment_complete": false,
      "toolchain_complete": false,
      "paths_normalized": true
    },
    {
      "name": "compile-generated-probe",
      "depends": ["generate-header"],
      "executable": "bash",
      "args": [
        "compiler-wrapper.sh",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        "-O2",
        "-I.",
        "generated_probe.c",
        "-o",
        "generated-probe"
      ],
      "cwd": ".",
      "inputs": ["generated_probe.c", "generated.h", "compiler-wrapper.sh"],
      "outputs": ["generated-probe"],
      "toolchain": "ubuntu-24.04-cc-generated-header-qualification",
      "inputs_complete": false,
      "environment_complete": false,
      "toolchain_complete": false,
      "paths_normalized": true
    }
  ]
}
JSON
cp "$pilot_dir/compiler-wrapper.sh" "$generated/"
before_compile="$(compile_count)"
(
  cd "$generated"
  "$cli" validate buildfile.json
  "$cli" build --jobs 2 --state state.bin buildfile.json
  ./generated-probe > "$evidence_dir/generated-header-cold-output.txt"
) > "$evidence_dir/generated-header-cold-build.log" 2>&1
[[ "$(cat "$evidence_dir/generated-header-cold-output.txt")" == "generated-probe:10" ]] || { echo "generated-header cold output mismatch" >&2; exit 1; }
[[ "$(generator_count)" -eq 1 ]] || { echo "generated-header producer did not run exactly once on cold build" >&2; exit 1; }
[[ "$(compile_count)" -eq $((before_compile + 1)) ]] || { echo "generated-header consumer compiler did not run after producer" >&2; exit 1; }
record "generated-header cold build: producer=1, consumer compiler=$(( $(compile_count) - before_compile )); output=generated-probe:10"

printf '11\n' > "$generated/seed.txt"
before_compile="$(compile_count)"
(
  cd "$generated"
  "$cli" validate buildfile.json
  "$cli" build --jobs 2 --state state.bin buildfile.json
  ./generated-probe > "$evidence_dir/generated-header-change-output.txt"
) > "$evidence_dir/generated-header-change-build.log" 2>&1
[[ "$(cat "$evidence_dir/generated-header-change-output.txt")" == "generated-probe:11" ]] || { echo "generated-header change was not observed by consumer" >&2; exit 1; }
[[ "$(generator_count)" -eq 2 ]] || { echo "generated-header producer count mismatch after seed mutation" >&2; exit 1; }
[[ "$(compile_count)" -eq $((before_compile + 1)) ]] || { echo "generated-header mutation did not trigger the consumer compiler" >&2; exit 1; }
record "generated-header mutation: producer=2 total, consumer compiler reran; output=generated-probe:11"

# Ensure a failed action fails the build and dependent actions do not run.
failure_dir="$evidence_dir/failure"
mkdir -p "$failure_dir"
cat > "$failure_dir/buildfile.json" <<'JSON'
{
  "version": 1,
  "actions": [
    {
      "name": "intentional-failure",
      "executable": "bash",
      "args": ["-c", "exit 7"],
      "cwd": ".",
      "inputs": [],
      "outputs": [],
      "toolchain": "intentional-failure-test",
      "inputs_complete": false,
      "environment_complete": false,
      "toolchain_complete": false,
      "paths_normalized": true
    },
    {
      "name": "must-not-run",
      "depends": ["intentional-failure"],
      "executable": "bash",
      "args": ["-c", "printf ran > after-failure.marker"],
      "cwd": ".",
      "inputs": [],
      "outputs": ["after-failure.marker"],
      "toolchain": "failure-dependent-test",
      "inputs_complete": false,
      "environment_complete": false,
      "toolchain_complete": false,
      "paths_normalized": true
    }
  ]
}
JSON
(
  cd "$failure_dir"
  "$cli" validate buildfile.json
) > "$evidence_dir/failure-validate.log" 2>&1
failure_status=0
(
  cd "$failure_dir"
  "$cli" build --jobs 2 --state state.bin buildfile.json
) > "$evidence_dir/failure-build.log" 2>&1 || failure_status=$?
[[ "$failure_status" -ne 0 ]] || { echo "intentional failure incorrectly returned success" >&2; exit 1; }
[[ ! -e "$failure_dir/after-failure.marker" ]] || { echo "dependent action ran after its prerequisite failed" >&2; exit 1; }
record "failure propagation: build exit=$failure_status (non-zero); dependent action did not run"

sha256sum "$evidence_dir"/*-output.txt "$evidence_dir/generated-header-"*-output.txt > "$evidence_dir/output-sha256.txt"
record "All qualification checks passed."
