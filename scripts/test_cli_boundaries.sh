#!/usr/bin/env bash
# ==============================================================================
# scripts/test_cli_boundaries.sh
# E2E Tier 2 Test: CLI Boundary, Corner Conditions & Parameter Validation
#
# Verifies that sublift_cli rejects invalid arguments, enforces manifest
# security policies, and fails closed with descriptive errors and non-zero rc.
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
    -h|--help)
      cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --cli PATH      Path to sublift_cli binary (auto-detected by default)
  --video PATH    Path to sample video (default: debug/smoke-test-data/Zootopia_clip_1080p.mp4)
  -h, --help      Show this help message
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
  echo "    Please build sublift_cli or specify with --cli <path>" >&2
  exit 1
fi

if [[ ! -f "${SAMPLE_VIDEO}" ]]; then
  echo "[-] ERROR: Sample video file not found: '${SAMPLE_VIDEO}'" >&2
  echo "    Please ensure test assets are available or specify with --video <path>" >&2
  exit 1
fi

echo "======================================================================"
echo "[+] Running Tier 2: CLI Parameter & Boundary Validation"
echo "    CLI binary: ${CLI_BIN}"
echo "    Video fixture: ${SAMPLE_VIDEO}"
echo "======================================================================"

PASSED_COUNT=0
FAILED_COUNT=0

run_boundary_case() {
  local desc="$1"
  local expected_to_fail="$2" # "fail" means non-zero rc is expected
  shift 2
  local cmd=("$@")

  echo -n "  Testing: ${desc} ... "

  local tmp_out
  tmp_out="$(mktemp)"
  local rc=0

  # Run command in background with 10.0s timeout to detect if it entered extraction instead of rejecting
  "${cmd[@]}" >"${tmp_out}" 2>&1 &
  local pid=$!
  local timed_out=false

  for _ in {1..100}; do
    if ! kill -0 "${pid}" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done

  if kill -0 "${pid}" 2>/dev/null; then
    timed_out=true
    kill -9 "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
    pkill -9 -f "ffmpeg.*Zootopia" 2>/dev/null || true
    rc=124
  else
    if wait "${pid}" 2>/dev/null; then
      rc=0
    else
      rc=$?
    fi
  fi

  local success=false
  local crashed=false
  if [[ ${rc} -ge 128 && "${timed_out}" == "false" ]]; then
    crashed=true
  fi

  if [[ "${expected_to_fail}" == "fail" ]]; then
    if [[ "${timed_out}" == "true" ]]; then
      success=false
    elif [[ "${crashed}" == "true" ]]; then
      success=false
    elif [[ ${rc} -ne 0 ]]; then
      success=true
    fi
  else
    if [[ ${rc} -eq 0 && "${timed_out}" == "false" ]]; then
      success=true
    fi
  fi

  if [[ "${success}" == "true" ]]; then
    echo "PASS (rc=${rc})"
    PASSED_COUNT=$((PASSED_COUNT + 1))
  else
    if [[ "${timed_out}" == "true" ]]; then
      echo "FAIL (did not reject argument at parse time; entered execution)"
    elif [[ "${crashed}" == "true" ]]; then
      echo "FAIL (CRASH: process terminated with fatal signal, rc=${rc})"
    else
      echo "FAIL (expected ${expected_to_fail}, got rc=${rc})"
    fi
    echo "    Output:"
    sed 's/^/      /' "${tmp_out}" | head -n 10
    FAILED_COUNT=$((FAILED_COUNT + 1))
  fi
  rm -f "${tmp_out}"
}

# 1. No arguments (should show help or error and exit non-zero or show usage)
# In sublift_cli, running without args shows help and returns 0. Let's test missing video when options given.
run_boundary_case "Missing input video path" "fail" \
  "${CLI_BIN}" --fps 5.0 -o /tmp/boundary_out.srt

# 2. Non-existent input video
run_boundary_case "Non-existent video file" "fail" \
  "${CLI_BIN}" /tmp/nonexistent_video_path_xyz_1234.mp4

# 3. Malformed region - insufficient tokens (3 tokens)
run_boundary_case "Malformed region: 3 components" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --region 0,848,1920

# 4. Malformed region - non-numeric tokens
run_boundary_case "Malformed region: non-numeric tokens" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --region 0,abc,1920,87

# 5. Malformed region - non-positive width/height
run_boundary_case "Malformed region: zero width" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --region 0,848,0,87

# 6. Invalid confidence threshold (> 1.0)
run_boundary_case "Invalid confidence > 1.0" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --confidence 1.5

# 7. Invalid confidence threshold (< 0.0)
run_boundary_case "Invalid confidence < 0.0" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --confidence -0.5

