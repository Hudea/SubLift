#!/usr/bin/env bash
# ==============================================================================
# scripts/test_determinism.sh
# E2E Tier 3 Test: Multi-Threading Concurrency Determinism
#
# Runs extraction on a sample video across different thread configurations
# (--threads 1, --threads 0, --threads 8) and verifies that all resulting SRT
# files have 100% bit-identical SHA-256 checksums.
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
THREADS_LIST=(1 0 8)
REGION_ARG="--region 0,848,1920,87"
USE_FULL=false
EXTRA_ARGS=()

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
    --threads-list)
      IFS=' ' read -r -a THREADS_LIST <<< "$2"
      shift 2
      ;;
    --no-region)
      REGION_ARG=""
      shift
      ;;
    --region)
      REGION_ARG="--region $2"
      shift 2
      ;;
    --full)
      USE_FULL=true
      shift
      ;;
    -h|--help)
      cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --cli PATH           Path to sublift_cli binary (auto-detected by default)
  --video PATH         Path to sample video (default: debug/smoke-test-data/Zootopia_clip_1080p.mp4)
  --threads-list "1 0 8" Space-separated list of thread counts to test (default: "1 0 8")
  --region "x,y,w,h"   Region coordinates (default: 0,848,1920,87)
  --no-region          Disable region cropping (run full bottom crop)
  --full               Run determinism test on the full video instead of a 40s clip
  -h, --help           Show this help message
EOF
      exit 0
      ;;
    *)
      EXTRA_ARGS+=("$1")
      shift
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

mkdir -p "${PROJECT_ROOT}/build"
TEMP_DIR="$(mktemp -d "${PROJECT_ROOT}/build/tmp_determinism_XXXXXX")"
cleanup() {
  rm -rf "${TEMP_DIR}"
}
trap cleanup EXIT INT TERM

INPUT_VIDEO="${SAMPLE_VIDEO}"
CLIP_DESC="full video"
if [[ "${USE_FULL}" != "true" ]]; then
  INPUT_VIDEO="${TEMP_DIR}/clip_40s.mp4"
  CLIP_DESC="40s clip (-ss 14 -t 40)"
  if ! command -v ffmpeg &>/dev/null; then
    echo "[-] ERROR: ffmpeg not found (needed to slice 40s determinism clip; pass --full to run full video)" >&2
    exit 1
  fi
  ffmpeg -nostdin -v error -y -ss 14 -t 40 -i "${SAMPLE_VIDEO}" -c copy "${INPUT_VIDEO}"
fi

echo "======================================================================"
echo "[+] Running Tier 3: Multi-Threading Concurrency Determinism"
echo "    CLI binary: ${CLI_BIN}"
echo "    Video fixture: ${SAMPLE_VIDEO} (${CLIP_DESC})"
echo "    Thread configurations: ${THREADS_LIST[*]}"
echo "    Region mode: ${REGION_ARG:-None (full bottom ratio)}"
echo "======================================================================"

declare -a SRT_HASHES=()
declare -a SRT_FILES=()
declare -a TESTED_THREADS=()
FIRST_HASH=""
ALL_IDENTICAL=true

compute_sha256() {
  local file="$1"
  if command -v sha256sum &>/dev/null; then
    sha256sum "${file}" | awk '{print $1}'
  elif command -v shasum &>/dev/null; then
    shasum -a 256 "${file}" | awk '{print $1}'
  else
    python3 -c "import hashlib, sys; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "${file}"
  fi
}

for th in "${THREADS_LIST[@]}"; do
  echo -n "  Running extraction with --threads ${th} ... "
  OUT_SRT="${TEMP_DIR}/out_th_${th}.srt"
  LOG_FILE="${TEMP_DIR}/out_th_${th}.log"

  CMD_ARGS=("${CLI_BIN}" "${INPUT_VIDEO}" -o "${OUT_SRT}" --threads "${th}" --script cjk)
  if [[ -n "${REGION_ARG}" ]]; then
    # shellcheck disable=SC2206
    CMD_ARGS+=(${REGION_ARG})
  fi
  if [[ ${#EXTRA_ARGS[@]} -gt 0 ]]; then
    CMD_ARGS+=("${EXTRA_ARGS[@]}")
  fi

  T0=$(python3 -c 'import time; print(time.time())')
  set +e
  "${CMD_ARGS[@]}" >"${LOG_FILE}" 2>&1
  RC=$?
  set -e
  T1=$(python3 -c 'import time; print(time.time())')
  ELAPSED=$(python3 -c "print(round(${T1} - ${T0}, 2))")

  if [[ ${RC} -ne 0 || ! -s "${OUT_SRT}" ]]; then
    echo "FAIL (rc=${RC})"
    echo "    Log:"
    tail -n 10 "${LOG_FILE}" | sed 's/^/      /'
    exit 1
  fi

  HASH=$(compute_sha256 "${OUT_SRT}")
  CUES=$(grep -c -e "-->" "${OUT_SRT}" || true)
  SRT_HASHES+=("${HASH}")
  SRT_FILES+=("${OUT_SRT}")
  TESTED_THREADS+=("${th}")

  echo "OK (${ELAPSED}s, ${CUES} cues, sha256: ${HASH:0:16}...)"

  if [[ -z "${FIRST_HASH}" ]]; then
    FIRST_HASH="${HASH}"
  elif [[ "${HASH}" != "${FIRST_HASH}" ]]; then
    ALL_IDENTICAL=false
  fi
done

echo "----------------------------------------------------------------------"
echo "Determinism Comparison Matrix:"
for i in "${!TESTED_THREADS[@]}"; do
  printf "  threads=%-2s | sha256=%s\n" "${TESTED_THREADS[i]}" "${SRT_HASHES[i]}"
done

if [[ "${ALL_IDENTICAL}" != "true" ]]; then
  echo "[-] ERROR: Determinism failure: output SRT files are NOT bit-identical!" >&2
  # Diff between first config and differing configs
  for i in "${!TESTED_THREADS[@]}"; do
    if [[ "${SRT_HASHES[i]}" != "${FIRST_HASH}" ]]; then
      echo "    Diff between --threads ${TESTED_THREADS[0]} and --threads ${TESTED_THREADS[i]}:"
      diff -u "${SRT_FILES[0]}" "${SRT_FILES[i]}" | head -n 30 | sed 's/^/      /' || true
    fi
  done
  exit 1
fi

echo "======================================================================"
echo "[+] Concurrency Determinism PASSED: 100% bit-identical SHA-256 hashes"
exit 0
