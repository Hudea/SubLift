#!/usr/bin/env bash
# SubLift Python-free container verification (Phase 14 D04 / ADR-0040).
#
# Requires a Docker daemon. Does not invoke python, uv, or .venv.
# Sequence: docker build → compose up with host media mounted at /media →
# readiness → paddle.available → sandbox rejects host system paths →
# Paddle job using only container /media paths → exact golden SRT compare.
#
# Missing golden: first run records a candidate and exits non-zero
# ("pending human confirmation"). It must not claim pass.
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
log_skip() { printf "${YELLOW}[SKIPPED]${NC} %s\n" "$1"; }

GOLDEN_SRT="${SUBLIFT_CONTAINER_GOLDEN_SRT:-$ROOT/benchmark/container/paddle_drawtext.v1.srt}"
PENDING_SRT="$ROOT/debug/container/pending_paddle_golden.srt"
COMPOSE_PROJECT="${SUBLIFT_CONTAINER_COMPOSE_PROJECT:-sublift-d04}"
HTTP_PORT="${SUBLIFT_CONTAINER_HTTP_PORT:-18766}"
BASE="http://127.0.0.1:${HTTP_PORT}"
IMAGE="${SUBLIFT_CONTAINER_IMAGE:-sublift:local}"

MEDIA=""
COMPOSE_UP=0