# 8. Invalid bottom-ratio (>= 1.0)
run_boundary_case "Invalid bottom-ratio >= 1.0" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --bottom-ratio 1.2

# 9. Invalid bottom-ratio (<= 0.0)
run_boundary_case "Invalid bottom-ratio <= 0.0" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --bottom-ratio 0.0

# 10. Invalid threads (< 0)
run_boundary_case "Invalid threads < 0" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --threads -1

# 11. Illegal script option (Feature F10: auto|cjk|latin)
run_boundary_case "Illegal --script klingon rejected" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --script klingon

# 12. Non-existent model directory (Feature F1: Fail-closed on missing model/manifest)
run_boundary_case "Non-existent model directory fail-closed" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --model-dir /tmp/sublift_nonexistent_model_dir_9999

# 13. Missing video with --timing-only
run_boundary_case "Missing video with --timing-only rejected" "fail" \
  "${CLI_BIN}" /tmp/sublift_nonexistent_video_9999.mp4 --timing-only

# 14. Missing video with timing subcommand
run_boundary_case "Missing video with timing subcommand rejected" "fail" \
  "${CLI_BIN}" timing /tmp/sublift_nonexistent_video_9999.mp4

# 15. Unsupported hwaccel mode rejected
run_boundary_case "Invalid --hwaccel backend rejected" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --hwaccel invalid_backend

# 16. Inverted time range rejected
run_boundary_case "Inverted --start-ms/--end-ms rejected" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --start-ms 5000 --end-ms 3000

# 17. Valid --timing-only time-range slice with --hwaccel auto passes
run_boundary_case "Valid --timing-only time-range slice passes" "pass" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --timing-only --hwaccel auto --start-ms 500 --end-ms 3500 -o /tmp/boundary_timing_slice.jsonl

# 18. Unsupported skip-frame mode rejected
run_boundary_case "Invalid --skip-frame mode rejected" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --skip-frame invalid_mode

# 19. Valid --timing-only time-range slice with --skip-frame noref passes
run_boundary_case "Valid --timing-only --skip-frame noref slice passes" "pass" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --timing-only --skip-frame noref --start-ms 500 --end-ms 2500 -o /tmp/boundary_timing_slice.jsonl

# 20. --export-strips-dir without --timing-only rejected
run_boundary_case "--export-strips-dir requires --timing-only" "fail" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --export-strips-dir /tmp/boundary_strips_fail -o /tmp/boundary_out.srt

# 21. Valid --timing-only --export-strips-dir emits readable P6 PPM strip(s) when cues exist
rm -rf /tmp/boundary_strips_ok
run_boundary_case "Valid --timing-only --export-strips-dir writes PPM strips" "pass" \
  "${CLI_BIN}" "${SAMPLE_VIDEO}" --timing-only --region 0,848,1920,87 --start-ms 25500 --end-ms 29500 --export-strips-dir /tmp/boundary_strips_ok -o /tmp/boundary_timing_slice.jsonl

python3 - <<'PY'
import json
from pathlib import Path

jsonl_path = Path("/tmp/boundary_timing_slice.jsonl")
strips_dir = Path("/tmp/boundary_strips_ok")
slots = [
    json.loads(line)
    for line in jsonl_path.read_text(encoding="utf-8").splitlines()
    if line.strip()
]
if not slots:
    raise SystemExit("expected >= 1 timing slot in [25500..29500ms] slice")
ppms = sorted(strips_dir.glob("strip_*.ppm"))
if len(slots) != len(ppms):
    raise SystemExit(f"slot count {len(slots)} != strip ppm count {len(ppms)}")
for slot, ppm in zip(slots, ppms):
    expected = f"strip_{slot['start_ms']}_{slot['end_ms']}.ppm"
    if ppm.name != expected:
        raise SystemExit(f"expected {expected}, got {ppm.name}")
    raw = ppm.read_bytes()
    if not raw.startswith(b"P6\n"):
        raise SystemExit(f"invalid PPM header in {ppm}")
print(f"Verified {len(ppms)} PPM strip(s) match timing JSONL slots")
PY

# Clean any stray temp files
rm -rf /tmp/boundary_out.srt /tmp/boundary_timing_slice.jsonl /tmp/boundary_strips_fail /tmp/boundary_strips_ok

echo "----------------------------------------------------------------------"
echo "Tier 2 Results: ${PASSED_COUNT} passed, ${FAILED_COUNT} failed"
echo "======================================================================"

if [[ ${FAILED_COUNT} -gt 0 ]]; then
  echo "[-] Tier 2 CLI boundary tests FAILED (${FAILED_COUNT} failures)" >&2
  exit 1
fi

echo "[+] Tier 2 CLI boundary tests PASSED (100%)"
exit 0
