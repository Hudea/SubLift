#!/usr/bin/env bash
# 为 Phase 14 选择可用的 Docker 后端，并尽量把项目侧配置留在仓库内。
#
# 优先级：
#   1. 系统 dockerd（/var/run/docker.sock）——本机已安装且当前用户在 docker 组时直接用
#   2. 项目目录 rootless daemon（.docker/）——需要 uidmap + rootlesskit；镜像层不进系统 /var/lib/docker
#
# 用法：
#   ./scripts/docker/local-daemon.sh start|status|stop|env
#   eval "$(./scripts/docker/local-daemon.sh env)"
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DOCKER_DIR="${ROOT_DIR}/.docker"
SOCK="${DOCKER_DIR}/docker.sock"
PIDFILE="${DOCKER_DIR}/docker.pid"
LOG="${DOCKER_DIR}/dockerd.log"
DATA="${DOCKER_DIR}/data"
EXEC_ROOT="${DOCKER_DIR}/run"
STATE_DIR="${DOCKER_DIR}/rootlesskit"

SYSTEM_SOCK="${DOCKER_HOST:-unix:///var/run/docker.sock}"
SYSTEM_SOCK="${SYSTEM_SOCK#unix://}"

usage() {
  echo "usage: $0 {start|stop|status|env}" >&2
  exit 2
}

system_docker_ready() {
  [[ -S "${SYSTEM_SOCK}" ]] && docker --host "unix://${SYSTEM_SOCK}" info >/dev/null 2>&1
}

is_local_running() {
  if [[ ! -f "${PIDFILE}" ]]; then
    return 1
  fi
  local pid
  pid="$(cat "${PIDFILE}" 2>/dev/null || true)"
  [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null
}

cmd_env() {
  if is_local_running && [[ -S "${SOCK}" ]]; then
    echo "export DOCKER_HOST=unix://${SOCK}"
    return 0
  fi
  if system_docker_ready; then
    echo "export DOCKER_HOST=unix://${SYSTEM_SOCK}"
    return 0
  fi
  echo "export DOCKER_HOST=unix://${SOCK}"
}

cmd_status() {
  if is_local_running && docker --host "unix://${SOCK}" info >/dev/null 2>&1; then
    echo "backend=project-local pid=$(cat "${PIDFILE}") sock=${SOCK}"
    docker --host "unix://${SOCK}" info --format 'server={{.ServerVersion}} driver={{.Driver}}' || true
    return 0
  fi
  if system_docker_ready; then
    echo "backend=system sock=${SYSTEM_SOCK}"
    docker --host "unix://${SYSTEM_SOCK}" info --format 'server={{.ServerVersion}} driver={{.Driver}} root={{.DockerRootDir}}' || true
    return 0
  fi
  echo "stopped"
  return 1
}

cmd_stop() {
  if ! is_local_running; then
    echo "project-local daemon not running (system docker left untouched)"
    return 0
  fi
  local pid
  pid="$(cat "${PIDFILE}")"
  echo "stopping project-local pid=${pid}"
  kill "${pid}" 2>/dev/null || true
  for _ in $(seq 1 20); do
    if ! kill -0 "${pid}" 2>/dev/null; then
      rm -f "${PIDFILE}"
      echo "stopped"
      return 0
    fi
    sleep 0.25
  done
  kill -9 "${pid}" 2>/dev/null || true
  rm -f "${PIDFILE}"
  echo "stopped (killed)"
}

cmd_start() {
  if system_docker_ready; then
    echo "using system docker at unix://${SYSTEM_SOCK}"
    echo "project compose/env live in the repo; image layers stay in the host daemon."
    echo "to force a project-local rootless daemon, install uidmap (newuidmap/newgidmap) and rerun with SUBLIFT_DOCKER_FORCE_LOCAL=1"
    if [[ "${SUBLIFT_DOCKER_FORCE_LOCAL:-}" != "1" ]]; then
      cmd_env
      return 0
    fi
  fi

  if is_local_running; then
    echo "already running pid=$(cat "${PIDFILE}")"
    cmd_env
    return 0
  fi

  if ! command -v dockerd-rootless.sh >/dev/null 2>&1; then
    echo "[FAIL] dockerd-rootless.sh not found" >&2
    exit 1
  fi
  if ! command -v newuidmap >/dev/null 2>&1 || ! command -v newgidmap >/dev/null 2>&1; then
    echo "[FAIL] project-local rootless daemon needs newuidmap/newgidmap (Debian/Ubuntu: apt install uidmap)" >&2
    echo "       system docker is the fallback when /var/run/docker.sock is reachable." >&2
    exit 1
  fi

  mkdir -p "${DATA}" "${EXEC_ROOT}" "${STATE_DIR}"
  : "${XDG_RUNTIME_DIR:=/run/user/$(id -u)}"
  if [[ ! -w "${XDG_RUNTIME_DIR}" ]]; then
    echo "[FAIL] XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR} is not writable" >&2
    exit 1
  fi

  export DOCKERD_ROOTLESS_ROOTLESSKIT_STATE_DIR="${STATE_DIR}"
  nohup env \
    HOME="${HOME}" \
    XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" \
    DOCKERD_ROOTLESS_ROOTLESSKIT_STATE_DIR="${STATE_DIR}" \
    dockerd-rootless.sh \
      --host="unix://${SOCK}" \
      --data-root="${DATA}" \
      --exec-root="${EXEC_ROOT}" \
      --pidfile="${PIDFILE}" \
      --iptables=false \
      --ip6tables=false \
    >"${LOG}" 2>&1 &

  for _ in $(seq 1 60); do
    if [[ -S "${SOCK}" ]] && docker --host "unix://${SOCK}" info >/dev/null 2>&1; then
      echo "started project-local sock=${SOCK}"
      cmd_env
      return 0
    fi
    sleep 0.25
  done

  echo "[FAIL] project-local dockerd did not become ready; last log:" >&2
  tail -n 40 "${LOG}" >&2 || true
  exit 1
}

case "${1:-}" in
  start) cmd_start ;;
  stop) cmd_stop ;;
  status) cmd_status ;;
  env) cmd_env ;;
  *) usage ;;
esac
