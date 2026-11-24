#!/usr/bin/env bash
# ==============================================================================
# scripts/run_e2e_tests.sh
# Master End-to-End Test Suite Runner for SubLift
#
# Supports hierarchical test tiers:
#   Tier 1: Feature Coverage Core (Dependency purity, CLI flags, CTest suite)
#   Tier 2: Boundary & Corner Conditions (Parameter validation, Fail-closed manifest)
#   Tier 3: Cross-Feature Interactions & Determinism (Threads determinism, SIGINT)
#   Tier 4: Real-World Workload & Benchmark (Zootopia 1080p benchmark, Parity)
#
# Usage:
#   ./scripts/run_e2e_tests.sh --all
#   ./scripts/run_e2e_tests.sh --tier 1
#   ./scripts/run_e2e_tests.sh --tier 2
#   ./scripts/run_e2e_tests.sh --tier 3
#   ./scripts/run_e2e_tests.sh --tier 4
#   ./scripts/run_e2e_tests.sh --tier 1 --tier 2
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Resolve CLI Binary
CLI_BIN="${SUBLIFT_CLI:-}"
if [[ -z "${CLI_BIN}" ]]; then
  if [[ -x "${PROJECT_ROOT}/build/release/tools/cli/sublift_cli" ]]; then
    CLI_BIN="${PROJECT_ROOT}/build/release/tools/cli/sublift_cli"
  elif [[ -x "${PROJECT_ROOT}/build/debug/tools/cli/sublift_cli" ]]; then
    CLI_BIN="${PROJECT_ROOT}/build/debug/tools/cli/sublift_cli"
  elif [[ -x "${PROJECT_ROOT}/build/tools/cli/sublift_cli" ]]; then
    CLI_BIN="${PROJECT_ROOT}/build/tools/cli/sublift_cli"
  elif command -v sublift_cli &>/dev/null; then
    CLI_BIN="$(command -v sublift_cli)"
  fi
fi

# Resolve Build Directory
if [[ -f "${PROJECT_ROOT}/build/release/CTestTestfile.cmake" ]]; then
  BUILD_DIR="${PROJECT_ROOT}/build/release"
elif [[ -f "${PROJECT_ROOT}/build/debug/CTestTestfile.cmake" ]]; then
  BUILD_DIR="${PROJECT_ROOT}/build/debug"
else
  BUILD_DIR="${PROJECT_ROOT}/build"
fi

RUN_TIER_1=false
RUN_TIER_2=false
RUN_TIER_3=false
RUN_TIER_4=false
SKIP_CTEST=false
ALLOW_PERF_WARN=false

# Parse arguments
if [[ $# -eq 0 ]]; then
  set -- "--help"
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --all)
      RUN_TIER_1=true
      RUN_TIER_2=true
      RUN_TIER_3=true
      RUN_TIER_4=true
      shift
      ;;
    --tier)
      if [[ $# -lt 2 ]]; then
        echo "[-] ERROR: --tier requires a tier number (1, 2, 3, or 4)" >&2
        exit 1
      fi
      case "$2" in
        1) RUN_TIER_1=true ;;
        2) RUN_TIER_2=true ;;
        3) RUN_TIER_3=true ;;
        4) RUN_TIER_4=true ;;
        *)
          echo "[-] ERROR: Invalid tier '$2'. Must be 1, 2, 3, or 4" >&2
          exit 1
          ;;
      esac
      shift 2
      ;;
    --cli)
      CLI_BIN="$2"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --skip-ctest)
      SKIP_CTEST=true
      shift
      ;;
    --allow-perf-warn)
      ALLOW_PERF_WARN=true
      shift
      ;;
    -h|--help)
      cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --all               Run all test tiers (Tier 1 through Tier 4)
  --tier <1|2|3|4>    Run specific tier (can be repeated, e.g. --tier 1 --tier 2)
  --cli PATH          Path to sublift_cli binary (auto-detected by default)
  --build-dir PATH    Path to CMake build directory (default: build/release, build/debug, or build)
  --skip-ctest        Skip CTest suite execution in Tier 1
  --allow-perf-warn   Treat performance limits in Tier 4 as warnings (useful in Debug)
  -h, --help          Show this help message

