#!/usr/bin/env bash
# SubLift Python-free product verification gate.
#
# This is the product verification entry for Native CLI, macOS, and Web.
# It must not install Python dependencies, must not run Python scripts, and
# must not skip a required product check because python/uv/.venv are missing.
#
# Isolated Oracle/benchmark tools: ./scripts/verify-offline.sh
# Historical cutover mixed gate (not default): ./scripts/verify-standard.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass=0
fail=0

log_ok() { printf "${GREEN}[OK]${NC}  %s\n" "$1"; pass=$((pass + 1)); }
log_fail() { printf "${RED}[FAIL]${NC} %s\n" "$1"; fail=$((fail + 1)); }
log_info() { printf "       %s\n" "$1"; }

require_cmd() {
  local name="$1"
  if ! command -v "$name" >/dev/null 2>&1; then
    log_fail "required command not found: $name"
    return 1
  fi
  log_ok "tool $name -> $(command -v "$name")"
}

run_or_fail() {
  local label="$1"
  shift
  if "$@" >/tmp/sublift_product_verify.log 2>&1; then
    log_ok "$label"
    return 0
  fi
  log_fail "$label"
  cat /tmp/sublift_product_verify.log 2>/dev/null || true
  return 0
}

echo "=============================="
echo " SubLift Python-free product gate"
echo "=============================="
echo
echo "This gate does not invoke python, uv, or .venv."
if command -v python3 >/dev/null 2>&1 || command -v uv >/dev/null 2>&1; then
  log_info "python/uv may exist on PATH; they are not used."
else
  log_info "python/uv absent; required product checks still run."
fi
echo

require_cmd cmake
require_cmd ffmpeg
require_cmd curl
if [ "$(uname -s)" = "Darwin" ]; then
  require_cmd swift
fi
require_cmd npm

if [ "$fail" -gt 0 ]; then
  echo "Missing required tools; stopping."
  exit 1
fi

echo
echo "=============================="
echo " Native build + CTest"
echo "=============================="
echo

cmake_cmd=(cmake -S cpp -B build/cpp -DCMAKE_BUILD_TYPE=Debug
  -DSUBLIFT_REQUIRE_OPENCV=ON -DSUBLIFT_ENABLE_PADDLE=ON)
if [ "$(uname -s)" = "Darwin" ] && [ "${SUBLIFT_VERIFY_SKIP_VISION:-0}" != "1" ]; then
  cmake_cmd+=(-DSUBLIFT_ENABLE_VISION=ON)
fi
if command -v ninja >/dev/null 2>&1; then
  cmake_cmd+=(-G Ninja)
fi

if "${cmake_cmd[@]}" >/tmp/sublift_product_cmake.log 2>&1 \
  && cmake --build build/cpp >/tmp/sublift_product_build.log 2>&1 \
  && ctest --test-dir build/cpp --output-on-failure >/tmp/sublift_product_ctest.log 2>&1; then
  log_ok "cmake/ctest (build/cpp Debug)"
else
  log_fail "cmake/ctest (build/cpp Debug)"
  tail -n 40 /tmp/sublift_product_cmake.log /tmp/sublift_product_build.log /tmp/sublift_product_ctest.log 2>/dev/null || true
fi

CLI="$ROOT/build/cpp/bin/sublift"
WORKER="$ROOT/build/cpp/bin/sublift_worker"
SERVER="$ROOT/build/cpp/bin/sublift_server"
if [ ! -x "$CLI" ] || [ ! -x "$WORKER" ] || [ ! -x "$SERVER" ]; then
  log_fail "native binaries missing (sublift / sublift_worker / sublift_server)"
fi

echo
echo "=============================="
echo " Swift + Web layer tests"
echo "=============================="
echo

if [ "$(uname -s)" = "Darwin" ]; then
  run_or_fail "swift test" swift test --package-path "$ROOT/apps/macos"
else
  log_info "macOS Swift tests are not applicable on $(uname -s)"
fi

run_or_fail "apps/web vitest" npm --prefix "$ROOT/apps/web" test
run_or_fail "apps/web build" npm --prefix "$ROOT/apps/web" run build

echo
echo "=============================="
echo " Resources, capability, extract"
echo "=============================="
echo

if [ -x "$CLI" ]; then
  if "$CLI" resources status >/tmp/sublift_resources_status.log 2>&1; then
    log_ok "sublift resources status"
  else
    log_info "resources status reported missing models; attempting install"
    if "$CLI" resources install >/tmp/sublift_resources_install.log 2>&1 \
      && "$CLI" resources status >/tmp/sublift_resources_status.log 2>&1; then
      log_ok "sublift resources install + status"
    else
      log_fail "sublift resources install/status"
      cat /tmp/sublift_resources_install.log /tmp/sublift_resources_status.log 2>/dev/null || true
    fi
  fi
  if grep -q "onnxruntime: available" /tmp/sublift_resources_status.log 2>/dev/null; then
    log_ok "ORT library located (not Python wheel)"
  else
    log_fail "ORT library not available"
    cat /tmp/sublift_resources_status.log 2>/dev/null || true
  fi
fi

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/sublift-product-gate.XXXXXX")"
cleanup() {
  if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$WORKDIR"
}
trap cleanup EXIT

VIDEO="$WORKDIR/clip.mp4"
if ffmpeg -y -f lavfi -i testsrc=size=320x240:rate=5 -t 1 -pix_fmt yuv420p "$VIDEO" >/dev/null 2>&1 \
  && [ -f "$VIDEO" ]; then
  log_ok "synthetic product video"
else
  log_fail "ffmpeg could not create a synthetic video"
fi

