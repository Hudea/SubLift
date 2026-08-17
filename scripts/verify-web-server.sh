#!/usr/bin/env bash
# =============================================================================
# SubLift Web Server Dual-Mode Acceptance & Quality Gate (Features 12401 & 12505)
#
# Docker mode: real container build + run, real PaddleOCR extraction with a
#   golden SRT baseline (recorded on first run, exact-compared afterwards).
# Local mode: explicit degradation with the same E2E suite against the native
#   binary + Catch2 server contract tests + frontend Vitest.
# No stage may silently skip: anything missing fails loudly.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

PORT="${SUBLIFT_TEST_PORT:-8899}"
HOST="127.0.0.1"
BASE_URL="http://${HOST}:${PORT}"
SERVER_BIN="${SUBLIFT_SERVER_BIN:-${ROOT_DIR}/build/cpp/bin/sublift_server}"
STATIC_DIR="${SUBLIFT_STATIC_DIR:-${ROOT_DIR}/apps/web/dist}"
GOLDEN_SRT="${ROOT_DIR}/debug/web-server/paddle_golden.srt"
TESTS_BIN="${ROOT_DIR}/build/cpp/bin/sublift_tests"

echo "================================================================="
echo " SubLift Web Server & Container Dual-Mode Quality Gate"
echo "================================================================="

# -----------------------------------------------------------------------------
# Mode Detection: Check if Docker daemon is available
# -----------------------------------------------------------------------------
DOCKER_AVAILABLE=0
if command -v docker >/dev/null 2>&1; then
  if docker info >/dev/null 2>&1; then
    DOCKER_AVAILABLE=1
  fi
fi

if [[ "${DOCKER_AVAILABLE}" -eq 1 && "${SUBLIFT_VERIFY_SKIP_DOCKER:-0}" != "1" ]]; then
  echo "[MODE] Docker daemon 可用，启动容器化真实端到端验收..."
  echo ""

  IMAGE_NAME="sublift/web:verify-gate"
  CONTAINER_NAME="sublift-verify-srv-$$"
  HOST_MEDIA="$(mktemp -d "${TMPDIR:-/tmp}/sublift_verify_media.XXXXXX")"

  docker_cleanup() {
    echo ""
    echo "清理测试容器 (${CONTAINER_NAME})..."
    docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
    rm -rf "${HOST_MEDIA}"
    echo "容器与临时媒体目录清理完成。"
  }
  trap docker_cleanup EXIT INT TERM

  echo "[1/6] 构建生产级多阶段 Docker 镜像 (PaddleOCR 模型经 SHA256 校验硬失败)..."
  docker build -t "${IMAGE_NAME}" -f "${ROOT_DIR}/Dockerfile" "${ROOT_DIR}"

  echo "[2/6] 启动测试容器 (${CONTAINER_NAME}) on port ${PORT}, 媒体目录挂载 ${HOST_MEDIA} -> /media..."
  docker run -d \
    --name "${CONTAINER_NAME}" \
    -p "${PORT}:8080" \
    -v "${HOST_MEDIA}:/media" \
    -e SUBLIFT_HOST=0.0.0.0 \
    -e SUBLIFT_PORT=8080 \
    -e SUBLIFT_ALLOWED_MEDIA_ROOT=/media \
    -e 'SUBLIFT_CORS_ORIGIN=*' \
    "${IMAGE_NAME}"

  echo "[3/6] 等待容器健康检查与服务就绪 (2xx 才算就绪)..."
  READY=0
  for _ in {1..60}; do
    if curl -sf "${BASE_URL}/api/system/info" >/dev/null 2>&1; then
      READY=1
      break
    fi
    sleep 0.5
  done

  if [[ ${READY} -eq 0 ]]; then
    echo "[FAIL] 容器未能成功响应 /api/system/info"
    docker logs "${CONTAINER_NAME}"
    exit 1
  fi

  echo "[4/6] 断言容器内 PaddleOCR 引擎真实可用 (paddle.available=true)..."
  uv run python - "${BASE_URL}" <<'PY'
import json
import sys
import urllib.request

