<template>
  <div 
    ref="containerRef" 
    class="sl-roi-overlay-container"
    :class="{ 'is-locked': isLocked }"
    @pointerdown="onPointerDown"
    @pointermove="onPointerMove"
    @pointerup="onPointerUp"
    @pointercancel="onPointerUp"
    @mouseleave="onMouseLeave"
  >
    <canvas ref="canvasRef" class="sl-roi-canvas" />

    <!-- 实时悬浮尺寸与坐标 Tooltip 胶囊 -->
    <transition name="sl-tooltip-fade">
      <div 
        v-if="activeAction !== 'idle' && tooltipVisible" 
        class="sl-roi-tooltip"
        :style="tooltipStyle"
      >
        <span class="sl-roi-tooltip-dim">{{ pixelDimensions.width }} × {{ pixelDimensions.height }} px</span>
        <span class="sl-roi-tooltip-pct">({{ (localBox.width * 100).toFixed(1) }}% × {{ (localBox.height * 100).toFixed(1) }}%)</span>
      </div>
    </transition>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted, onBeforeUnmount, reactive } from 'vue';
import { useWorkbenchStore } from '../stores/workbench';
import type { VideoMetadata } from '../composables/useVideoPlayer';
import type { NormalizedRegionBox } from '../types/api';
import {
  calculateVideoViewport,
  normalizedToCanvas,
  canvasToNormalized,
  getHandleGeometries,
  hitTestHandle,
  isPointInsideBox,
  isPointInsideViewport,
  type ViewportRect,
  type CanvasBox,
  type ResizeHandleType,
} from '../utils/coordinate_mapper';

const props = defineProps<{
  metadata: VideoMetadata;
}>();

const workbenchStore = useWorkbenchStore();
const isLocked = computed(() => workbenchStore.isLocked);

const containerRef = ref<HTMLDivElement | null>(null);
const canvasRef = ref<HTMLCanvasElement | null>(null);

// 本地选区状态（归一化 0.0 ~ 1.0）
const localBox = reactive<NormalizedRegionBox>({
  x: workbenchStore.regionBox.x ?? 0.0,
  y: workbenchStore.regionBox.y ?? 0.7,
  width: workbenchStore.regionBox.width ?? 1.0,
  height: workbenchStore.regionBox.height ?? 0.3,
});

// 监听 Pinia store 变化（外部点击底部预设或全画幅预设时即时重绘）
watch(
  () => workbenchStore.regionBox,
  (newBox) => {
    if (activeAction.value === 'idle') {
      localBox.x = newBox.x;
      localBox.y = newBox.y;
      localBox.width = newBox.width;
      localBox.height = newBox.height;
      scheduleRender();
    }
  },
  { deep: true }
);

// 交互状态机
type ActionType = 'idle' | 'create' | 'move' | 'resize';
const activeAction = ref<ActionType>('idle');
const activeHandle = ref<ResizeHandleType | null>(null);

// 拖拽起始锚点
const dragStartPoint = { x: 0, y: 0 };
const dragStartBox: CanvasBox = { x: 0, y: 0, width: 0, height: 0 };

// 视口与画布尺寸
let viewport: ViewportRect = { x: 0, y: 0, width: 0, height: 0 };
let containerWidth = 0;
let containerHeight = 0;
let resizeObserver: ResizeObserver | null = null;
let rafId: number | null = null;

// Tooltip 状态
const tooltipVisible = ref(false);
const tooltipPos = reactive({ x: 0, y: 0 });

const pixelDimensions = computed(() => {
  const vw = props.metadata.videoWidth || 1920;
  const vh = props.metadata.videoHeight || 1080;
  return {
    width: Math.round(localBox.width * vw),
    height: Math.round(localBox.height * vh),
  };
});

const tooltipStyle = computed(() => ({
  transform: `translate3d(${tooltipPos.x}px, ${tooltipPos.y}px, 0)`,
}));

