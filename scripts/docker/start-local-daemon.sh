#!/usr/bin/env bash
# 在项目目录内启动一份独立的 containerd + dockerd，不依赖 /var/run/docker.sock。
#
# 所有状态写到 debug/docker/（已被 .gitignore 忽略）。启动成功后：
#   export DOCKER_HOST="unix://$(pwd)/debug/docker/docker.sock"
#   docker info
#
# 本脚本不要求系统 systemd Docker 服务；失败时如实退出，不静默降级。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BASE="${ROOT_DIR}/debug/docker"

CONTAINERD_BIN="${CONTAINERD_BIN:-$(command -v containerd || true)}"
DOCKERD_BIN="${DOCKERD_BIN:-$(command -v dockerd || true)}"

if [[ -z "${CONTAINERD_BIN}" ]]; then
  echo "[FAIL] containerd not found in PATH" >&2
  exit 2
fi
if [[ -z "${DOCKERD_BIN}" ]]; then
  echo "[FAIL] dockerd not found in PATH" >&2
  exit 2
fi

mkdir -p \
  "${BASE}" \
  "${BASE}/containerd-root" \
  "${BASE}/containerd-state" \
  "${BASE}/data" \
  "${BASE}/exec" \
  "${BASE}/logs"

CONTAINERD_SOCK="${BASE}/containerd.sock"
DOCKER_SOCK="${BASE}/docker.sock"
CONTAINERD_PID="${BASE}/containerd.pid"
DOCKER_PID="${BASE}/docker.pid"

if [[ -S "${DOCKER_SOCK}" ]]; then
  if DOCKER_HOST="unix://${DOCKER_SOCK}" docker info >/dev/null 2>&1; then
    echo "[OK] local dockerd already running"
    echo "     DOCKER_HOST=unix://${DOCKER_SOCK}"
    exit 0
  fi
fi

# 上一轮残留的半启动进程：只清理本目录对应的 pid，不碰系统 dockerd。
stop_pidfile() {
  local pidfile="$1"
  if [[ -f "${pidfile}" ]]; then
    local pid
    pid="$(cat "${pidfile}" 2>/dev/null || true)"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null || true
      sleep 1
    fi
    rm -f "${pidfile}"
  fi
}
stop_pidfile "${DOCKER_PID}"
stop_pidfile "${CONTAINERD_PID}"
rm -f "${CONTAINERD_SOCK}" "${DOCKER_SOCK}"

# 项目内 containerd 配置：socket 不 chown，避免沙箱缺少 CAP_CHOWN。
CONFIG="${BASE}/containerd.toml"
sed \
  -e "s#{{ROOT}}#${BASE}/containerd-root#g" \
  -e "s#{{STATE}}#${BASE}/containerd-state#g" \
  -e "s#{{GRPC}}#${CONTAINERD_SOCK}#g" \
  -e "s#{{TTRPC}}#${CONTAINERD_SOCK}.ttrpc#g" \
  "${SCRIPT_DIR}/containerd.toml.template" >"${CONFIG}"

echo "[1/2] starting containerd (state in ${BASE})"
"${CONTAINERD_BIN}" --config "${CONFIG}" \
  >"${BASE}/logs/containerd.log" 2>&1 &
echo $! >"${CONTAINERD_PID}"

for _ in $(seq 1 40); do
  if [[ -S "${CONTAINERD_SOCK}" ]]; then
    break
  fi
  sleep 0.25
done
if [[ ! -S "${CONTAINERD_SOCK}" ]]; then
  echo "[FAIL] containerd socket not created; last log:" >&2
  tail -20 "${BASE}/logs/containerd.log" >&2 || true
  exit 1
fi

# 沙箱/无特权环境：关闭 iptables 与默认桥，存储用 vfs（不依赖 overlay mount）。
echo "[2/2] starting dockerd (socket ${DOCKER_SOCK})"
"${DOCKERD_BIN}" \
  --host "unix://${DOCKER_SOCK}" \
  --data-root "${BASE}/data" \
  --exec-root "${BASE}/exec" \
  --pidfile "${DOCKER_PID}" \
  --containerd "${CONTAINERD_SOCK}" \
  --iptables=false \
  --ip6tables=false \
  --ip-forward=false \
  --ip-masq=false \
  --userland-proxy=false \
  --bridge=none \
  --storage-driver=vfs \
  --exec-opt native.cgroupdriver=cgroupfs \
  >"${BASE}/logs/dockerd.log" 2>&1 &

for _ in $(seq 1 60); do
  if [[ -S "${DOCKER_SOCK}" ]] && DOCKER_HOST="unix://${DOCKER_SOCK}" docker info >/dev/null 2>&1; then
    echo "[OK] local Docker daemon ready"
    echo "     export DOCKER_HOST=unix://${DOCKER_SOCK}"
    DOCKER_HOST="unix://${DOCKER_SOCK}" docker info --format 'ServerVersion={{.ServerVersion}} Driver={{.Driver}}'
    exit 0
  fi
  sleep 0.5
done

echo "[FAIL] dockerd did not become ready; last log:" >&2
tail -40 "${BASE}/logs/dockerd.log" >&2 || true
exit 1
