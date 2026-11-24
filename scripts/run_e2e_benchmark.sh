#!/usr/bin/env bash
# ==============================================================================
# scripts/run_e2e_benchmark.sh
# E2E Tier 4 Benchmark: Zootopia 1080p Quality & Performance Acceptance Gate
#
# Evaluates SubLift on the 254s 1080p Zootopia clip across:
# 1. Default Path: Full frame bottom crop pushdown (--script cjk)
#    - TP >= 70
#    - Timing F1 >= 0.895
#    - CER <= 9.0%
#    - Wall-clock time <= 45.0s (Release)
#    - Peak RSS <= 650MB
#    - Multi-sentence & comma preservation ("可以收工啦，谢啰，长舌妇" & "在ZPD，搭档合作是成功的基石")
# 2. Region Path: Precise coordinate crop (--region 0,848,1920,87 --script cjk)
#    - Timing F1 >= 0.990
#    - CER <= 4.0%
#    - Exactly 87 cues produced
#    - Wall-clock time <= 20.0s (Release)
#    - Peak RSS <= 450MB
# 3. Auto-Region Path: Automatic subtitle region pre-detection (--auto-region --script cjk)
#    - True Positives (TP) >= 80
#    - Timing F1 >= 0.980
#    - Matched CER <= 4.0%
#    - Wall-clock time <= 22.0s (Release)
#    - Peak RSS <= 550MB
#    - Multi-sentence & comma preservation ("可以收工啦，谢啰，长舌妇" & "在ZPD，搭档合作是成功的基石")
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

# Resolve Test Assets
DATA_DIR="${SUBLIFT_TEST_DATA_DIR:-}"
if [[ -z "${DATA_DIR}" ]]; then
  if [[ -d "${PROJECT_ROOT}/debug/smoke-test-data" ]]; then
    DATA_DIR="${PROJECT_ROOT}/debug/smoke-test-data"
  elif [[ -d "${PROJECT_ROOT}/../../debug/smoke-test-data" ]]; then
    DATA_DIR="${PROJECT_ROOT}/../../debug/smoke-test-data"
  elif [[ -d "${PROJECT_ROOT}/../debug/smoke-test-data" ]]; then
    DATA_DIR="${PROJECT_ROOT}/../debug/smoke-test-data"
  else
    DATA_DIR="${PROJECT_ROOT}/debug/smoke-test-data"
  fi
fi
SAMPLE_VIDEO="${DATA_DIR}/Zootopia_clip_1080p.mp4"
GT_SRT="${DATA_DIR}/Zootopia_clip_1080p.srt"

RUN_DEFAULT=true
RUN_REGION=true
RUN_AUTOREGION=true
ALLOW_PERF_WARN=false
OUT_DIR=""

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
    --gt)
      GT_SRT="$2"
      shift 2
      ;;
    --default-only)
      RUN_DEFAULT=true
      RUN_REGION=false
      RUN_AUTOREGION=false
      shift
      ;;
    --region-only)
      RUN_DEFAULT=false
      RUN_REGION=true
      RUN_AUTOREGION=false
      shift
      ;;
    --auto-region-only)
      RUN_DEFAULT=false
      RUN_REGION=false
      RUN_AUTOREGION=true
      shift
      ;;
    --allow-perf-warn)
      ALLOW_PERF_WARN=true
      shift
      ;;
    --out-dir)
      OUT_DIR="$2"
      shift 2
      ;;
    -h|--help)
      cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --cli PATH          Path to sublift_cli binary (auto-detected by default)
  --video PATH        Path to test video (default: debug/smoke-test-data/Zootopia_clip_1080p.mp4)
  --gt PATH           Path to ground-truth SRT (default: debug/smoke-test-data/Zootopia_clip_1080p.srt)
  --default-only      Run only Default path benchmark
  --region-only       Run only Region path benchmark
  --auto-region-only  Run only Auto-Region path benchmark
  --allow-perf-warn   Treat wall-clock time & RSS overruns as warnings (e.g. Debug build)
  --out-dir DIR       Directory to save benchmark output SRTs and logs
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
  echo "[-] ERROR: Benchmark video not found: '${SAMPLE_VIDEO}'" >&2
  exit 1