info = json.load(urllib.request.urlopen(sys.argv[1] + "/api/system/info"))
paddle = [e for e in info.get("engines", []) if e.get("name") == "paddle"]
assert paddle and paddle[0].get("available") is True, (
    f"paddle.available is not true; engines = {info.get('engines')}"
)
print(f"paddle.available=true ({paddle[0].get('detail', '')})")
PY

  echo "[5/6] 针对容器化服务运行 E2E 回归 (含真实 Paddle 提取与 Golden SRT 比对)..."
  echo "      Golden 基线: ${GOLDEN_SRT} (首次运行录制，此后精确比对)"
  SUBLIFT_SERVER_URL="${BASE_URL}" \
  SUBLIFT_E2E_WORKSPACE_DIR="${HOST_MEDIA}" \
  SUBLIFT_E2E_API_PATH_PREFIX="/media" \
  SUBLIFT_E2E_EXPECT_CORS=1 \
  SUBLIFT_E2E_GOLDEN_SRT="${GOLDEN_SRT}" \
    uv run python "${ROOT_DIR}/scripts/test_server_e2e.py"

  echo "[6/6] 容器日志尾部核对..."
  docker logs --tail 5 "${CONTAINER_NAME}"

  echo ""
  echo "================================================================="
  echo " [OK] Docker 容器化验收通过：镜像构建 / paddle.available=true /"
  echo "      E2E 全量 (含真实 Paddle 提取 + Golden SRT 比对) 全部执行成功"
  echo "================================================================="
  exit 0
else
  echo "[MODE] [SKIP] Docker daemon 未运行或被显式跳过，降级为本地 Native 二进制验收"
  echo "       (容器阶段未执行；以下为本地实测结果，非容器化结论)"
  echo ""

  echo "Target Binary: ${SERVER_BIN}"
  echo "Static Assets: ${STATIC_DIR}"
  echo "Test Endpoint: ${BASE_URL}"
  echo ""

  # 1. 检查可执行文件与静态资源
  if [[ ! -x "${SERVER_BIN}" ]]; then
    echo "[ERROR] Server binary not found at: ${SERVER_BIN}"
    echo "        Building with cmake..."
    cmake --build "${ROOT_DIR}/build/cpp" --target sublift_server
  fi

  if [[ ! -f "${STATIC_DIR}/index.html" ]]; then
    echo "[WARN] Static directory does not contain index.html, building web frontend..."
    npm --prefix "${ROOT_DIR}/apps/web" run build
  fi

  # 2. 运行 Python E2E 完整套件（本机 paddle 可用时自动包含 TC-JOB-07 真实提取）
  echo "[1/3] 运行 E2E 接口与视频处理套件 (含 SRT 比对、排队流转、安全沙箱与 Paddle 真实用例)..."
  SUBLIFT_E2E_GOLDEN_SRT="${GOLDEN_SRT}" \
    uv run python "${ROOT_DIR}/scripts/test_server_e2e.py"

  # 3. 验证 Catch2 服务端底层契约（缺失即失败，不允许静默跳过）
  echo ""
  echo "[2/3] 运行 C++ 服务端 Catch2 单元测试..."
  if [[ ! -x "${TESTS_BIN}" ]]; then
    echo "[INFO] sublift_tests 不存在，尝试构建..."
    cmake --build "${ROOT_DIR}/build/cpp" --target sublift_tests
  fi
  if [[ ! -x "${TESTS_BIN}" ]]; then
    echo "[FAIL] Catch2 测试二进制缺失且构建失败: ${TESTS_BIN}"
    exit 1
  fi
  "${TESTS_BIN}" "[server]"
  echo "[OK] Catch2 [server] 契约测试通过"

  # 4. 前端单元测试
  echo ""
  echo "[3/3] 运行前端 Vitest 单元测试..."
  npm --prefix "${ROOT_DIR}/apps/web" test

  echo ""
  echo "================================================================="
  echo " [OK] 本地 Native 验收通过：E2E (含 Paddle 真实用例，若引擎可用) /"
  echo "      Catch2 [server] / Vitest 全部执行成功"
  echo " [NOTE] 容器化阶段本次未执行（无 Docker daemon）"
  echo "================================================================="
  exit 0
fi
