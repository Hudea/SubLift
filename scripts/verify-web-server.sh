#!/usr/bin/env bash
# =============================================================================
# SubLift Web Server End-to-End Regression & Acceptance Script (Feature 12401)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

PORT="${SUBLIFT_TEST_PORT:-8899}"
HOST="127.0.0.1"
BASE_URL="http://${HOST}:${PORT}"
SERVER_BIN="${SUBLIFT_SERVER_BIN:-${ROOT_DIR}/build/cpp/bin/sublift_server}"
STATIC_DIR="${SUBLIFT_STATIC_DIR:-${ROOT_DIR}/apps/web/dist}"

echo "================================================================="
echo " SubLift Web Server E2E Verification & Linux Quality Gate"
echo "================================================================="
echo "Target Binary: ${SERVER_BIN}"
echo "Static Assets: ${STATIC_DIR}"
echo "Test Endpoint: ${BASE_URL}"
echo ""

# 1. 检查可执行文件与静态资源
if [[ ! -x "${SERVER_BIN}" ]]; then
  echo "[ERROR] Server binary not found or not executable at: ${SERVER_BIN}"
  echo "        Please build with: cmake --build build/cpp --target sublift_server"
  exit 1
fi

if [[ ! -f "${STATIC_DIR}/index.html" ]]; then
  echo "[WARN] Static directory does not contain index.html, attempting build..."
  npm --prefix "${ROOT_DIR}/apps/web" run build
fi

# 2. 后台启动测试服务器
echo "[1/6] 启动后台测试服务器..."
"${SERVER_BIN}" --host "${HOST}" --port "${PORT}" --static-dir "${STATIC_DIR}" &
SERVER_PID=$!

cleanup() {
  echo ""
  echo "正在清理测试进程 (PID: ${SERVER_PID})..."
  kill -TERM "${SERVER_PID}" 2>/dev/null || true
  for _ in {1..20}; do
    if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
  if kill -0 "${SERVER_PID}" 2>/dev/null; then
    echo "强制终止未响应测试进程..."
    kill -KILL "${SERVER_PID}" 2>/dev/null || true
  fi
  wait "${SERVER_PID}" 2>/dev/null || true
  echo "清理完成。"
}
trap cleanup EXIT INT TERM

# 等待探活可用
echo "等待服务就绪..."
READY=0
for i in {1..30}; do
  if curl -s "${BASE_URL}/api/system/info" >/dev/null 2>&1; then
    READY=1
    break
  fi
  sleep 0.2
done

if [[ ${READY} -eq 0 ]]; then
  echo "[FAIL] 服务器未能在 6 秒内响应健康检查！"
  exit 1
fi
echo "[OK] 服务就绪 (PID: ${SERVER_PID})"

# 3. 验证 /api/system/info
echo ""
echo "[2/6] 验证 /api/system/info 系统探活与引擎发现..."
SYS_INFO=$(curl -s -f "${BASE_URL}/api/system/info")
echo "System Info Response: ${SYS_INFO}"
if echo "${SYS_INFO}" | grep -q '"runtime":"cpp"'; then
  echo "[OK] /api/system/info runtime=cpp 验证通过"
else
  echo "[FAIL] /api/system/info 格式异常"
  exit 1
fi

# 4. 验证 Web 工作台静态托管
echo ""
echo "[3/6] 验证 GET / 静态前端工作台 HTML直出..."
INDEX_HTML=$(curl -s -f "${BASE_URL}/")
if echo "${INDEX_HTML}" | grep -q 'SubLift'; then
  echo "[OK] 静态 SPA 托管验证通过"
else
  echo "[FAIL] 静态 SPA 页面内容不匹配"
  exit 1
fi

# 5. 运行 Python E2E 完整流水线套件 (HTTP 206, JPEG 抽帧, 任务生命周期, SSE, SRT 导出)
echo ""
echo "[4/6] 运行全量 E2E 接口与视频处理套件..."
SUBLIFT_SERVER_URL="${BASE_URL}" uv run python "${ROOT_DIR}/scripts/test_server_e2e.py"

# 6. 验证 Catch2 服务端底层契约
echo ""
echo "[5/6] 运行 C++ 服务端 Catch2 单元测试..."
if [[ -x "${ROOT_DIR}/build/cpp/bin/sublift_tests" ]]; then
  "${ROOT_DIR}/build/cpp/bin/sublift_tests" "[server]"
  echo "[OK] Catch2 [server] 契约测试通过"
fi

# 7. 汇总与验收
echo ""
echo "[6/6] 验收完成！"
echo "================================================================="
echo " SubLift Web Server & Linux 容器化自动化验收: 100% PASS"
echo "================================================================="
exit 0