fi

if [[ ! -f "${GT_SRT}" ]]; then
  echo "[-] ERROR: Ground-truth SRT not found: '${GT_SRT}'" >&2
  exit 1
fi

GATES_FILE="${PROJECT_ROOT}/benchmark/gates.json"
if [[ ! -f "${GATES_FILE}" ]]; then
  echo "[-] ERROR: Gates config not found: '${GATES_FILE}'" >&2
  exit 1
fi

# Load centralized thresholds from benchmark/gates.json
eval "$(python3 - "${GATES_FILE}" <<'EOF'
import json, shlex, sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    cfg = json.load(f)

tracks = cfg["tracks"]
d = tracks["default"]
r = tracks["region"]
a = tracks["auto_region"]

def emit(k, v):
    print(f"{k}={shlex.quote(str(v))}")

emit("GATE_DEF_TP_MIN", d["tp_min"])
emit("GATE_DEF_F1_MIN", d["f1_min"])
emit("GATE_DEF_CER_MAX", d["cer_matched_max"])
emit("GATE_DEF_WALL_MAX", d["wall_sec_max"])
emit("GATE_DEF_RSS_MAX", int(d["rss_mb_max"]))
emit("GATE_DEF_CLAUSES_JSON", json.dumps(d.get("required_intact_clauses", []), ensure_ascii=False))

emit("GATE_REG_ARG", ",".join(str(x) for x in r["region"]))
emit("GATE_REG_CUES_EXACT", r["cues_exact"])
emit("GATE_REG_F1_MIN", r["f1_min"])
emit("GATE_REG_CER_MAX", r["cer_matched_max"])
emit("GATE_REG_WALL_MAX", r["wall_sec_max"])
emit("GATE_REG_RSS_MAX", int(r["rss_mb_max"]))

emit("GATE_AUTO_TP_MIN", a.get("tp_min", 80))
emit("GATE_AUTO_F1_MIN", a["f1_min"])
emit("GATE_AUTO_CER_MAX", a["cer_matched_max"])
emit("GATE_AUTO_WALL_MAX", a["wall_sec_max"])
emit("GATE_AUTO_RSS_MAX", int(a["rss_mb_max"]))
emit("GATE_AUTO_CLAUSES_JSON", json.dumps(a.get("required_intact_clauses", d.get("required_intact_clauses", [])), ensure_ascii=False))
EOF
)"

if [[ -z "${OUT_DIR}" ]]; then
  mkdir -p "${PROJECT_ROOT}/build"
  OUT_DIR="$(mktemp -d "${PROJECT_ROOT}/build/e2e_bench_XXXXXX")"
  CLEANUP_OUT_DIR=true
else
  mkdir -p "${OUT_DIR}"
  CLEANUP_OUT_DIR=false
fi

cleanup() {
  if [[ "${CLEANUP_OUT_DIR}" == "true" ]]; then
    rm -rf "${OUT_DIR}"
  fi
}
trap cleanup EXIT INT TERM

echo "======================================================================"
echo "[+] Running Tier 4: Zootopia 1080p Quality & Performance Benchmark"
echo "    CLI binary:   ${CLI_BIN}"
echo "    Video file:   ${SAMPLE_VIDEO}"
echo "    Ground Truth: ${GT_SRT}"
echo "    Artifact dir: ${OUT_DIR}"
echo "======================================================================"

MEASURE_TIME_RSS() {
  local time_out="$1"
  shift
  if [[ "$(uname -s)" == "Darwin" ]]; then
    /usr/bin/time -l "$@" 2>"${time_out}"
  else
    /usr/bin/time -v "$@" 2>"${time_out}"
  fi
}

