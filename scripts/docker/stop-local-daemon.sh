#!/usr/bin/env bash
# 停止由 start-local-daemon.sh 在 debug/docker/ 启动的本地 daemon。
# 不停止系统级 Docker 服务。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BASE="${ROOT_DIR}/debug/docker"

stop_pidfile() {
  local pidfile="$1"
  local name="$2"
  if [[ ! -f "${pidfile}" ]]; then
    return 0
  fi
  local pid
  pid="$(cat "${pidfile}" 2>/dev/null || true)"
  if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
    echo "stopping ${name} pid=${pid}"
    kill "${pid}" 2>/dev/null || true
    for _ in $(seq 1 20); do
      if ! kill -0 "${pid}" 2>/dev/null; then
        break
      fi
      sleep 0.25
    done
    if kill -0 "${pid}" 2>/dev/null; then
      kill -9 "${pid}" 2>/dev/null || true
    fi
  fi
  rm -f "${pidfile}"
}

stop_pidfile "${BASE}/docker.pid" "dockerd"
stop_pidfile "${BASE}/containerd.pid" "containerd"
rm -f "${BASE}/docker.sock" "${BASE}/containerd.sock"
echo "[OK] local Docker daemon stopped"
