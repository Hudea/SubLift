#!/usr/bin/env bash
# SubLift continuity gate: phase truth, status alignment, links, optional schema.
# Not a product gate; see docs/DEVELOPMENT.md and docs/TESTMAP.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${PYTHON:-python3}"

if ! command -v "$PYTHON" >/dev/null 2>&1; then
  echo "verify-harness: python3 not found (set PYTHON=...)" >&2
  exit 1
fi

exec "$PYTHON" "$ROOT/scripts/check_project.py" "$@"