EXTRACT_METRICS() {
  local time_file="$1"
  local srt_file="$2"

  python3 - <<EOF
import json, re, sys, os

time_file = "${time_file}"
srt_file = "${srt_file}"
gt_file = "${GT_SRT}"
eval_script = "${SCRIPT_DIR}/srt_eval.py"

rss_mb = 0
wall_sec = 0.0

if os.path.exists(time_file):
    with open(time_file, 'r', errors='ignore') as f:
        tdata = f.read()
    m_real = re.search(r'([\d.]+)\s*real', tdata)
    if m_real:
        wall_sec = float(m_real.group(1))
    else:
        m_elapsed = re.search(r'Elapsed \(wall clock\) time.*:\s*([\d:.]+)', tdata)
        if m_elapsed:
            parts = m_elapsed.group(1).split(':')
            if len(parts) == 2:
                wall_sec = float(parts[0]) * 60 + float(parts[1])
            elif len(parts) == 3:
                wall_sec = float(parts[0]) * 3600 + float(parts[1]) * 60 + float(parts[2])

    m_rss = re.search(r'(\d+)\s+maximum resident', tdata)
    if m_rss:
        raw_rss = int(m_rss.group(1))
        # On macOS, time -l gives bytes. On Linux time -v gives KB.
        if sys.platform == 'darwin':
            rss_mb = round(raw_rss / (1024 * 1024))
        else:
            rss_mb = round(raw_rss / 1024)

# Call srt_eval.py
import subprocess
res = subprocess.run([sys.executable, eval_script, gt_file, srt_file, "--json"], capture_output=True, text=True)
eval_data = {}
if res.returncode == 0:
    try:
        eval_data = json.loads(res.stdout).get("eval", {})
    except Exception:
        pass

out = {
    "wall_sec": wall_sec,
    "rss_mb": rss_mb,
    "cues": eval_data.get("pred", 0),
    "tp": eval_data.get("tp", 0),
    "precision": eval_data.get("precision", 0.0),
    "recall": eval_data.get("recall", 0.0),
    "f1": eval_data.get("f1", 0.0),
    "cer": eval_data.get("cer_matched", 1.0),
    "exact_text": eval_data.get("exact_text", 0),
}
print(json.dumps(out))
EOF
}

CHECK_MULTI_SENTENCE() {
  local srt_file="$1"
  local clauses_json="$2"
  local check_res
  check_res=$(python3 - "${srt_file}" "${clauses_json}" <<'EOF'
import json, re, sys

srt_path, clauses_raw = sys.argv[1], sys.argv[2]
clauses = json.loads(clauses_raw)
found = [False] * len(clauses)

with open(srt_path, "r", encoding="utf-8-sig", errors="replace") as f:
    content = f.read()

blocks = re.split(r"\r?\n\s*\r?\n", content.strip())
for b in blocks:
    lines = [line.strip() for line in b.splitlines() if line.strip()]
    for idx, line in enumerate(lines):
        if "-->" in line:
            text = "".join(lines[idx + 1:])
            for ci, parts in enumerate(clauses):
                if all(p in text for p in parts):
                    found[ci] = True
            break

if all(found):
    print("true")
else:
    print(f"false ({found})")
EOF
)
  if [[ "${check_res}" == "true" ]]; then
    echo "      Multi-sentence Clauses Check: PASS (all clauses preserved in cues)"
    return 0
  else
    echo "      Multi-sentence Clauses Check: FAIL (${check_res})"
    return 1
  fi
}

OVERALL_SUCCESS=true

