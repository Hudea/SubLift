#!/bin/bash
# 08511 T01–T07：直接启动 DEBUG 二进制并传入环境变量。
# 禁止 `open -g`（会丢掉 SUBLIFT_*）。每张图独立 fixture，禁止复用文件。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/apps/macos/.build/debug/SubLiftMac"
EVIDENCE_DIR="$ROOT/docs/design_ui/evidence/08511"
DELAY="${SUBLIFT_EVIDENCE_DELAY:-8}"

cd "$ROOT/apps/macos"
export PYTHONPATH=
swift build --configuration debug

mkdir -p "$EVIDENCE_DIR"

wait_for_file() {
    local out="$1"
    local timeout="${2:-25}"
    local i=0
    while [ "$i" -lt "$timeout" ]; do
        if [ -f "$out" ] && [ -s "$out" ]; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

capture() {
    local name="$1"
    shift
    local out="$EVIDENCE_DIR/${name}.png"
    rm -f "$out"
    echo "→ $name"
    env -u SUBLIFT_EVIDENCE_DARK "$@" \
        SUBLIFT_EVIDENCE_SHOT="$out" \
        SUBLIFT_EVIDENCE_DELAY="$DELAY" \
        "$APP" >/tmp/sublift-08511-${name}.log 2>&1 &
    local pid=$!
    if ! wait_for_file "$out" 30; then
        echo "  ✗ timeout: $out"
        echo "  log: /tmp/sublift-08511-${name}.log"
        tail -20 "/tmp/sublift-08511-${name}.log" || true
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        return 1
    fi
    sleep 1
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "  ✓ $out ($(wc -c < "$out") bytes)"
}

# T01 空态：无 Table、无蓝按钮、虚线 drop zone
capture T01-empty-1280 \
    SUBLIFT_EVIDENCE_TASKCENTER=EMPTY

# T02 drop 高亮（必须与 T01 不同）
capture T02-drop-highlight \
    SUBLIFT_EVIDENCE_TASKCENTER=EMPTY \
    SUBLIFT_EVIDENCE_TASKCENTER_DROP=1

# T03 真实文件夹导入：位置 + 扫描摘要 + sidecar 预览
capture T03-waiting-queue \
    SUBLIFT_EVIDENCE_TASKCENTER=EMPTY \
    SUBLIFT_EVIDENCE_SCAN=1

# T04 运行中：进度 + 已规划输出
capture T04-running \
    SUBLIFT_EVIDENCE_TASKCENTER=RUNNING

# T05 Inspector：选中 waiting，输出预览可见
capture T05-inspector \
    SUBLIFT_EVIDENCE_TASKCENTER=WAITING \
    SUBLIFT_EVIDENCE_SELECT=1

# T06 960：位置列消失，折进文件单元格
capture T06-compact-960 \
    SUBLIFT_EVIDENCE_TASKCENTER=MIXED \
    SUBLIFT_EVIDENCE_COMPACT=1 \
    SUBLIFT_EVIDENCE_WINDOW_WIDTH=960

# T07 公共输出根：Toolbar 文案 + 输出列路径变化
mkdir -p /tmp/SubLiftSRT
capture T07-output-location \
    SUBLIFT_EVIDENCE_TASKCENTER=WAITING \
    SUBLIFT_EVIDENCE_SELECT=1 \
    SUBLIFT_EVIDENCE_OUTPUT_ROOT=/tmp/SubLiftSRT

echo
echo "=== checksums ==="
cd "$EVIDENCE_DIR"
md5 T01-empty-1280.png T02-drop-highlight.png T03-waiting-queue.png \
    T04-running.png T05-inspector.png T06-compact-960.png T07-output-location.png
