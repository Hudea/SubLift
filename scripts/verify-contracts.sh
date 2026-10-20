#!/usr/bin/env bash
# SubLift contract fixture gate (Phase 17 D01/D02). Not a product gate.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${PYTHON:-python3}"

if ! command -v "$PYTHON" >/dev/null 2>&1; then
  echo "verify-contracts: python3 not found (set PYTHON=...)" >&2
  exit 1
fi

exec "$PYTHON" "$ROOT/scripts/check_contracts.py" "$@"