# ------------------------------------------------------------------------------
# 1. Benchmark Default Path
# ------------------------------------------------------------------------------
if [[ "${RUN_DEFAULT}" == "true" ]]; then
  echo ""
  echo "--- [1/3] Benchmarking Default Path (--script cjk) ---"
  DEF_SRT="${OUT_DIR}/zootopia_default.srt"
  DEF_LOG="${OUT_DIR}/zootopia_default.log"
  DEF_TIME="${OUT_DIR}/zootopia_default.time"

  set +e
  MEASURE_TIME_RSS "${DEF_TIME}" "${CLI_BIN}" "${SAMPLE_VIDEO}" -o "${DEF_SRT}" --script cjk >"${DEF_LOG}"
  DEF_RC=$?
  set -e

  if [[ ${DEF_RC} -ne 0 || ! -s "${DEF_SRT}" ]]; then
    echo "[-] Default path extraction failed (rc=${DEF_RC})"
    tail -n 15 "${DEF_LOG}" | sed 's/^/    /'
    OVERALL_SUCCESS=false
  else
    DEF_METRICS=$(EXTRACT_METRICS "${DEF_TIME}" "${DEF_SRT}")
    DEF_WALL=$(python3 -c "import json; print(json.loads('''${DEF_METRICS}''')['wall_sec'])")
    DEF_RSS=$(python3 -c "import json; print(json.loads('''${DEF_METRICS}''')['rss_mb'])")
    DEF_CUES=$(python3 -c "import json; print(json.loads('''${DEF_METRICS}''')['cues'])")
    DEF_TP=$(python3 -c "import json; print(json.loads('''${DEF_METRICS}''')['tp'])")
    DEF_F1=$(python3 -c "import json; print(json.loads('''${DEF_METRICS}''')['f1'])")
    DEF_CER=$(python3 -c "import json; print(json.loads('''${DEF_METRICS}''')['cer'])")
    DEF_EXACT=$(python3 -c "import json; print(json.loads('''${DEF_METRICS}''')['exact_text'])")

    echo "    Results:"
    echo "      Wall Time:  ${DEF_WALL}s   (Target: <= ${GATE_DEF_WALL_MAX}s)"
    echo "      Peak RSS:   ${DEF_RSS}MB    (Target: <= ${GATE_DEF_RSS_MAX}MB)"
    echo "      Pred Cues:  ${DEF_CUES}"
    echo "      TP:         ${DEF_TP}       (Target: >= ${GATE_DEF_TP_MIN})"
    echo "      Time F1:    ${DEF_F1}      (Target: >= ${GATE_DEF_F1_MIN})"
    echo "      Matched CER: $(python3 -c "print(round(${DEF_CER} * 100, 2))")% (${DEF_CER}) (Target: <= $(python3 -c "print(round(${GATE_DEF_CER_MAX} * 100, 1))")%)"
    echo "      Exact Cues: ${DEF_EXACT} / ${GATE_REG_CUES_EXACT}"

    # Check multi-sentence comma truncation elimination
    CHECK_MULTI_SENTENCE "${DEF_SRT}" "${GATE_DEF_CLAUSES_JSON}" || OVERALL_SUCCESS=false

    # Gate verification
    if python3 -c "import sys; sys.exit(0 if ${DEF_TP} >= ${GATE_DEF_TP_MIN} else 1)"; then
      echo "      [✓] Gate TP >= ${GATE_DEF_TP_MIN}: PASS"
    else
      echo "      [✗] Gate TP >= ${GATE_DEF_TP_MIN}: FAIL (${DEF_TP} < ${GATE_DEF_TP_MIN})"
      OVERALL_SUCCESS=false
    fi

    if python3 -c "import sys; sys.exit(0 if ${DEF_F1} >= ${GATE_DEF_F1_MIN} else 1)"; then
      echo "      [✓] Gate F1 >= ${GATE_DEF_F1_MIN}: PASS"
    else
      echo "      [✗] Gate F1 >= ${GATE_DEF_F1_MIN}: FAIL (${DEF_F1} < ${GATE_DEF_F1_MIN})"
      OVERALL_SUCCESS=false
    fi

    if python3 -c "import sys; sys.exit(0 if ${DEF_CER} <= ${GATE_DEF_CER_MAX} else 1)"; then
      echo "      [✓] Gate CER <= ${GATE_DEF_CER_MAX}: PASS"
    else
      echo "      [✗] Gate CER <= ${GATE_DEF_CER_MAX}: FAIL (${DEF_CER} > ${GATE_DEF_CER_MAX})"
      OVERALL_SUCCESS=false
    fi

    if python3 -c "import sys; sys.exit(0 if ${DEF_WALL} <= ${GATE_DEF_WALL_MAX} else 1)"; then
      echo "      [✓] Gate Wall Time <= ${GATE_DEF_WALL_MAX}s: PASS"
    elif [[ "${ALLOW_PERF_WARN}" == "true" ]]; then
      echo "      [!] Gate Wall Time <= ${GATE_DEF_WALL_MAX}s: WARN (${DEF_WALL}s > ${GATE_DEF_WALL_MAX}s, non-fatal due to --allow-perf-warn)"
    else
      echo "      [✗] Gate Wall Time <= ${GATE_DEF_WALL_MAX}s: FAIL (${DEF_WALL}s > ${GATE_DEF_WALL_MAX}s)"
      OVERALL_SUCCESS=false
    fi

    if python3 -c "import sys; sys.exit(0 if ${DEF_RSS} <= ${GATE_DEF_RSS_MAX} else 1)"; then
      echo "      [✓] Gate Peak RSS <= ${GATE_DEF_RSS_MAX}MB: PASS"
    elif [[ "${ALLOW_PERF_WARN}" == "true" ]]; then
      echo "      [!] Gate Peak RSS <= ${GATE_DEF_RSS_MAX}MB: WARN (${DEF_RSS}MB > ${GATE_DEF_RSS_MAX}MB, non-fatal due to --allow-perf-warn)"
    else
      echo "      [✗] Gate Peak RSS <= ${GATE_DEF_RSS_MAX}MB: FAIL (${DEF_RSS}MB > ${GATE_DEF_RSS_MAX}MB)"
      OVERALL_SUCCESS=false
    fi
  fi