/**
 * 调度下一帧重绘 (rAF 防抖)
 */
function scheduleRender() {
  if (rafId !== null) return;
  rafId = requestAnimationFrame(() => {
    renderCanvas();
    rafId = null;
  });
}

/**
 * 更新容器与视口尺寸 (Retina 画布)
 */
function updateDimensions() {
  if (!containerRef.value || !canvasRef.value) return;
  const rect = containerRef.value.getBoundingClientRect();
  containerWidth = rect.width;
  containerHeight = rect.height;

  const dpr = window.devicePixelRatio || 1;
  const canvas = canvasRef.value;
  canvas.width = Math.round(containerWidth * dpr);
  canvas.height = Math.round(containerHeight * dpr);
  canvas.style.width = `${containerWidth}px`;
  canvas.style.height = `${containerHeight}px`;

  viewport = calculateVideoViewport(containerWidth, containerHeight, props.metadata);
  scheduleRender();
}

/**
 * 核心 Canvas 渲染管线
 */
function renderCanvas() {
  const canvas = canvasRef.value;
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;

  const dpr = window.devicePixelRatio || 1;
  ctx.save();
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.scale(dpr, dpr);

  if (viewport.width <= 0 || viewport.height <= 0) {
    ctx.restore();
    return;
  }

  const canvasBox = normalizedToCanvas(localBox, viewport);

  // 1. 外部暗化遮罩 (EvenOdd 裁剪仅作用于有效视频视口内部)
  ctx.save();
  ctx.beginPath();
  ctx.rect(viewport.x, viewport.y, viewport.width, viewport.height);
  ctx.rect(canvasBox.x, canvasBox.y, canvasBox.width, canvasBox.height);
  ctx.fillStyle = 'rgba(0, 0, 0, 0.42)';
  ctx.fill('evenodd');
  ctx.restore();

  // 2. 选框高亮填充与虚线边框
  ctx.save();
  ctx.fillStyle = 'rgba(10, 132, 255, 0.14)';
  ctx.fillRect(canvasBox.x, canvasBox.y, canvasBox.width, canvasBox.height);

  ctx.strokeStyle = '#0a84ff';
  ctx.lineWidth = 1.5;
  ctx.setLineDash([5, 4]);
  ctx.shadowColor = 'rgba(10, 132, 255, 0.4)';
  ctx.shadowBlur = 4;
  ctx.strokeRect(canvasBox.x, canvasBox.y, canvasBox.width, canvasBox.height);
  ctx.restore();

  // 3. 绘制 8 个控制手柄（锁定状态下不绘制手柄）
  if (!isLocked.value) {
    const handles = getHandleGeometries(canvasBox);
    const handleRadius = 4.5;

    for (const h of handles) {
      ctx.save();
      ctx.beginPath();
      ctx.arc(h.x, h.y, handleRadius, 0, Math.PI * 2);

      // 发光与立体阴影
      ctx.shadowColor = 'rgba(0, 0, 0, 0.45)';
      ctx.shadowBlur = 3;
      ctx.shadowOffsetY = 1;

      // 纯白内核与 Apple Blue 描边
      ctx.fillStyle = '#ffffff';
      ctx.fill();

      ctx.shadowColor = 'transparent';
      ctx.strokeStyle = '#0a84ff';
      ctx.lineWidth = 1.5;
      ctx.stroke();
      ctx.restore();
    }
  }

  ctx.restore();
}

/**
 * 鼠标/触控交互处理
 */
function getEventPos(e: PointerEvent): { x: number; y: number } {
  if (!containerRef.value) return { x: 0, y: 0 };
  const rect = containerRef.value.getBoundingClientRect();
  return {
    x: e.clientX - rect.x,
    y: e.clientY - rect.y,
  };
}