cleanup() {
  if [ "$COMPOSE_UP" -eq 1 ]; then
    SUBLIFT_MEDIA_DIR="${MEDIA:-/tmp}" SUBLIFT_HTTP_PORT="$HTTP_PORT" \
      docker compose -p "$COMPOSE_PROJECT" down --remove-orphans >/dev/null 2>&1 || true
  fi
  if [ -n "${MEDIA:-}" ] && [ -d "$MEDIA" ]; then
    # 容器以 uid 10001 写入 .sublift_cache，宿主用户未必能删；借用镜像 root 清掉。
    docker run --rm --user root --entrypoint /bin/rm \
      -v "$MEDIA:/wipe" "$IMAGE" -rf /wipe/.sublift_cache >/dev/null 2>&1 || true
    rm -rf "$MEDIA" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "=============================="
echo " SubLift Python-free container gate"
echo "=============================="
echo
echo "This gate does not invoke python, uv, or .venv."
echo

if ! command -v docker >/dev/null 2>&1; then
  log_skip "container stage: docker client not found"
  echo "Container verification requires Docker. Not claiming pass."
  exit 2
fi

if ! docker info >/dev/null 2>&1; then
  log_skip "container stage: docker daemon not reachable"
  echo "Container verification requires a running Docker daemon. Not claiming pass."
  exit 2
fi
log_ok "docker daemon reachable ($(docker info --format '{{.ServerVersion}}'))"

if ! command -v ffmpeg >/dev/null 2>&1; then
  log_fail "required command not found: ffmpeg (host-side test media)"
  echo "Container verification failed."
  exit 1
fi
log_ok "tool ffmpeg -> $(command -v ffmpeg)"

echo
echo "=============================="
echo " Image build"
echo "=============================="
echo

if [ "${SUBLIFT_CONTAINER_SKIP_BUILD:-0}" = "1" ] && docker image inspect "$IMAGE" >/dev/null 2>&1; then
  log_info "SUBLIFT_CONTAINER_SKIP_BUILD=1 and $IMAGE exists; skipping docker build"
else
  if docker build -t "$IMAGE" "$ROOT" >/tmp/sublift_container_build.log 2>&1; then
    log_ok "docker build -t $IMAGE"
  else
    log_fail "docker build -t $IMAGE"
    tail -n 40 /tmp/sublift_container_build.log || true
    echo "Container verification failed."
    exit 1
  fi
fi

echo
echo "=============================="
echo " Compose run (/media workspace)"
echo "=============================="
echo

MEDIA="$(mktemp -d "${TMPDIR:-/tmp}/sublift-container-media.XXXXXX")"
chmod 0777 "$MEDIA"

VIDEO_HOST="$MEDIA/clip.mp4"
VIDEO_CTR="/media/clip.mp4"

# Host-generated burned-in text; the API must only see the container path.
FONT=""
for cand in \
    /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf \
    /usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf \
    /usr/share/fonts/truetype/freefont/FreeSansBold.ttf; do
  if [ -f "$cand" ]; then
    FONT="$cand"
    break
  fi
done

DRAWTEXT="text='SUBLIFT2026':fontsize=64:fontcolor=white:x=(w-text_w)/2:y=h-text_h-24"
if [ -n "$FONT" ]; then
  DRAWTEXT="fontfile=${FONT}:${DRAWTEXT}"
fi

if ffmpeg -y -f lavfi -i color=c=black:s=640x360:d=2:r=5 \
    -vf "drawtext=${DRAWTEXT}" \
    -pix_fmt yuv420p "$VIDEO_HOST" >/tmp/sublift_container_ffmpeg.log 2>&1 \
    && [ -f "$VIDEO_HOST" ]; then
  chmod 0666 "$VIDEO_HOST"
  log_ok "burned-in test clip at host $VIDEO_HOST"
else
  log_fail "ffmpeg could not create burned-in test clip"
  cat /tmp/sublift_container_ffmpeg.log 2>/dev/null || true
  echo "Container verification failed."
  exit 1
fi

export SUBLIFT_MEDIA_DIR="$MEDIA"
export SUBLIFT_HTTP_PORT="$HTTP_PORT"
unset SUBLIFT_ACCESS_TOKEN || true

if docker compose -p "$COMPOSE_PROJECT" config >/tmp/sublift_container_compose.log 2>&1; then
  log_ok "docker compose config (SUBLIFT_MEDIA_DIR required)"
else
  log_fail "docker compose config"
  cat /tmp/sublift_container_compose.log || true
  echo "Container verification failed."
  exit 1
fi

if docker compose -p "$COMPOSE_PROJECT" up -d --no-build >/tmp/sublift_container_up.log 2>&1; then
  COMPOSE_UP=1
  log_ok "docker compose up -d (project=$COMPOSE_PROJECT port=$HTTP_PORT)"
else
  log_fail "docker compose up"
  cat /tmp/sublift_container_up.log || true
  echo "Container verification failed."
  exit 1
fi

READY=0
for _ in $(seq 1 60); do
  if curl -sf "$BASE/api/system/info" >/tmp/sublift_container_sysinfo.json 2>/dev/null; then
    READY=1
    break
  fi
  sleep 0.5
done
if [ "$READY" -ne 1 ]; then
  log_fail "container /api/system/info did not become ready"
  docker compose -p "$COMPOSE_PROJECT" logs --no-color --tail 80 || true
  echo "Container verification failed."
  exit 1
fi
log_ok "readiness: GET $BASE/api/system/info"

if grep -q '"runtime":"cpp"' /tmp/sublift_container_sysinfo.json; then
  log_ok "system info runtime=cpp"
else
  log_fail "system info runtime is not cpp"
  cat /tmp/sublift_container_sysinfo.json
fi

if grep -o '{[^}]*"name":"paddle"[^}]*}' /tmp/sublift_container_sysinfo.json \
    | grep -q '"available":true'; then
  log_ok "paddle.available=true"
else
  log_fail "paddle.available is not true"
  cat /tmp/sublift_container_sysinfo.json
fi

echo
echo "=============================="
echo " Sandbox (system paths must fail closed)"
echo "=============================="
echo

SANDBOX_CODE="$(curl -s -o /tmp/sublift_container_sandbox.json -w '%{http_code}' \
  -X POST "$BASE/api/jobs" \
  -H 'Content-Type: application/json' \
  -d '{"video_path":"/etc/passwd","engine":"paddle"}' || true)"
if [ "$SANDBOX_CODE" = "400" ]; then
  log_ok "POST /api/jobs /etc/passwd rejected ($SANDBOX_CODE)"
else
  log_fail "POST /api/jobs /etc/passwd expected 400, got ${SANDBOX_CODE:-empty}"
  cat /tmp/sublift_container_sandbox.json 2>/dev/null || true
fi

WS_CODE="$(curl -s -o /tmp/sublift_container_ws.json -w '%{http_code}' \
  -X POST "$BASE/api/config/workspace" \
  -H 'Content-Type: application/json' \
  -d '{"media_dir":"/etc"}' || true)"
if [ "$WS_CODE" = "403" ]; then
  log_ok "POST /api/config/workspace locked (403)"
else
  log_fail "locked workspace expected 403, got ${WS_CODE:-empty}"
  cat /tmp/sublift_container_ws.json 2>/dev/null || true
fi

echo
echo "=============================="
echo " Paddle extract via /media"
echo "=============================="
echo

JOB_JSON="$(curl -sf -X POST "$BASE/api/jobs" \
  -H 'Content-Type: application/json' \
  -d "{\"video_path\":\"${VIDEO_CTR}\",\"engine\":\"paddle\",\"fps\":5.0,\"region_box\":{\"x\":0.0,\"y\":0.0,\"width\":1.0,\"height\":1.0}}" \
  || true)"
JOB_ID="$(printf '%s' "$JOB_JSON" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')"
if [ -z "$JOB_ID" ]; then
  log_fail "POST /api/jobs paddle extract (container path $VIDEO_CTR)"
  printf '%s\n' "$JOB_JSON"
else
  log_ok "POST /api/jobs job_id=$JOB_ID video_path=$VIDEO_CTR"

  EXPORT_OK=0
  ACTUAL_SRT="/tmp/sublift_container_actual.srt"
  for _ in $(seq 1 120); do
    CODE="$(curl -s -o "$ACTUAL_SRT" -w '%{http_code}' "$BASE/api/jobs/${JOB_ID}/export" || true)"
    if [ "$CODE" = "200" ]; then
      EXPORT_OK=1
      break
    fi
    sleep 1
  done
  if [ "$EXPORT_OK" -eq 1 ] && [ -s "$ACTUAL_SRT" ]; then
    log_ok "Paddle job exported SRT ($(wc -c < "$ACTUAL_SRT") bytes)"
  else
    log_fail "Paddle SRT export empty or not ready (last HTTP ${CODE:-none})"
    curl -sf "$BASE/api/jobs/${JOB_ID}" || true
    echo
    docker compose -p "$COMPOSE_PROJECT" logs --no-color --tail 40 || true
    EXPORT_OK=0
  fi

  if [ "$EXPORT_OK" -eq 1 ]; then
    if [ ! -f "$GOLDEN_SRT" ]; then
      mkdir -p "$(dirname "$PENDING_SRT")"
      cp "$ACTUAL_SRT" "$PENDING_SRT"
      log_fail "golden SRT missing at $GOLDEN_SRT"
      log_info "recorded candidate at $PENDING_SRT (pending human confirmation)"
      log_info "not claiming pass; copy to $GOLDEN_SRT after review, then re-run"
      printf '%s\n' "----- candidate SRT -----"
      cat "$ACTUAL_SRT"
    elif cmp -s "$ACTUAL_SRT" "$GOLDEN_SRT"; then
      log_ok "golden SRT exact match ($GOLDEN_SRT)"
    else
      log_fail "golden SRT mismatch vs $GOLDEN_SRT"
      diff -u "$GOLDEN_SRT" "$ACTUAL_SRT" || true
    fi
  fi
fi

echo
echo "=============================="
echo " Summary"
echo "=============================="
printf "passed: ${GREEN}%d${NC}  failed: ${RED}%d${NC}\n" "$pass" "$fail"
echo "Container gate: ./scripts/verify-container.sh"
echo "Product gate (no Docker): ./scripts/verify-product.sh"

if [ "$fail" -gt 0 ]; then
  printf "${RED}Python-free container verification failed.${NC}\n"
  exit 1
fi
printf "${GREEN}Python-free container verification passed.${NC}\n"