fi

# ------------------------------------------------------------------------------
# 2. Benchmark Region Path
# ------------------------------------------------------------------------------
if [[ "${RUN_REGION}" == "true" ]]; then
  echo ""
  echo "--- [2/3] Benchmarking Region Path (--region ${GATE_REG_ARG} --script cjk) ---"
  REG_SRT="${OUT_DIR}/zootopia_region.srt"
  REG_LOG="${OUT_DIR}/zootopia_region.log"
  REG_TIME="${OUT_DIR}/zootopia_region.time"

  set +e
  MEASURE_TIME_RSS "${REG_TIME}" "${CLI_BIN}" "${SAMPLE_VIDEO}" -o "${REG_SRT}" --region "${GATE_REG_ARG}" --script cjk >"${REG_LOG}"
  REG_RC=$?
  set -e

  if [[ ${REG_RC} -ne 0 || ! -s "${REG_SRT}" ]]; then
    echo "[-] Region path extraction failed (rc=${REG_RC})"
    tail -n 15 "${REG_LOG}" | sed 's/^/    /'
    OVERALL_SUCCESS=false
  else
    REG_METRICS=$(EXTRACT_METRICS "${REG_TIME}" "${REG_SRT}")
    REG_WALL=$(python3 -c "import json; print(json.loads('''${REG_METRICS}''')['wall_sec'])")
    REG_RSS=$(python3 -c "import json; print(json.loads('''${REG_METRICS}''')['rss_mb'])")
    REG_CUES=$(python3 -c "import json; print(json.loads('''${REG_METRICS}''')['cues'])")
    REG_F1=$(python3 -c "import json; print(json.loads('''${REG_METRICS}''')['f1'])")
    REG_CER=$(python3 -c "import json; print(json.loads('''${REG_METRICS}''')['cer'])")
    REG_EXACT=$(python3 -c "import json; print(json.loads('''${REG_METRICS}''')['exact_text'])")

    echo "    Results:"
    echo "      Wall Time:  ${REG_WALL}s   (Target: <= ${GATE_REG_WALL_MAX}s)"
    echo "      Peak RSS:   ${REG_RSS}MB    (Target: <= ${GATE_REG_RSS_MAX}MB)"
    echo "      Pred Cues:  ${REG_CUES}     (Target: exactly ${GATE_REG_CUES_EXACT})"
    echo "      Time F1:    ${REG_F1}      (Target: >= ${GATE_REG_F1_MIN})"
    echo "      Matched CER: $(python3 -c "print(round(${REG_CER} * 100, 2))")% (${REG_CER}) (Target: <= $(python3 -c "print(round(${GATE_REG_CER_MAX} * 100, 1))")%)"
    echo "      Exact Cues: ${REG_EXACT} / ${GATE_REG_CUES_EXACT}"

    # Gate verification
    if [[ "${REG_CUES}" -eq "${GATE_REG_CUES_EXACT}" ]]; then
      echo "      [✓] Gate Cue Count == ${GATE_REG_CUES_EXACT}: PASS"
    else
      echo "      [✗] Gate Cue Count == ${GATE_REG_CUES_EXACT}: FAIL (${REG_CUES} != ${GATE_REG_CUES_EXACT})"
      OVERALL_SUCCESS=false
    fi

    if python3 -c "import sys; sys.exit(0 if ${REG_F1} >= ${GATE_REG_F1_MIN} else 1)"; then
      echo "      [✓] Gate F1 >= ${GATE_REG_F1_MIN}: PASS"
    else
      echo "      [✗] Gate F1 >= ${GATE_REG_F1_MIN}: FAIL (${REG_F1} < ${GATE_REG_F1_MIN})"
      OVERALL_SUCCESS=false
    fi

    if python3 -c "import sys; sys.exit(0 if ${REG_CER} <= ${GATE_REG_CER_MAX} else 1)"; then
      echo "      [✓] Gate CER <= ${GATE_REG_CER_MAX}: PASS"
    else
      echo "      [✗] Gate CER <= ${GATE_REG_CER_MAX}: FAIL (${REG_CER} > ${GATE_REG_CER_MAX})"
      OVERALL_SUCCESS=false
    fi

    if python3 -c "import sys; sys.exit(0 if ${REG_WALL} <= ${GATE_REG_WALL_MAX} else 1)"; then
      echo "      [✓] Gate Wall Time <= ${GATE_REG_WALL_MAX}s: PASS"
    elif [[ "${ALLOW_PERF_WARN}" == "true" ]]; then
      echo "      [!] Gate Wall Time <= ${GATE_REG_WALL_MAX}s: WARN (${REG_WALL}s > ${GATE_REG_WALL_MAX}s, non-fatal due to --allow-perf-warn)"
    else
      echo "      [✗] Gate Wall Time <= ${GATE_REG_WALL_MAX}s: FAIL (${REG_WALL}s > ${GATE_REG_WALL_MAX}s)"
      OVERALL_SUCCESS=false
    fi

    if python3 -c "import sys; sys.exit(0 if ${REG_RSS} <= ${GATE_REG_RSS_MAX} else 1)"; then
      echo "      [✓] Gate Peak RSS <= ${GATE_REG_RSS_MAX}MB: PASS"
    elif [[ "${ALLOW_PERF_WARN}" == "true" ]]; then
      echo "      [!] Gate Peak RSS <= ${GATE_REG_RSS_MAX}MB: WARN (${REG_RSS}MB > ${GATE_REG_RSS_MAX}MB, non-fatal due to --allow-perf-warn)"
    else
      echo "      [✗] Gate Peak RSS <= ${GATE_REG_RSS_MAX}MB: FAIL (${REG_RSS}MB > ${GATE_REG_RSS_MAX}MB)"
      OVERALL_SUCCESS=false
    fi
  fi
