#!/usr/bin/env bash
# Thin wrapper so the Python-free container gate has a stable short path.
set -euo pipefail
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/verification/verify-container.sh" "$@"
