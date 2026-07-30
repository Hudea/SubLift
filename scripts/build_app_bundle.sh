#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

NO_BUILD=0
CHECK_ONLY=0

for arg in "$@"; do
  case "${arg}" in
    --no-build) NO_BUILD=1 ;;
    --check) CHECK_ONLY=1 ;;
  esac
done

BUNDLE_DIR="${ROOT_DIR}/build/SubLift.app"

if [[ ${CHECK_ONLY} -eq 1 ]]; then
  python3 "${SCRIPT_DIR}/check_app_bundle.py" --bundle-path "${BUNDLE_DIR}"
  exit 0
fi

echo "=============================="
echo " SubLift App Bundle Packaging "
echo "=============================="

if [[ ${NO_BUILD} -eq 0 ]]; then
  cmake -B "${ROOT_DIR}/build/cpp" "${ROOT_DIR}/cpp"
  cmake --build "${ROOT_DIR}/build/cpp" -j
fi

# Clean and recreate Bundle structure
rm -rf "${BUNDLE_DIR}"
mkdir -p "${BUNDLE_DIR}/Contents/MacOS"
mkdir -p "${BUNDLE_DIR}/Contents/Helpers"
mkdir -p "${BUNDLE_DIR}/Contents/Resources/models"
mkdir -p "${BUNDLE_DIR}/Contents/Resources/bin"
mkdir -p "${BUNDLE_DIR}/Contents/Frameworks"

# Copy binaries
cp "${ROOT_DIR}/build/cpp/bin/sublift_cli" "${BUNDLE_DIR}/Contents/MacOS/sublift_cli"
cp "${ROOT_DIR}/build/cpp/bin/sublift_worker" "${BUNDLE_DIR}/Contents/Helpers/sublift_worker"
chmod +x "${BUNDLE_DIR}/Contents/MacOS/sublift_cli"
chmod +x "${BUNDLE_DIR}/Contents/Helpers/sublift_worker"

# Inject relative RPATH for Frameworks lookup
install_name_tool -add_rpath "@executable_path/../Frameworks" "${BUNDLE_DIR}/Contents/MacOS/sublift_cli"
install_name_tool -add_rpath "@executable_path/../Frameworks" "${BUNDLE_DIR}/Contents/Helpers/sublift_worker"

# Copy Info.plist
cp "${ROOT_DIR}/cpp/packaging/macos/Info.plist.in" "${BUNDLE_DIR}/Contents/Info.plist"

# Enable nullglob for safe copying
shopt -s nullglob

# Copy models if available in user cache
MODEL_CACHE="${HOME}/.cache/sublift/rapidocr-models"
if [[ -d "${MODEL_CACHE}" ]]; then
  MODEL_FILES=("${MODEL_CACHE}"/*)
  if [[ ${#MODEL_FILES[@]} -gt 0 ]]; then
    cp "${MODEL_FILES[@]}" "${BUNDLE_DIR}/Contents/Resources/models/"
  fi
fi

# Copy FFmpeg binary if available
FFMPEG_BIN=""
if command -v ffmpeg &>/dev/null; then
  FFMPEG_BIN="$(command -v ffmpeg)"
elif [[ -x "/opt/homebrew/opt/ffmpeg-full/bin/ffmpeg" ]]; then
  FFMPEG_BIN="/opt/homebrew/opt/ffmpeg-full/bin/ffmpeg"
elif [[ -x "/opt/homebrew/bin/ffmpeg" ]]; then
  FFMPEG_BIN="/opt/homebrew/bin/ffmpeg"
fi

if [[ -n "${FFMPEG_BIN}" && -f "${FFMPEG_BIN}" ]]; then
  cp "${FFMPEG_BIN}" "${BUNDLE_DIR}/Contents/Resources/bin/ffmpeg"
  chmod +x "${BUNDLE_DIR}/Contents/Resources/bin/ffmpeg"
fi

# Copy ONNX Runtime shared library if built
ORT_FILES=("${ROOT_DIR}/build/cpp/lib/libonnxruntime"*".dylib")
if [[ ${#ORT_FILES[@]} -gt 0 ]]; then
  cp "${ORT_FILES[@]}" "${BUNDLE_DIR}/Contents/Frameworks/"
fi

echo "[OK] App Bundle built successfully at: ${BUNDLE_DIR}"

# Run verification script
python3 "${SCRIPT_DIR}/check_app_bundle.py" --bundle-path "${BUNDLE_DIR}"