function updateCursor(e: PointerEvent) {
  if (isLocked.value || activeAction.value !== 'idle' || !containerRef.value) return;

  const { x, y } = getEventPos(e);
  const canvasBox = normalizedToCanvas(localBox, viewport);
  const handles = getHandleGeometries(canvasBox);
  const hitHandle = hitTestHandle(x, y, handles, 8);

  if (hitHandle) {
    containerRef.value.style.cursor = hitHandle.cursor;
  } else if (isPointInsideBox(x, y, canvasBox)) {
    containerRef.value.style.cursor = 'move';
  } else if (isPointInsideViewport(x, y, viewport)) {
    containerRef.value.style.cursor = 'crosshair';
  } else {
    containerRef.value.style.cursor = 'default';
  }
}

function onPointerDown(e: PointerEvent) {
  if (isLocked.value || e.button !== 0 || !containerRef.value) return;

  containerRef.value.setPointerCapture(e.pointerId);
  const { x, y } = getEventPos(e);
  dragStartPoint.x = x;
  dragStartPoint.y = y;

  const canvasBox = normalizedToCanvas(localBox, viewport);
  Object.assign(dragStartBox, canvasBox);

  const handles = getHandleGeometries(canvasBox);
  const hitHandle = hitTestHandle(x, y, handles, 8);

  if (hitHandle) {
    activeAction.value = 'resize';
    activeHandle.value = hitHandle.type;
  } else if (isPointInsideBox(x, y, canvasBox)) {
    activeAction.value = 'move';
  } else if (isPointInsideViewport(x, y, viewport)) {
    activeAction.value = 'create';
    const clampedX = Math.max(viewport.x, Math.min(viewport.x + viewport.width, x));
    const clampedY = Math.max(viewport.y, Math.min(viewport.y + viewport.height, y));
    dragStartPoint.x = clampedX;
    dragStartPoint.y = clampedY;
    dragStartBox.x = clampedX;
    dragStartBox.y = clampedY;
    dragStartBox.width = 0;
    dragStartBox.height = 0;
  }

  if (activeAction.value !== 'idle') {
    tooltipVisible.value = true;
    updateTooltipPosition();
  }
}

function onPointerMove(e: PointerEvent) {
  const { x, y } = getEventPos(e);

  if (activeAction.value === 'idle') {
    updateCursor(e);
    return;
  }

  const dx = x - dragStartPoint.x;
  const dy = y - dragStartPoint.y;
  const newCanvasBox: CanvasBox = { ...dragStartBox };

  if (activeAction.value === 'move') {
    newCanvasBox.x = Math.max(viewport.x, Math.min(viewport.x + viewport.width - dragStartBox.width, dragStartBox.x + dx));
    newCanvasBox.y = Math.max(viewport.y, Math.min(viewport.y + viewport.height - dragStartBox.height, dragStartBox.y + dy));
  } else if (activeAction.value === 'create') {
    const curX = Math.max(viewport.x, Math.min(viewport.x + viewport.width, x));
    const curY = Math.max(viewport.y, Math.min(viewport.y + viewport.height, y));
    newCanvasBox.x = Math.min(dragStartPoint.x, curX);
    newCanvasBox.y = Math.min(dragStartPoint.y, curY);
    newCanvasBox.width = Math.abs(curX - dragStartPoint.x);
    newCanvasBox.height = Math.abs(curY - dragStartPoint.y);
  } else if (activeAction.value === 'resize' && activeHandle.value) {
    const handle = activeHandle.value;
    let left = dragStartBox.x;
    let top = dragStartBox.y;
    let right = dragStartBox.x + dragStartBox.width;
    let bottom = dragStartBox.y + dragStartBox.height;

    if (handle.includes('w')) left = Math.max(viewport.x, Math.min(right - 10, dragStartBox.x + dx));
    if (handle.includes('e')) right = Math.min(viewport.x + viewport.width, Math.max(left + 10, dragStartBox.x + dragStartBox.width + dx));
    if (handle.includes('n')) top = Math.max(viewport.y, Math.min(bottom - 10, dragStartBox.y + dy));
    if (handle.includes('s')) bottom = Math.min(viewport.y + viewport.height, Math.max(top + 10, dragStartBox.y + dragStartBox.height + dy));

    newCanvasBox.x = left;
    newCanvasBox.y = top;
    newCanvasBox.width = right - left;
    newCanvasBox.height = bottom - top;
  }

  const normalized = canvasToNormalized(newCanvasBox, viewport);
  Object.assign(localBox, normalized);
  workbenchStore.updateRegionBox(normalized);

  updateTooltipPosition();
  scheduleRender();
}

