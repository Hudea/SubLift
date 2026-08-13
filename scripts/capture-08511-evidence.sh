#!/bin/bash
# 08511 T01-T07 截图脚本（DEBUG 构建 + EvidenceShot fixture）
set -euo pipefail

APP="/Volumes/lab/pp/SubLift/apps/macos/.build/debug/SubLiftMac"
EVIDENCE_DIR="/Volumes/lab/pp/SubLift/docs/design_ui/evidence/08511"
DELAY=10

mkdir -p "$EVIDENCE_DIR"

capture() {
    local name="$1"
    local out="$EVIDENCE_DIR/$name.png"
    shift
    echo "→ Capturing $name ..."
    env "$@" SUBLIFT_EVIDENCE_SHOT="$out" SUBLIFT_EVIDENCE_DELAY="$DELAY" "$APP" &
    APP_PID=$!
    sleep $((DELAY + 5))
    kill $APP_PID 2>/dev/null || true
    wait $APP_PID 2>/dev/null || true
    if [ -f "$out" ]; then
        echo "  ✓ $out ($(ls -lh "$out" | awk '{print $5}'))"
    else
        echo "  ✗ FAILED: $out not created"
    fi
    sleep 2
}

# T01: 空态 1280 — 用 open -g 后台启动
echo "→ Capturing T01-empty-1280 ..."
T01="$EVIDENCE_DIR/T01-empty-1280.png"
rm -f "$T01"
SUBLIFT_EVIDENCE_SHOT="$T01" SUBLIFT_EVIDENCE_DELAY="$DELAY" SUBLIFT_EVIDENCE_TASKCENTER=EMPTY open -g "$APP"
sleep $((DELAY + 5))
if [ -f "$T01" ]; then
    echo "  ✓ $T01 ($(ls -lh "$T01" | awk '{print $5}'))"
else
    echo "  ✗ FAILED: $T01 not created"
fi
sleep 2

# T02: 空态 drop 高亮
echo "→ Capturing T02-drop-highlight ..."
T02="$EVIDENCE_DIR/T02-drop-highlight.png"
rm -f "$T02"
SUBLIFT_EVIDENCE_SHOT="$T02" SUBLIFT_EVIDENCE_DELAY="$DELAY" SUBLIFT_EVIDENCE_TASKCENTER=EMPTY SUBLIFT_EVIDENCE_TASKCENTER_DROP=1 open -g "$APP"
sleep $((DELAY + 5))
if [ -f "$T02" ]; then
    echo "  ✓ $T02 ($(ls -lh "$T02" | awk '{print $5}'))"
else
    echo "  ✗ FAILED: $T02 not created"
fi
sleep 2

# T03: 等待队列（位置列可见 ≥1100）
capture "T03-waiting-queue" \
    SUBLIFT_EVIDENCE_TASKCENTER=WAITING

# T04: 运行中
capture "T04-running" \
    SUBLIFT_EVIDENCE_TASKCENTER=RUNNING

# T05: Inspector（单选 waiting）
capture "T05-inspector" \
    SUBLIFT_EVIDENCE_TASKCENTER=WAITING

# T06: 混合 + 窄屏 960
capture "T06-compact-960" \
    SUBLIFT_EVIDENCE_TASKCENTER=MIXED \
    SUBLIFT_EVIDENCE_COMPACT=1 \
    SUBLIFT_EVIDENCE_WINDOW_WIDTH=960

# T07: 输出位置菜单（Accent）
capture "T07-output-location" \
    SUBLIFT_EVIDENCE_TASKCENTER=WAITING \
    SUBLIFT_EVIDENCE_ACCENT=1

echo ""
echo "=== Done ==="
ls -lh "$EVIDENCE_DIR"/*.png 2>/dev/null || echo "No PNGs found"
