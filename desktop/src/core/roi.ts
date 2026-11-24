import type { ContainRect, DisplayRect, Point2D, SourceBox } from './types';

export const DEFAULT_BAND_BOTTOM_RATIO = 0.30;
export const DEFAULT_AUTO_BOTTOM_RATIO = 0.45;
export const MIN_NEW_ROI_DRAG_PX = 4;

/**
 * 基于权威解码分辨率 (srcW, srcH) 与舞台容器尺寸 (boxW, boxH) 计算严格等比 contain 显示矩形。
 * 舞台尺寸 <= 0、源尺寸 < 1 或存在非有限数字时返回 null（与 roundAndClampEdges 保持一致）。
 */
export function computeContainRect(
  boxW: number,
  boxH: number,
  srcW: number,
  srcH: number
): ContainRect | null {
  if (
    !Number.isFinite(boxW) ||
    !Number.isFinite(boxH) ||
    !Number.isFinite(srcW) ||
    !Number.isFinite(srcH) ||
    boxW <= 0 ||
    boxH <= 0 ||
    srcW < 1 ||
    srcH < 1
  ) {
    return null;
  }
  const scale = Math.min(boxW / srcW, boxH / srcH);
  const drawW = srcW * scale;
  const drawH = srcH * scale;
  const offsetX = (boxW - drawW) / 2;
  const offsetY = (boxH - drawH) / 2;
  return { scale, drawW, drawH, offsetX, offsetY };
}

/**
 * 判断画布局部坐标是否位于有效视频内容区域内（屏蔽上下/左右 letterbox 黑边）。
 */
export function isPointInContainContent(
  px: number,
  py: number,
  contain: ContainRect
): boolean {
  if (!Number.isFinite(px) || !Number.isFinite(py)) {
    return false;
  }
  return (
    px >= contain.offsetX &&
    px <= contain.offsetX + contain.drawW &&
    py >= contain.offsetY &&
    py <= contain.offsetY + contain.drawH
  );
}

/**
 * 将画布坐标钳位至有效视频内容矩形 [offsetX, offsetX + drawW] x [offsetY, offsetY + drawH]。
 */
export function clampPointToContainContent(
  px: number,
  py: number,
  contain: ContainRect
): Point2D {
  const safeX = Number.isFinite(px) ? px : contain.offsetX;
  const safeY = Number.isFinite(py) ? py : contain.offsetY;
  return {
    x: Math.max(contain.offsetX, Math.min(contain.offsetX + contain.drawW, safeX)),
    y: Math.max(contain.offsetY, Math.min(contain.offsetY + contain.drawH, safeY)),
  };
}

/**
 * 采用 [x0, x1] / [y0, y1] 边取整与边界钳位，避免独立 (x, width) 取整导致的右/下边界抖动或越界。
 * 保证 0 <= x, width >= 1, x + width <= floor(srcW), 0 <= y, height >= 1, y + height <= floor(srcH)。
 */
export function roundAndClampEdges(
  fx0: number,
  fy0: number,
  fx1: number,
  fy1: number,
  srcW: number,
  srcH: number
): SourceBox | null {
  if (
    !Number.isFinite(fx0) ||
    !Number.isFinite(fy0) ||
    !Number.isFinite(fx1) ||
    !Number.isFinite(fy1) ||
    !Number.isFinite(srcW) ||
    !Number.isFinite(srcH) ||
    srcW < 1 ||
    srcH < 1
  ) {
    return null;
  }
  const maxW = Math.floor(srcW);
  const maxH = Math.floor(srcH);
  if (maxW < 1 || maxH < 1) {
    return null;
  }

  const left = Math.min(fx0, fx1);
  const right = Math.max(fx0, fx1);
  const top = Math.min(fy0, fy1);
  const bottom = Math.max(fy0, fy1);

  let x0 = Math.max(0, Math.min(maxW - 1, Math.round(left)));
  let x1 = Math.max(1, Math.min(maxW, Math.round(right)));
  if (x1 <= x0) {
    if (x0 + 1 <= maxW) {
      x1 = x0 + 1;
    } else {
      x0 = Math.max(0, maxW - 1);
      x1 = maxW;
    }
  }

  let y0 = Math.max(0, Math.min(maxH - 1, Math.round(top)));
  let y1 = Math.max(1, Math.min(maxH, Math.round(bottom)));
  if (y1 <= y0) {
    if (y0 + 1 <= maxH) {
      y1 = y0 + 1;
    } else {
      y0 = Math.max(0, maxH - 1);
      y1 = maxH;
    }
  }

  return {
    x: x0,
    y: y0,
    width: x1 - x0,
    height: y1 - y0,
  };
}

/**
 * 将画布上的两个点投影为源视频像素坐标系下的合法 SourceBox。
 */
export function containPointsToSourceBox(
  p1: Point2D,
  p2: Point2D,
  contain: ContainRect,
  srcW: number,
  srcH: number
): SourceBox | null {
  if (contain.scale <= 0 || !Number.isFinite(contain.scale)) {
    return null;
  }
  const c1 = clampPointToContainContent(p1.x, p1.y, contain);
  const c2 = clampPointToContainContent(p2.x, p2.y, contain);
  const fx0 = (c1.x - contain.offsetX) / contain.scale;
  const fy0 = (c1.y - contain.offsetY) / contain.scale;
  const fx1 = (c2.x - contain.offsetX) / contain.scale;
  const fy1 = (c2.y - contain.offsetY) / contain.scale;
  return roundAndClampEdges(fx0, fy0, fx1, fy1, srcW, srcH);
}

/**
 * 将源视频像素 SourceBox 反向投影至当前 contain 显示舞台坐标矩形。
 */
