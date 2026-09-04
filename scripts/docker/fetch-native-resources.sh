#!/usr/bin/env bash
# 按 resources/manifest.json 下载并校验 Paddle 模型与 ONNX Runtime。
#
# 只认 manifest 中已冻结的 URL 与 SHA-256；任一文件缺失、下载失败或校验失败都非零退出，
# 不留下会被误判为可用的资源。本机与容器共用同一份合同：
#   ./scripts/docker/fetch-native-resources.sh <target_dir> [linux|darwin] [x64|arm64]
set -euo pipefail

TARGET_DIR="${1:-}"
TARGET_OS="${2:-linux}"
TARGET_ARCH="${3:-x64}"

if [[ -z "${TARGET_DIR}" ]]; then
  echo "usage: $0 <target_dir> [linux|darwin] [x64|arm64]" >&2
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANIFEST="$(cd "${SCRIPT_DIR}/../.." && pwd)/resources/manifest.json"

if [[ ! -f "${MANIFEST}" ]]; then
  echo "[FAIL] manifest not found: ${MANIFEST}" >&2
  exit 2
fi

MODEL_DIR="${TARGET_DIR}/models/ppocrv6-small"
ORT_DIR="${TARGET_DIR}/onnxruntime"
mkdir -p "${MODEL_DIR}" "${ORT_DIR}"

echo "manifest      : ${MANIFEST}"
echo "target os/arch: ${TARGET_OS}/${TARGET_ARCH}"
echo "model dir     : ${MODEL_DIR}"
echo "ort dir       : ${ORT_DIR}"

# --- Paddle 模型 -------------------------------------------------------------
# manifest 的 paddle.install_subdir 与 cpp/src/models 的默认布局一致。
mapfile -t ROLES < <(jq -r '.paddle.files[]?.role // empty' "${MANIFEST}")
if [[ ${#ROLES[@]} -eq 0 ]]; then
  echo "[FAIL] manifest declares no paddle model files" >&2
  exit 1
fi

for role in "${ROLES[@]}"; do
  filename="$(jq -r --arg r "${role}" '.paddle.files[] | select(.role==$r) | .filename' "${MANIFEST}")"
  sha256="$(jq -r --arg r "${role}" '.paddle.files[] | select(.role==$r) | .sha256' "${MANIFEST}")"
  url="$(jq -r --arg r "${role}" '.paddle.files[] | select(.role==$r) | .url' "${MANIFEST}")"

  if [[ -z "${filename}" || "${filename}" == "null" ]]; then
    echo "[FAIL] role ${role} missing filename in manifest" >&2
    exit 1
  fi
  if [[ -z "${sha256}" || "${sha256}" == "null" ]]; then
    echo "[FAIL] ${filename} missing sha256 in manifest; refusing to download" >&2
    exit 1
  fi
  if [[ -z "${url}" || "${url}" == "null" ]]; then
    echo "[FAIL] ${filename} missing url in manifest" >&2
    exit 1
  fi

  echo "[model] ${filename}"
  curl -fL --retry 3 --retry-delay 2 -o "${MODEL_DIR}/${filename}.part" "${url}"
  echo "${sha256}  ${MODEL_DIR}/${filename}.part" | sha256sum -c -
  mv "${MODEL_DIR}/${filename}.part" "${MODEL_DIR}/${filename}"
done

# --- ONNX Runtime ------------------------------------------------------------
ORT_ENTRY="$(jq -c --arg os "${TARGET_OS}" --arg arch "${TARGET_ARCH}" \
  '[.onnxruntime[] | select(.os==$os and .arch==$arch)][0] // empty' "${MANIFEST}")"

if [[ -z "${ORT_ENTRY}" ]]; then
  echo "[FAIL] manifest has no onnxruntime entry for ${TARGET_OS}/${TARGET_ARCH}" >&2
  exit 1
fi

ORT_URL="$(jq -r '.url' <<<"${ORT_ENTRY}")"
ORT_TARBALL_SHA="$(jq -r '.tarball_sha256' <<<"${ORT_ENTRY}")"
ORT_LIB_SHA="$(jq -r '.sha256' <<<"${ORT_ENTRY}")"
ORT_LIB="$(jq -r '.library' <<<"${ORT_ENTRY}")"
ORT_VERSION="$(jq -r '.version' <<<"${ORT_ENTRY}")"

if [[ -z "${ORT_URL}" || "${ORT_URL}" == "null" ]]; then
  echo "[FAIL] onnxruntime ${TARGET_OS}/${TARGET_ARCH} has no url in manifest" >&2
  exit 1
fi
if [[ -z "${ORT_LIB_SHA}" || "${ORT_LIB_SHA}" == "null" ]]; then
  echo "[FAIL] onnxruntime ${ORT_LIB} missing library sha256 in manifest" >&2
  exit 1
fi

echo "[onnxruntime] ${ORT_VERSION} ${ORT_LIB}"
curl -fL --retry 3 --retry-delay 2 -o "${ORT_DIR}/ort.tgz.part" "${ORT_URL}"
if [[ -n "${ORT_TARBALL_SHA}" && "${ORT_TARBALL_SHA}" != "null" ]]; then
  echo "${ORT_TARBALL_SHA}  ${ORT_DIR}/ort.tgz.part" | sha256sum -c -
else
  echo "[FAIL] onnxruntime tarball sha256 missing in manifest; refusing to unpack" >&2
  exit 1
fi

# 官方 tarball 顶层为 onnxruntime-<os>-<arch>-<version>/；只取头文件与库文件。
tar -xzf "${ORT_DIR}/ort.tgz.part" -C "${ORT_DIR}"
rm -f "${ORT_DIR}/ort.tgz.part"

ORT_ROOT="$(find "${ORT_DIR}" -maxdepth 2 -type d -name 'onnxruntime-*' | head -n 1)"
if [[ -z "${ORT_ROOT}" ]]; then
  echo "[FAIL] unpacked onnxruntime tarball has no onnxruntime-* directory" >&2
  exit 1
fi

mkdir -p "${ORT_DIR}/include" "${ORT_DIR}/lib"
cp -R "${ORT_ROOT}"/include/. "${ORT_DIR}/include/"
cp -R "${ORT_ROOT}"/lib/. "${ORT_DIR}/lib/"

if [[ ! -f "${ORT_DIR}/lib/${ORT_LIB}" ]]; then
  echo "[FAIL] ${ORT_LIB} not present after unpack" >&2
  exit 1
fi

echo "${ORT_LIB_SHA}  ${ORT_DIR}/lib/${ORT_LIB}" | sha256sum -c -

# CMake 的 FindONNXRuntime 与 SUBLIFT_BUNDLE_ONNXRUNTIME 需要 ABI 名的实体文件。
cp -f "${ORT_DIR}/lib/${ORT_LIB}" "${ORT_DIR}/lib/libonnxruntime.so.1"
rm -rf "${ORT_ROOT}"

echo "[OK] native resources ready in ${TARGET_DIR}"
