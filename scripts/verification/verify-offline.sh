#!/usr/bin/env bash
# Isolated offline-tool verification (benchmark, scoring, frozen Oracle).
#
# This is NOT the product gate. Product verification is ./scripts/verify-product.sh
# and must remain Python-free. This entry installs optional Python extras and
# never writes docs/plans or docs/reports.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

pass=0
fail=0
UV=(uv run)

log_ok() { printf "${GREEN}[OK]${NC}  %s\n" "$1"; pass=$((pass + 1)); }
log_fail() { printf "${RED}[FAIL]${NC} %s\n" "$1"; fail=$((fail + 1)); }

run_or_fail() {
    local label="$1"
    shift
    if "$@" >/tmp/sublift_offline_verify.log 2>&1; then
        log_ok "$label"
        return 0
    fi
    log_fail "$label"
    cat /tmp/sublift_offline_verify.log 2>/dev/null || true
    return 0
}

echo "=============================="
echo " SubLift isolated offline tools"
echo "=============================="
echo
echo "Not a product gate. Outputs stay under debug/benchmark/."
echo

if ! command -v uv >/dev/null 2>&1; then
    log_fail "uv is required for isolated offline tools"
    echo
    echo "result: $pass passed, $fail failed"
    exit 1
fi
log_ok "tool uv -> $(command -v uv)"

echo
echo "=============================="
echo " Optional Python extras"
echo "=============================="
echo
if uv sync --extra oracle --extra vision --extra paddle >/tmp/sublift_offline_verify.log 2>&1; then
    log_ok "uv sync --extra oracle --extra vision --extra paddle"
else
    log_fail "uv sync --extra oracle --extra vision --extra paddle"
    cat /tmp/sublift_offline_verify.log
fi

echo
echo "=============================="
echo " Namespace and product command"
echo "=============================="
echo

if "${UV[@]}" python -m sublift >/tmp/sublift_offline_verify.log 2>&1; then
    log_fail "python -m sublift should exit 2"
else
    code=$?
    if [ "$code" = "2" ]; then
        log_ok "python -m sublift exits 2"
    else
        log_fail "python -m sublift exits 2 (got $code)"
        cat /tmp/sublift_offline_verify.log
    fi
fi

if "${UV[@]}" python -c '
from importlib.metadata import entry_points
names = {ep.name for ep in entry_points(group="console_scripts")}
assert "sublift" not in names, names
assert "sublift-benchmark" in names
assert "sublift-offline" in names
' >/tmp/sublift_offline_verify.log 2>&1; then
    log_ok "console scripts: sublift-benchmark / sublift-offline; no product sublift"
else
    log_fail "console scripts occupy product sublift or missing offline entries"
    cat /tmp/sublift_offline_verify.log
fi

run_or_fail "python -m sublift_offline --help" "${UV[@]}" python -m sublift_offline --help
run_or_fail "sublift-benchmark --help" "${UV[@]}" sublift-benchmark --help

echo
echo "=============================="
echo " Import isolation + static + tests"
echo "=============================="
echo

run_or_fail "ruff check ." "${UV[@]}" ruff check .
run_or_fail "mypy src tests" "${UV[@]}" mypy src tests
run_or_fail "scoring import isolation" "${UV[@]}" pytest tests/test_benchmark_isolation.py --no-cov
run_or_fail "pytest (not integration)" "${UV[@]}" pytest -m "not integration" --no-cov

echo
echo "=============================="
echo " Default output directory"
echo "=============================="
echo

if "${UV[@]}" python -c '
from pathlib import Path
from sublift.benchmark.config import RunConfig
assert RunConfig(video_path=Path("v.mp4"), ground_truth_path=Path("g.srt")).output_dir == Path("debug/benchmark/runs")
' >/tmp/sublift_offline_verify.log 2>&1; then
    log_ok "default run output is debug/benchmark/runs"
else
    log_fail "default run output is debug/benchmark/runs"
    cat /tmp/sublift_offline_verify.log
fi

if [ -d docs/plans ] || [ -d docs/reports ]; then
    log_fail "docs/plans or docs/reports exists; offline tools must not recreate process archives"
else
    log_ok "docs/plans and docs/reports remain absent"
fi

echo
echo "=============================="
echo " Offline tools result"
echo "=============================="
echo
if [ "$fail" -eq 0 ]; then
    printf "${GREEN}passed %s / failed %s${NC}\n" "$pass" "$fail"
    exit 0
fi
printf "${RED}passed %s / failed %s${NC}\n" "$pass" "$fail"
exit 1