Test Tiers:
  Tier 1: Feature Coverage Core (Dependency purity, CLI help, CTest test suite)
  Tier 2: Boundary & Corner Conditions (CLI argument validation, Fail-closed checks)
  Tier 3: Concurrency & Cross-Feature (Deterministic multi-threading, SIGINT clean exit)
  Tier 4: Real-World Workload (Zootopia 1080p benchmark, Parity fixtures)
EOF
      exit 0
      ;;
    *)
      echo "[-] ERROR: Unknown option: $1" >&2
      exit 1
      ;;
  esac
done

echo "======================================================================"
echo "            SubLift Master E2E Test Suite Runner                     "
echo "======================================================================"
echo "Project Root: ${PROJECT_ROOT}"
echo "CLI Binary:   ${CLI_BIN:-<not found>}"
echo "Build Dir:    ${BUILD_DIR}"
echo "Active Tiers: $([[ "${RUN_TIER_1}" == "true" ]] && echo -n "Tier 1 ") $([[ "${RUN_TIER_2}" == "true" ]] && echo -n "Tier 2 ") $([[ "${RUN_TIER_3}" == "true" ]] && echo -n "Tier 3 ") $([[ "${RUN_TIER_4}" == "true" ]] && echo -n "Tier 4 ")"
echo "======================================================================"

declare -a TEST_NAMES=()
declare -a TEST_TIERS=()
declare -a TEST_STATUS=()
declare -a TEST_TIMES=()

RUN_STEP() {
  local tier="$1"
  local name="$2"
  shift 2
  local cmd=("$@")

  echo ""
  echo ">>> [Tier ${tier}] Running: ${name} ..."
  local t0
  t0=$(python3 -c 'import time; print(time.time())')
  local rc=0

  set +e
  "${cmd[@]}"
  rc=$?
  set -e

  local t1
  t1=$(python3 -c 'import time; print(time.time())')
  local elapsed
  elapsed=$(python3 -c "print(round(${t1} - ${t0}, 2))")

  TEST_NAMES+=("${name}")
  TEST_TIERS+=("${tier}")
  TEST_TIMES+=("${elapsed}s")

  if [[ ${rc} -eq 0 ]]; then
    echo ">>> [Tier ${tier}] ${name}: PASS (${elapsed}s)"
    TEST_STATUS+=("PASS")
  else
    echo ">>> [Tier ${tier}] ${name}: FAIL (rc=${rc}, ${elapsed}s)"
    TEST_STATUS+=("FAIL")
  fi
}

# ==============================================================================
# Tier 1: Feature Coverage Core
# ==============================================================================
if [[ "${RUN_TIER_1}" == "true" ]]; then
  echo ""
  echo "======================================================================"
  echo "                   TIER 1: FEATURE COVERAGE CORE                      "
  echo "======================================================================"

  # 1.1 Architecture Dependency Purity
  RUN_STEP 1 "Architecture Dependency Purity" \
    python3 "${SCRIPT_DIR}/check_dependencies.py"

  # 1.2 CLI Interface & Help Display
  if [[ -n "${CLI_BIN}" && -x "${CLI_BIN}" ]]; then
    RUN_STEP 1 "CLI Help Interface" \
      "${CLI_BIN}" --help
  else
    TEST_NAMES+=("CLI Help Interface")
    TEST_TIERS+=(1)
    TEST_TIMES+=("0.0s")
    TEST_STATUS+=("SKIP (CLI missing)")
  fi

  # 1.3 CTest Unit & Component Suite
  if [[ "${SKIP_CTEST}" == "true" ]]; then
    TEST_NAMES+=("CTest Unit Suite")
    TEST_TIERS+=(1)
    TEST_TIMES+=("0.0s")
    TEST_STATUS+=("SKIP (--skip-ctest)")
  elif [[ -d "${BUILD_DIR}" ]]; then
    RUN_STEP 1 "CTest Unit Suite" \
      ctest --test-dir "${BUILD_DIR}" --output-on-failure
  else
    TEST_NAMES+=("CTest Unit Suite")
    TEST_TIERS+=(1)
    TEST_TIMES+=("0.0s")
    TEST_STATUS+=("SKIP (Build dir missing)")
  fi
fi

