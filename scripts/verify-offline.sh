#!/usr/bin/env bash
# Thin wrapper for isolated offline-tool verification.
set -euo pipefail
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/verification/verify-offline.sh" "$@"
