#!/usr/bin/env bash
# SubLift Web Server local acceptance and quality gate.
#
# Runs the E2E suite against the local Native Server, followed by Catch2 server
# contract tests and frontend Vitest. Paddle extraction uses a golden SRT when
# the local engine is available.
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
echo " SubLift Web Server Local Quality Gate"
echo "================================================================="
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
echo "================================================================="