# ==============================================================================
# Tier 2: Boundary & Corner Conditions
# ==============================================================================
if [[ "${RUN_TIER_2}" == "true" ]]; then
  echo ""
  echo "======================================================================"
  echo "               TIER 2: BOUNDARY & CORNER CONDITIONS                   "
  echo "======================================================================"

  if [[ -n "${CLI_BIN}" && -x "${CLI_BIN}" ]]; then
    RUN_STEP 2 "CLI Boundary & Parameter Validation" \
      "${SCRIPT_DIR}/test_cli_boundaries.sh" --cli "${CLI_BIN}"
  else
    echo "[-] Cannot run Tier 2: CLI binary not found or not executable"
    TEST_NAMES+=("CLI Boundary & Parameter Validation")
    TEST_TIERS+=(2)
    TEST_TIMES+=("0.0s")
    TEST_STATUS+=("FAIL (CLI missing)")
  fi
fi

# ==============================================================================
# Tier 3: Cross-Feature Interactions & Concurrency
# ==============================================================================
if [[ "${RUN_TIER_3}" == "true" ]]; then
  echo ""
  echo "======================================================================"
  echo "          TIER 3: CONCURRENCY & CROSS-FEATURE INTERACTIONS            "
  echo "======================================================================"

  if [[ -n "${CLI_BIN}" && -x "${CLI_BIN}" ]]; then
    # 3.1 Multi-Threading Determinism
    RUN_STEP 3 "Multi-Threading Concurrency Determinism" \
      "${SCRIPT_DIR}/test_determinism.sh" --cli "${CLI_BIN}"

    # 3.2 Asynchronous SIGINT Safe Termination
    RUN_STEP 3 "Asynchronous SIGINT Safe Exit" \
      "${SCRIPT_DIR}/test_sigint.sh" --cli "${CLI_BIN}"
  else
    echo "[-] Cannot run Tier 3: CLI binary not found or not executable"
    TEST_NAMES+=("Multi-Threading Concurrency Determinism")
    TEST_TIERS+=(3)
    TEST_TIMES+=("0.0s")
    TEST_STATUS+=("FAIL (CLI missing)")

    TEST_NAMES+=("Asynchronous SIGINT Safe Exit")
    TEST_TIERS+=(3)
    TEST_TIMES+=("0.0s")
    TEST_STATUS+=("FAIL (CLI missing)")
  fi
fi

# ==============================================================================
# Tier 4: Real-World Workload & Benchmark Acceptance
# ==============================================================================
if [[ "${RUN_TIER_4}" == "true" ]]; then
  echo ""
  echo "======================================================================"
  echo "               TIER 4: REAL-WORLD WORKLOAD & BENCHMARK                "
  echo "======================================================================"

  # 4.1 Zootopia Quality & Performance Benchmark
  if [[ -n "${CLI_BIN}" && -x "${CLI_BIN}" ]]; then
    BENCH_CMD=("${SCRIPT_DIR}/run_e2e_benchmark.sh" --cli "${CLI_BIN}")
    if [[ "${ALLOW_PERF_WARN}" == "true" ]]; then
      BENCH_CMD+=("--allow-perf-warn")
    fi
    RUN_STEP 4 "Zootopia 1080p Benchmark" "${BENCH_CMD[@]}"
  else
    echo "[-] Cannot run Tier 4: CLI binary not found or not executable"
    TEST_NAMES+=("Zootopia 1080p Benchmark")
    TEST_TIERS+=(4)
    TEST_TIMES+=("0.0s")
    TEST_STATUS+=("FAIL (CLI missing)")
  fi
fi

# ==============================================================================
# Test Execution Summary
# ==============================================================================
echo ""
echo "======================================================================"
echo "                     E2E TEST SUMMARY REPORT                          "
echo "======================================================================"
printf "%-6s | %-42s | %-12s | %s\n" "Tier" "Test Name" "Duration" "Status"
echo "----------------------------------------------------------------------"

ALL_PASSED=true
for i in "${!TEST_NAMES[@]}"; do
  printf "Tier %-1s | %-42s | %-12s | %s\n" \
    "${TEST_TIERS[i]}" \
    "${TEST_NAMES[i]}" \
    "${TEST_TIMES[i]}" \
    "${TEST_STATUS[i]}"
  if [[ "${TEST_STATUS[i]}" =~ FAIL ]]; then
    ALL_PASSED=false
  fi
done

echo "======================================================================"
if [[ "${ALL_PASSED}" == "true" ]]; then
  echo "[+] SUCCESS: All executed E2E test suites PASSED."
  exit 0
else
  echo "[-] FAILURE: One or more E2E test suites FAILED." >&2
  exit 1
fi