if [ -x "$CLI" ] && [ -f "$VIDEO" ]; then
  SRT="$WORKDIR/cli-mock.srt"
  if "$CLI" extract "$VIDEO" --engine mock -o "$SRT" >/tmp/sublift_cli_mock.log 2>&1 \
    && [ -f "$SRT" ]; then
    log_ok "Native CLI mock extract wrote $SRT"
  else
    log_fail "Native CLI mock extract"
    cat /tmp/sublift_cli_mock.log 2>/dev/null || true
  fi

  if "$CLI" extract "$WORKDIR/missing.mp4" --engine mock -o "$WORKDIR/missing.srt" \
    >/tmp/sublift_cli_missing.log 2>&1; then
    log_fail "CLI missing-video path should fail closed"
  else
    log_ok "CLI missing-video path fail-closed"
  fi
  if "$CLI" extract "$VIDEO" --runtime python -o "$WORKDIR/py.srt" \
    >/tmp/sublift_cli_runtime.log 2>&1; then
    log_fail "CLI --runtime python should fail closed"
  else
    log_ok "CLI --runtime python fail-closed"
  fi
fi

if [ -x "$WORKER" ]; then
  if "$WORKER" --probe-engine paddle >/tmp/sublift_probe_paddle.log 2>&1; then
    log_ok "worker --probe-engine paddle"
    if [ -x "$CLI" ] && [ -f "$VIDEO" ]; then
      if "$CLI" extract "$VIDEO" --engine paddle -o "$WORKDIR/cli-paddle.srt" \
        >/tmp/sublift_cli_paddle.log 2>&1 && [ -f "$WORKDIR/cli-paddle.srt" ]; then
        log_ok "Native CLI paddle extract wrote SRT"
      else
        log_fail "Native CLI paddle extract"
        cat /tmp/sublift_cli_paddle.log 2>/dev/null || true
      fi
    fi
  else
    log_info "paddle probe unavailable; mock extract remains the required product path"
    cat /tmp/sublift_probe_paddle.log 2>/dev/null || true
  fi
fi

echo
echo "=============================="
echo " Web Native Server extract"
echo "=============================="
echo

PORT="${SUBLIFT_TEST_PORT:-18765}"
if [ -x "$SERVER" ] && [ -f "$VIDEO" ]; then
  export SUBLIFT_ALLOWED_MEDIA_ROOT="$WORKDIR"
  export SUBLIFT_STATIC_DIR="$ROOT/apps/web/dist"
  export SUBLIFT_HOST=127.0.0.1
  export SUBLIFT_PORT="$PORT"
  "$SERVER" >/tmp/sublift_server_gate.log 2>&1 &
  SERVER_PID=$!
  READY=0
  for _ in $(seq 1 50); do
    if curl -sf "http://127.0.0.1:${PORT}/api/system/info" >/tmp/sublift_sysinfo.json 2>/dev/null; then
      READY=1
      break
    fi
    sleep 0.1
  done
  if [ "$READY" -ne 1 ]; then
    log_fail "sublift_server did not become ready"
    cat /tmp/sublift_server_gate.log 2>/dev/null || true
  else
    log_ok "sublift_server /api/system/info"
    if grep -q '"runtime":"cpp"' /tmp/sublift_sysinfo.json; then
      log_ok "system info runtime=cpp"
    else
      log_fail "system info runtime is not cpp"
      cat /tmp/sublift_sysinfo.json
    fi
    if grep -q '"name":"mock"' /tmp/sublift_sysinfo.json; then
      log_ok "system info advertises mock engine"
    else
      log_fail "system info missing mock engine"
    fi
    if curl -sf "http://127.0.0.1:${PORT}/" >/tmp/sublift_web_index.html 2>/dev/null \
      && grep -q "<html" /tmp/sublift_web_index.html; then
      log_ok "Web static index served by Native Server"
    else
      log_fail "Web static index not served"
    fi

    JOB_JSON=$(curl -sf -X POST "http://127.0.0.1:${PORT}/api/jobs" \
      -H "Content-Type: application/json" \
      -d "{\"video_path\":\"$VIDEO\",\"engine\":\"mock\",\"fps\":5.0}" \
      || true)
    JOB_ID=$(printf '%s' "$JOB_JSON" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
    if [ -z "$JOB_ID" ]; then
      log_fail "POST /api/jobs mock extract"
      printf '%s\n' "$JOB_JSON"
    else
      log_ok "POST /api/jobs job_id=$JOB_ID"
      EXPORT_OK=0
      for _ in $(seq 1 80); do
        CODE=$(curl -s -o "$WORKDIR/web.srt" -w "%{http_code}" \
          "http://127.0.0.1:${PORT}/api/jobs/${JOB_ID}/export" || true)
        if [ "$CODE" = "200" ]; then
          EXPORT_OK=1
          break
        fi
        sleep 0.1
      done
      if [ "$EXPORT_OK" -eq 1 ] && [ -f "$WORKDIR/web.srt" ]; then
        log_ok "Web Native Server mock extract exported SRT"
      else
        log_fail "Web Native Server SRT export"
      fi
    fi
  fi
else
  log_fail "server binary or product video missing; Web extract is required"
fi

echo
echo "=============================="
echo " Summary"
echo "=============================="
printf "passed: ${GREEN}%d${NC}  failed: ${RED}%d${NC}\n" "$pass" "$fail"
echo "Product gate: ./scripts/verification/verify-product.sh"
echo "Offline tools: ./scripts/verify-offline.sh"
echo "Explicit cutover mixed gate (not default): ./scripts/verify-standard.sh"

if [ "$fail" -gt 0 ]; then
  printf "${RED}Python-free product verification failed.${NC}\n"
  exit 1
fi
printf "${GREEN}Python-free product verification passed.${NC}\n"