export function sourceBoxToContainRect(
  box: SourceBox,
  contain: ContainRect
): DisplayRect {
  return {
    left: contain.offsetX + box.x * contain.scale,
    top: contain.offsetY + box.y * contain.scale,
    width: box.width * contain.scale,
    height: box.height * contain.scale,
  };
}

/**
 * 生成默认全宽底部字幕带（默认底部 30%）。
 */
export function createDefaultBandBox(
  srcW: number,
  srcH: number,
  bottomRatio: number = DEFAULT_BAND_BOTTOM_RATIO
): SourceBox | null {
  if (!Number.isFinite(srcW) || !Number.isFinite(srcH) || srcW < 1 || srcH < 1) {
    return null;
  }
  const maxW = Math.floor(srcW);
  const maxH = Math.floor(srcH);
  const ratio = Number.isFinite(bottomRatio)
    ? Math.max(0.01, Math.min(1, bottomRatio))
    : DEFAULT_BAND_BOTTOM_RATIO;
  const height = Math.max(1, Math.min(maxH, Math.round(maxH * ratio)));
  const y = Math.max(0, maxH - height);
  return {
    x: 0,
    y,
    width: maxW,
    height,
  };
}

/**
 * 将任意框选收敛为字幕带（锁定 x = 0, width = floor(srcW)），并对 y 边执行严格取整钳位。
 */
export function toBandSourceBox(
  box: SourceBox,
  srcW: number,
  srcH: number
): SourceBox | null {
  const clamped = roundAndClampEdges(
    0,
    box.y,
    Math.floor(srcW),
    box.y + box.height,
    srcW,
    srcH
  );
  if (!clamped) {
    return null;
  }
  return {
    x: 0,
    y: clamped.y,
    width: Math.floor(srcW),
    height: clamped.height,
  };
}

/**
 * 钳位与整数化任意 SourceBox（用于 rect 模式拖拽、数字输入与键盘微调）。
 */
export function clampSourceBox(
  box: SourceBox,
  srcW: number,
  srcH: number
): SourceBox | null {
  return roundAndClampEdges(
    box.x,
    box.y,
    box.x + box.width,
    box.y + box.height,
    srcW,
    srcH
  );
}

/**
 * 判断 SourceBox 是否满足全宽字幕带特征（x === 0 && width >= floor(srcW)）。
 */
export function isFullWidthBand(box: SourceBox, srcW: number): boolean {
  if (!Number.isFinite(srcW) || srcW < 1) {
    return false;
  }
  return box.x === 0 && box.width >= Math.floor(srcW);
}

/**
 * 跨分辨率按比例缩放 SourceBox（同分辨率直接走精确整数钳位快速路径）。
 */
export function scaleSourceBox(
  box: SourceBox,
  fromW: number,
  fromH: number,
  toW: number,
  toH: number
): SourceBox | null {
  if (
    !Number.isFinite(fromW) ||
    !Number.isFinite(fromH) ||
    !Number.isFinite(toW) ||
    !Number.isFinite(toH) ||
    fromW < 1 ||
    fromH < 1 ||
    toW < 1 ||
    toH < 1
  ) {
    return null;
  }
  const fW = Math.floor(fromW);
  const fH = Math.floor(fromH);
  const tW = Math.floor(toW);
  const tH = Math.floor(toH);
  if (fW === tW && fH === tH) {
    return clampSourceBox(box, tW, tH);
  }
  const fx0 = (box.x / fW) * tW;
  const fy0 = (box.y / fH) * tH;
  const fx1 = ((box.x + box.width) / fW) * tW;
  const fy1 = ((box.y + box.height) / fH) * tH;
  return roundAndClampEdges(fx0, fy0, fx1, fy1, tW, tH);
}

/**
 * 键盘微调 SourceBox：支持平移 (dx, dy) 或调高 (dh)，自动保持在 [0, srcW] x [0, srcH] 内且尺寸不变形。
 */
export function nudgeSourceBox(
  box: SourceBox,
  srcW: number,
  srcH: number,
  delta: { dx?: number; dy?: number; dh?: number; lockBandWidth?: boolean }
): SourceBox | null {
  if (!Number.isFinite(srcW) || !Number.isFinite(srcH) || srcW < 1 || srcH < 1) {
    return null;
  }
  const maxW = Math.floor(srcW);
  const maxH = Math.floor(srcH);
  if (maxW < 1 || maxH < 1) {
    return null;
  }
  const clamped = clampSourceBox(box, maxW, maxH);
  if (!clamped) {
    return null;
  }
  const rawDx = delta.dx ?? 0;
  const rawDy = delta.dy ?? 0;
  const rawDh = delta.dh ?? 0;
  if (
    !Number.isFinite(rawDx) ||
    !Number.isFinite(rawDy) ||
    !Number.isFinite(rawDh)
  ) {
    return null;
  }
  const dx = Math.round(rawDx);
  const dy = Math.round(rawDy);
  const dh = Math.round(rawDh);

  const nextX = delta.lockBandWidth
    ? 0
    : Math.max(0, Math.min(maxW - clamped.width, clamped.x + dx));
  const nextY = Math.max(0, Math.min(maxH - clamped.height, clamped.y + dy));

  if (dh !== 0) {
    const newH = Math.max(1, Math.min(maxH - nextY, clamped.height + dh));
    const next: SourceBox = {
      x: nextX,
      y: nextY,
      width: delta.lockBandWidth ? maxW : clamped.width,
      height: newH,
    };
    return delta.lockBandWidth
      ? toBandSourceBox(next, maxW, maxH)
      : clampSourceBox(next, maxW, maxH);
  }

  return {
    x: nextX,
    y: nextY,
    width: delta.lockBandWidth ? maxW : clamped.width,
    height: clamped.height,
  };
}
