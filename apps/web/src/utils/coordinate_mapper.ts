import type { NormalizedRegionBox } from '../types/api';
import type { VideoMetadata } from '../composables/useVideoPlayer';

export type ResizeHandleType =
  | 'nw' | 'n' | 'ne'
  | 'e'  | 'se' | 's'
  | 'sw' | 'w';

export interface ViewportRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface CanvasBox {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface HandleGeometry {
  type: ResizeHandleType;
  x: number;
  y: number;
  cursor: string;
}

export interface Point2D {
  x: number;
  y: number;
}

function safeClamp(val: number, min: number, max: number): number {
  if (min > max) {
    return max;
  }
  return Math.max(min, Math.min(max, val));
}

/**
 * 计算 video 在 object-fit: contain 模式下的真实渲染视口矩形 (去除上下/左右黑边)
 */
export function calculateVideoViewport(
  containerWidth: number,
  containerHeight: number,
  metadata: VideoMetadata
): ViewportRect {
  const cw = Math.max(0, containerWidth);
  const ch = Math.max(0, containerHeight);
  const vw = Math.max(0, metadata.videoWidth || 0);
  const vh = Math.max(0, metadata.videoHeight || 0);

  if (cw === 0 || ch === 0 || vw === 0 || vh === 0) {
    return { x: 0, y: 0, width: cw, height: ch };
  }

  const containerRatio = cw / ch;
  const videoRatio = vw / vh;

  let width = cw;
  let height = ch;
  let x = 0;
  let y = 0;

  if (videoRatio > containerRatio) {
    // 宽对齐，上下留黑边 (Letterbox)
    width = cw;
    height = cw / videoRatio;
    y = (ch - height) / 2;
  } else {
    // 高对齐，左右留黑边 (Pillarbox)
    height = ch;
    width = ch * videoRatio;
    x = (cw - width) / 2;
  }

  return {
    x: Math.round(x * 100) / 100,
    y: Math.round(y * 100) / 100,
    width: Math.round(width * 100) / 100,
    height: Math.round(height * 100) / 100,
  };
}

/**
 * 归一化选区坐标 [0.0 ~ 1.0] -> Canvas 屏幕像素坐标
 */
export function normalizedToCanvas(norm: NormalizedRegionBox, viewport: ViewportRect): CanvasBox {
  if (viewport.width <= 0 || viewport.height <= 0) {
    return { x: 0, y: 0, width: 0, height: 0 };
  }
  return {
    x: viewport.x + norm.x * viewport.width,
    y: viewport.y + norm.y * viewport.height,
    width: norm.width * viewport.width,
    height: norm.height * viewport.height,
  };
}

/**
 * Canvas 屏幕像素坐标 -> 归一化选区坐标 [0.0 ~ 1.0] (强制 clamp 到视口内部，保证 x+w<=1.0, y+h<=1.0)
 */
export function canvasToNormalized(canvas: CanvasBox, viewport: ViewportRect): NormalizedRegionBox {
  if (viewport.width <= 0 || viewport.height <= 0) {
    return { x: 0, y: 0, width: 1, height: 1 };
  }

  const rawX = Number.isFinite(canvas.x) ? canvas.x : viewport.x;
  const rawY = Number.isFinite(canvas.y) ? canvas.y : viewport.y;
  const rawW = Number.isFinite(canvas.width) ? canvas.width : viewport.width;
  const rawH = Number.isFinite(canvas.height) ? canvas.height : viewport.height;

  // 1. 预先限制原点在 [0.0, 0.99]，确保保留至少 0.01 的选区空间
  const x = safeClamp((rawX - viewport.x) / viewport.width, 0.0, 0.99);
  const y = safeClamp((rawY - viewport.y) / viewport.height, 0.0, 0.99);

  // 2. 宽度/高度安全 clamp 到 [0.01, 1.0 - 原点]
  const width = safeClamp(rawW / viewport.width, 0.01, 1.0 - x);
  const height = safeClamp(rawH / viewport.height, 0.01, 1.0 - y);

  return {
    x: Number(x.toFixed(4)),
    y: Number(y.toFixed(4)),
    width: Number(width.toFixed(4)),
    height: Number(height.toFixed(4)),
  };
}

/**
 * 屏幕/容器 DOM 像素坐标 -> 归一化点 (0.0 ~ 1.0)
 */
export function screenToNormalizedPoint(point: Point2D, viewport: ViewportRect): Point2D {
  if (viewport.width <= 0 || viewport.height <= 0) {
    return { x: 0, y: 0 };
  }
  const relX = point.x - viewport.x;
  const relY = point.y - viewport.y;
  return {
    x: safeClamp(relX / viewport.width, 0.0, 1.0),
    y: safeClamp(relY / viewport.height, 0.0, 1.0),
  };
}

/**
 * 计算 8 个手柄的屏幕坐标与对应光标
 */
export function getHandleGeometries(box: CanvasBox): HandleGeometry[] {
  const { x, y, width: w, height: h } = box;
  const cx = x + w / 2;
  const cy = y + h / 2;

  return [
    { type: 'nw', x: x, y: y, cursor: 'nwse-resize' },
    { type: 'n', x: cx, y: y, cursor: 'ns-resize' },
    { type: 'ne', x: x + w, y: y, cursor: 'nesw-resize' },
    { type: 'e', x: x + w, y: cy, cursor: 'ew-resize' },
    { type: 'se', x: x + w, y: y + h, cursor: 'nwse-resize' },
    { type: 's', x: cx, y: y + h, cursor: 'ns-resize' },
    { type: 'sw', x: x, y: y + h, cursor: 'nesw-resize' },
    { type: 'w', x: x, y: cy, cursor: 'ew-resize' },
  ];
}

/**
 * 手柄命中测试 (Hit Test)
 */
export function hitTestHandle(
  px: number,
  py: number,
  handles: HandleGeometry[],
  handleRadius = 8
): HandleGeometry | null {
  for (const h of handles) {
    const distSq = (px - h.x) ** 2 + (py - h.y) ** 2;
    if (distSq <= handleRadius ** 2) {
      return h;
    }
  }
  return null;
}

/**
 * 选框主体命中测试
 */
export function isPointInsideBox(px: number, py: number, box: CanvasBox): boolean {
  return px >= box.x && px <= box.x + box.width && py >= box.y && py <= box.y + box.height;
}

/**
 * 视口主体命中测试
 */
export function isPointInsideViewport(px: number, py: number, viewport: ViewportRect): boolean {
  return px >= viewport.x && px <= viewport.x + viewport.width && py >= viewport.y && py <= viewport.y + viewport.height;
}
