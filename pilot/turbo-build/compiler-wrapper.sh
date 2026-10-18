#!/usr/bin/env bash
set -euo pipefail

# The log is enabled only by the opt-in qualification script. Ordinary pilot
# builds still invoke the same compiler without requiring logging state.
if [[ -n "${TB_PROBE_INVOCATION_LOG:-}" ]]; then
  printf '%s\n' "$*" >> "$TB_PROBE_INVOCATION_LOG"
fi

exec cc "$@"