fi

# ------------------------------------------------------------------------------
# 3. Benchmark Auto-Region Path (--auto-region --script cjk)
# ------------------------------------------------------------------------------
if [[ "${RUN_AUTOREGION}" == "true" ]]; then
  echo ""
  echo "--- [3/3] Benchmarking Auto-Region Path (--auto-region --script cjk) ---"
  AUTO_SRT="${OUT_DIR}/zootopia_autoregion.srt"
  AUTO_LOG="${OUT_DIR}/zootopia_autoregion.log"
  AUTO_TIME="${OUT_DIR}/zootopia_autoregion.time"

  set +e
  MEASURE_TIME_RSS "${AUTO_TIME}" "${CLI_BIN}" "${SAMPLE_VIDEO}" -o "${AUTO_SRT}" --auto-region --script cjk >"${AUTO_LOG}"
  AUTO_RC=$?
  set -e

  if [[ ${AUTO_RC} -ne 0 || ! -s "${AUTO_SRT}" ]]; then
    echo "[-] Auto-region path extraction failed (rc=${AUTO_RC})"
    tail -n 15 "${AUTO_LOG}" | sed 's/^/    /'
    OVERALL_SUCCESS=false
  else
    AUTO_METRICS=$(EXTRACT_METRICS "${AUTO_TIME}" "${AUTO_SRT}")
    AUTO_WALL=$(python3 -c "import json; print(json.loads('''${AUTO_METRICS}''')['wall_sec'])")
    AUTO_RSS=$(python3 -c "import json; print(json.loads('''${AUTO_METRICS}''')['rss_mb'])")
    AUTO_TP=$(python3 -c "import json; print(json.loads('''${AUTO_METRICS}''')['tp'])")
    AUTO_CUES=$(python3 -c "import json; print(json.loads('''${AUTO_METRICS}''')['cues'])")
    AUTO_F1=$(python3 -c "import json; print(json.loads('''${AUTO_METRICS}''')['f1'])")
    AUTO_CER=$(python3 -c "import json; print(json.loads('''${AUTO_METRICS}''')['cer'])")
    AUTO_EXACT=$(python3 -c "import json; print(json.loads('''${AUTO_METRICS}''')['exact_text'])")

    echo "    Results:"
    echo "      Wall Time:  ${AUTO_WALL}s   (Target: <= ${GATE_AUTO_WALL_MAX}s)"
    echo "      Peak RSS:   ${AUTO_RSS}MB    (Target: <= ${GATE_AUTO_RSS_MAX}MB)"
    echo "      True Pos:   ${AUTO_TP} / ${GATE_REG_CUES_EXACT}  (Target: >= ${GATE_AUTO_TP_MIN})"
    echo "      Pred Cues:  ${AUTO_CUES}     (Reference: ~${GATE_REG_CUES_EXACT})"
    echo "      Time F1:    ${AUTO_F1}      (Target: >= ${GATE_AUTO_F1_MIN})"
    echo "      Matched CER: $(python3 -c "print(round(${AUTO_CER} * 100, 2))")% (${AUTO_CER}) (Target: <= $(python3 -c "print(round(${GATE_AUTO_CER_MAX} * 100, 1))")%)"
    echo "      Exact Cues: ${AUTO_EXACT} / ${GATE_REG_CUES_EXACT}"

    # Gate verification
    if python3 -c "import sys; sys.exit(0 if ${AUTO_TP} >= ${GATE_AUTO_TP_MIN} else 1)"; then
      echo "      [✓] Gate TP >= ${GATE_AUTO_TP_MIN}: PASS (${AUTO_TP} >= ${GATE_AUTO_TP_MIN})"
    else
      echo "      [✗] Gate TP >= ${GATE_AUTO_TP_MIN}: FAIL (${AUTO_TP} < ${GATE_AUTO_TP_MIN})"
      OVERALL_SUCCESS=false
    fi

    if python3 -c "import sys; sys.exit(0 if ${AUTO_F1} >= ${GATE_AUTO_F1_MIN} else 1)"; then
      echo "      [✓] Gate F1 >= ${GATE_AUTO_F1_MIN}: PASS (${AUTO_F1} >= ${GATE_AUTO_F1_MIN})"
    else
      echo "      [✗] Gate F1 >= ${GATE_AUTO_F1_MIN}: FAIL (${AUTO_F1} < ${GATE_AUTO_F1_MIN})"
      OVERALL_SUCCESS=false
    fi

    if python3 -c "import sys; sys.exit(0 if ${AUTO_CER} <= ${GATE_AUTO_CER_MAX} else 1)"; then
      echo "      [✓] Gate CER <= ${GATE_AUTO_CER_MAX}: PASS (${AUTO_CER} <= ${GATE_AUTO_CER_MAX})"
    else
      echo "      [✗] Gate CER <= ${GATE_AUTO_CER_MAX}: FAIL (${AUTO_CER} > ${GATE_AUTO_CER_MAX})"
      OVERALL_SUCCESS=false
    fi

    if python3 -c "import sys; sys.exit(0 if ${AUTO_WALL} <= ${GATE_AUTO_WALL_MAX} else 1)"; then
      echo "      [✓] Gate Wall Time <= ${GATE_AUTO_WALL_MAX}s: PASS (${AUTO_WALL}s <= ${GATE_AUTO_WALL_MAX}s)"
    elif [[ "${ALLOW_PERF_WARN}" == "true" ]]; then
      echo "      [!] Gate Wall Time <= ${GATE_AUTO_WALL_MAX}s: WARN (${AUTO_WALL}s > ${GATE_AUTO_WALL_MAX}s, non-fatal due to --allow-perf-warn)"
    else
      echo "      [✗] Gate Wall Time <= ${GATE_AUTO_WALL_MAX}s: FAIL (${AUTO_WALL}s > ${GATE_AUTO_WALL_MAX}s)"
      OVERALL_SUCCESS=false
    fi

    if python3 -c "import sys; sys.exit(0 if ${AUTO_RSS} <= ${GATE_AUTO_RSS_MAX} else 1)"; then
      echo "      [✓] Gate Peak RSS <= ${GATE_AUTO_RSS_MAX}MB: PASS (${AUTO_RSS}MB <= ${GATE_AUTO_RSS_MAX}MB)"
    elif [[ "${ALLOW_PERF_WARN}" == "true" ]]; then
      echo "      [!] Gate Peak RSS <= ${GATE_AUTO_RSS_MAX}MB: WARN (${AUTO_RSS}MB > ${GATE_AUTO_RSS_MAX}MB, non-fatal due to --allow-perf-warn)"
    else
      echo "      [✗] Gate Peak RSS <= ${GATE_AUTO_RSS_MAX}MB: FAIL (${AUTO_RSS}MB > ${GATE_AUTO_RSS_MAX}MB)"
      OVERALL_SUCCESS=false
    fi

    # Multi-sentence verification
    CHECK_MULTI_SENTENCE "${AUTO_SRT}" "${GATE_AUTO_CLAUSES_JSON}" || OVERALL_SUCCESS=false
  fi
fi

echo ""
echo "======================================================================"
if [[ "${OVERALL_SUCCESS}" == "true" ]]; then
  echo "[+] Tier 4 Zootopia Benchmark: ALL GATES PASSED (100%)"
  exit 0
else
  echo "[-] Tier 4 Zootopia Benchmark: ACCEPTANCE GATES FAILED" >&2
  exit 1
fi
