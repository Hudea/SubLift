#!/usr/bin/env bash
# ==============================================================================
# scripts/test_sigint.sh
# E2E Tier 3 Test: Asynchronous SIGINT Safe Termination & Resource Cleanup
#
# Launches extraction on a sample video, sends SIGINT (Ctrl+C) mid-stream at
# defined delay checkpoints (3s, 10s), and verifies:
# 1. Exit code is exactly 130 (or signal-safe clean exit).
# 2. Process termination latency is strictly < 2.0 seconds.
# 3. Exactly 0 orphaned ffmpeg child processes remain active.
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

# Resolve Sample Video
SAMPLE_VIDEO="${PROJECT_ROOT}/debug/smoke-test-data/Zootopia_clip_1080p.mp4"
DELAYS=(3 10)
MAX_LATENCY="2.0"

# Parse CLI options
while [[ $# -gt 0 ]]; do
  case "$1" in
    --cli)
      CLI_BIN="$2"
      shift 2
      ;;
    --video)
      SAMPLE_VIDEO="$2"
      shift 2
      ;;
    --delays)
      # e.g. --delays "3 10"
      IFS=' ' read -r -a DELAYS <<< "$2"
      shift 2
      ;;
    --max-latency)
      MAX_LATENCY="$2"
      shift 2
      ;;
    -h|--help)
      cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --cli PATH          Path to sublift_cli binary (auto-detected by default)
  --video PATH        Path to sample video (default: debug/smoke-test-data/Zootopia_clip_1080p.mp4)
  --delays "D1 D2"    Space-separated delays in seconds before sending SIGINT (default: "3 10")
  --max-latency SEC   Maximum allowed exit latency in seconds (default: 2.0)
  -h, --help          Show this help message
EOF
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
  esac
done

if [[ -z "${CLI_BIN}" || ! -x "${CLI_BIN}" ]]; then
  echo "[-] ERROR: sublift_cli binary not found or not executable: '${CLI_BIN}'" >&2
  exit 1
fi

if [[ ! -f "${SAMPLE_VIDEO}" ]]; then
  echo "[-] ERROR: Sample video not found: '${SAMPLE_VIDEO}'" >&2
  exit 1
fi

TEMP_DIR="$(mktemp -d -t sublift_sigint_test_XXXXXX)"
cleanup() {
  local pids
  pids=$(pgrep -f "ffmpeg.*Zootopia" || true)
  if [[ -n "${pids}" ]]; then
    kill -9 ${pids} 2>/dev/null || true
  fi
  rm -rf "${TEMP_DIR}"
}
trap cleanup EXIT INT TERM

echo "======================================================================"
echo "[+] Running Tier 3: Asynchronous SIGINT Safe Termination"
echo "    CLI binary: ${CLI_BIN}"
echo "    Video fixture: ${SAMPLE_VIDEO}"
echo "    Delay checkpoints: ${DELAYS[*]} seconds"
echo "    Max latency limit: ${MAX_LATENCY}s"
echo "======================================================================"

TOTAL_TESTS=0
FAILED_TESTS=0
VIDEO_BASENAME="$(basename "${SAMPLE_VIDEO}")"

for delay in "${DELAYS[@]}"; do
  TOTAL_TESTS=$((TOTAL_TESTS + 1))
  echo -n "  Testing SIGINT after ${delay}s delay ... "

  OUT_SRT="${TEMP_DIR}/sigint_${delay}.srt"
  LOG_FILE="${TEMP_DIR}/sigint_${delay}.log"

  # Launch CLI in background
  "${CLI_BIN}" "${SAMPLE_VIDEO}" -o "${OUT_SRT}" --script cjk >"${LOG_FILE}" 2>&1 &
  PID=$!

  # Sleep for the designated delay
  sleep "${delay}"

  # Verify process is still alive before signaling
  if ! kill -0 "${PID}" 2>/dev/null; then
    echo "FAIL (process already exited prematurely before delay ${delay}s)"
    tail -n 10 "${LOG_FILE}" | sed 's/^/      /'
    FAILED_TESTS=$((FAILED_TESTS + 1))
    continue
  fi

  # Record timestamp immediately before SIGINT
  T0=$(python3 -c 'import time; print(time.time())')
  kill -INT "${PID}"

  # Poll for process termination (timeout 10s)
  HUNG=false
  for _ in {1..100}; do
    if ! kill -0 "${PID}" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done

  if kill -0 "${PID}" 2>/dev/null; then
    HUNG=true
    kill -9 "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    RC=hung
  else
    if wait "${PID}" 2>/dev/null; then
      RC=0
    else
      RC=$?
    fi
  fi

  T1=$(python3 -c 'import time; print(time.time())')
  LATENCY=$(python3 -c "print(round(${T1} - ${T0}, 3))")

  # Allow small grace period for ffmpeg pipes to close
  sleep 0.5
  LEFTOVER_FFMPEG=$( (pgrep -f "ffmpeg.*${VIDEO_BASENAME}" || true) | wc -l | tr -d ' ')

  # Clean up any orphan ffmpeg processes immediately
  if [[ ${LEFTOVER_FFMPEG} -gt 0 ]]; then
    pkill -9 -f "ffmpeg.*${VIDEO_BASENAME}" 2>/dev/null || true
  fi

  # Assertions:
  # 1. Not hung
  # 2. Exit code == 130
  # 3. Latency < MAX_LATENCY
  # 4. LEFTOVER_FFMPEG == 0
  FAIL_REASON=""
  LATENCY_OK=$(python3 -c "print(1 if ${LATENCY} <= float('${MAX_LATENCY}') else 0)")

  if [[ "${HUNG}" == "true" ]]; then
    FAIL_REASON="Process hung > 10s after SIGINT"
  elif [[ "${RC}" != "130" ]]; then
    FAIL_REASON="Expected exit code 130, got ${RC}"
  elif [[ "${LATENCY_OK}" != "1" ]]; then
    FAIL_REASON="Latency ${LATENCY}s exceeded limit of ${MAX_LATENCY}s"
  fi

  if [[ -z "${FAIL_REASON}" && ${LEFTOVER_FFMPEG} -gt 0 ]]; then
    FAIL_REASON="Found ${LEFTOVER_FFMPEG} orphaned ffmpeg processes"
  fi

  if [[ -z "${FAIL_REASON}" ]]; then
    echo "PASS (rc=${RC}, latency=${LATENCY}s, ffmpeg_leftovers=0)"
  else
    echo "FAIL: ${FAIL_REASON} (rc=${RC}, latency=${LATENCY}s, ffmpeg_leftovers=${LEFTOVER_FFMPEG})"
    echo "    Last log output:"
    tail -n 6 "${LOG_FILE}" | sed 's/^/      /'
    FAILED_TESTS=$((FAILED_TESTS + 1))
  fi
done

echo "----------------------------------------------------------------------"
echo "SIGINT Results: $((TOTAL_TESTS - FAILED_TESTS))/${TOTAL_TESTS} checkpoints passed"
echo "======================================================================"

if [[ ${FAILED_TESTS} -gt 0 ]]; then
  echo "[-] SIGINT safe termination tests FAILED (${FAILED_TESTS} failures)" >&2
  exit 1
fi

echo "[+] SIGINT safe termination tests PASSED (100%)"
exit 0
