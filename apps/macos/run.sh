#!/usr/bin/env bash
# 在 macOS 26+ 上以「当前系统设计语言」运行 SubLift SwiftUI GUI。
#
# 背景（macOS 27 适配）：
#   SwiftPM 以 macOS 13.0 deployment target 链接时，ld 记录的 LC_BUILD_VERSION 的
#   sdk 字段是 13.0（而不是构建所用 SDK 的实际版本）。macOS 26 起系统按该字段决定
#   是否启用 Liquid Glass / 新控制样式；sdk < 26 时整套 UI 回退到旧版平面外观，
#   表现为「工具栏没有玻璃胶囊、按钮变成旧式圆角矩形、材质效果消失」。
#   显式向链接器传 -platform_version 可写入真实 SDK 版本，恢复现代设计。
#
# 用法：
#   apps/macos/run.sh                 # 构建（必要时）并前台运行
#   apps/macos/run.sh --release       # Release 构建运行（参数原样传给 swift build）
#
# 环境变量：
#   SUBLIFT_MACOS_SCRATCH=<dir>       自定义 --scratch-path
#   SUBLIFT_SWIFT_DISABLE_SANDBOX=1   build 时加 --disable-sandbox（受限沙箱环境用）
#   其余 SUBLIFT_EVIDENCE_* 会透传给 app（DEBUG 截图/测试 fixture）。
set -euo pipefail

cd "$(dirname "$0")"

SDK_VERSION="$(xcrun --show-sdk-version)"
MIN_MACOS="13.0"
BUILD_ARGS=(
  -Xlinker -platform_version -Xlinker macos
  -Xlinker "$MIN_MACOS" -Xlinker "$SDK_VERSION"
)
if [[ -n "${SUBLIFT_MACOS_SCRATCH:-}" ]]; then
  BUILD_ARGS+=(--scratch-path "$SUBLIFT_MACOS_SCRATCH")
fi
if [[ "${SUBLIFT_SWIFT_DISABLE_SANDBOX:-0}" == "1" ]]; then
  BUILD_ARGS+=(--disable-sandbox)
fi

swift build "${BUILD_ARGS[@]}" "$@"

BIN_DIR="$(swift build "${BUILD_ARGS[@]}" --show-bin-path "$@" 2>/dev/null || true)"
BIN="$BIN_DIR/SubLiftMac"
if [[ ! -x "$BIN" ]]; then
  echo "run.sh: SubLiftMac 可执行文件未找到（$BIN）" >&2
  exit 1
fi

LINKED_SDK="$(vtool -show-build "$BIN" 2>/dev/null | awk '/^ +sdk /{print $2; exit}')"
if [[ -z "$LINKED_SDK" ]]; then
  echo "run.sh: 无法读取 LC_BUILD_VERSION sdk（vtool 失败）" >&2
  exit 1
fi
# 26.0 起系统才会使用当前设计语言；13.x 是 SwiftPM 的旧行为，必须显式纠正。
if ! awk -v sdk="$LINKED_SDK" 'BEGIN { exit !(sdk+0 >= 26.0) }'; then
  echo "run.sh: 构建产物的 linked SDK 为 $LINKED_SDK（< 26），系统会回退旧版 UI。" >&2
  echo "run.sh: 请确认 -Xlinker -platform_version 参数生效（swift build 需支持 -Xlinker）。" >&2
  exit 1
fi

echo "run.sh: SubLiftMac（linked SDK $LINKED_SDK, deployment ${MIN_MACOS}）"
exec "$BIN" "$@"
