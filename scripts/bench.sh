#!/usr/bin/env bash
# Reverse-proxy latency/throughput reference run. See docs/PERFORMANCE.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${PYTHON_BIN:-}"
if [[ -z "${PY}" ]]; then
  if command -v python3 >/dev/null 2>&1; then
    PY=python3
  elif command -v python >/dev/null 2>&1; then
    PY=python
  else
    echo "bench: python3 (or python) is required" >&2
    exit 1
  fi
fi
exec "${PY}" "${ROOT}/scripts/bench.py" "$@"