function onPointerUp(e: PointerEvent) {
  if (activeAction.value === 'idle') return;

  if (containerRef.value && containerRef.value.hasPointerCapture(e.pointerId)) {
    containerRef.value.releasePointerCapture(e.pointerId);
  }

  // 极小选区自动重置为默认底部 30%
  if (localBox.width < 0.02 || localBox.height < 0.02) {
    workbenchStore.resetDefaultBottomRoi();
  }

  activeAction.value = 'idle';
  activeHandle.value = null;
  tooltipVisible.value = false;
  scheduleRender();
}

function onMouseLeave() {
  if (activeAction.value === 'idle' && containerRef.value) {
    containerRef.value.style.cursor = 'default';
  }
}

function updateTooltipPosition() {
  const canvasBox = normalizedToCanvas(localBox, viewport);
  const tooltipX = canvasBox.x + canvasBox.width / 2;
  let tooltipY = canvasBox.y - 28;
  if (tooltipY < 10) {
    tooltipY = canvasBox.y + canvasBox.height + 8;
  }
  tooltipPos.x = Math.round(tooltipX);
  tooltipPos.y = Math.round(tooltipY);
}

watch(
  () => props.metadata,
  () => {
    updateDimensions();
  },
  { deep: true }
);

onMounted(() => {
  if (containerRef.value) {
    resizeObserver = new ResizeObserver(() => {
      updateDimensions();
    });
    resizeObserver.observe(containerRef.value);
  }
  updateDimensions();
});

onBeforeUnmount(() => {
  if (resizeObserver) {
    resizeObserver.disconnect();
  }
  if (rafId !== null) {
    cancelAnimationFrame(rafId);
  }
});
</script>

<style scoped>
.sl-roi-overlay-container {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  pointer-events: auto;
  user-select: none;
  touch-action: none;
  z-index: 10;
}

.sl-roi-overlay-container.is-locked {
  cursor: not-allowed !important;
}

.sl-roi-canvas {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  display: block;
}

/* Apple 风格悬浮尺寸 Tooltip 胶囊 */
.sl-roi-tooltip {
  position: absolute;
  top: 0;
  left: 0;
  margin-left: -70px;
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 3px 8px;
  background: var(--sl-glass-bar);
  backdrop-filter: var(--sl-glass-blur-sm);
  -webkit-backdrop-filter: var(--sl-glass-blur-sm);
  border: 1px solid var(--sl-border-focus);
  border-radius: var(--sl-radius-pill);
  box-shadow: var(--sl-shadow-floating);
  pointer-events: none;
  z-index: 25;
  white-space: nowrap;
}

.sl-roi-tooltip-dim {
  font-family: var(--sl-font-family-mono);
  font-size: var(--sl-font-size-xs);
  font-weight: 600;
  color: #ffffff;
}

.sl-roi-tooltip-pct {
  font-family: var(--sl-font-family-mono);
  font-size: 10px;
  color: var(--sl-color-accent-hover);
}

.sl-tooltip-fade-enter-active,
.sl-tooltip-fade-leave-active {
  transition: opacity 120ms ease, transform 120ms ease;
}

.sl-tooltip-fade-enter-from,
.sl-tooltip-fade-leave-to {
  opacity: 0;
  transform: translateY(4px);
}
</style>
