import { describe, it, expect } from 'vitest';
import {
  calculateVideoViewport,
  normalizedToCanvas,
  canvasToNormalized,
  screenToNormalizedPoint,
  getHandleGeometries,
  hitTestHandle,
  isPointInsideBox,
  isPointInsideViewport,
} from './coordinate_mapper';

describe('coordinate_mapper', () => {
  it('calculates Letterbox and Pillarbox video viewports correctly', () => {
    // 16:9 video in 4:3 container -> Letterbox (Top/Bottom black bars)
    const lb = calculateVideoViewport(800, 600, { videoWidth: 1920, videoHeight: 1080, duration: 10, aspectRatio: 16 / 9 });
    expect(lb.x).toBe(0);
    expect(lb.y).toBe(75);
    expect(lb.width).toBe(800);
    expect(lb.height).toBe(450);

    // 9:16 vertical video in 16:9 container -> Pillarbox (Left/Right black bars)
    const pb = calculateVideoViewport(1280, 720, { videoWidth: 1080, videoHeight: 1920, duration: 10, aspectRatio: 9 / 16 });
    expect(pb.x).toBe(437.5);
    expect(pb.y).toBe(0);
    expect(pb.width).toBe(405);
    expect(pb.height).toBe(720);

    // 21:9 Ultrawide Video in 16:9 container -> Letterbox
    const uw = calculateVideoViewport(1920, 1080, { videoWidth: 2560, videoHeight: 1080, duration: 10, aspectRatio: 2560 / 1080 });
    expect(uw.x).toBe(0);
    expect(uw.y).toBeGreaterThan(0);
    expect(uw.width).toBe(1920);

    // Zero / degraded dimensions
    const zeroVp = calculateVideoViewport(0, 0, { videoWidth: 0, videoHeight: 0, duration: 0, aspectRatio: 16 / 9 });
    expect(zeroVp.width).toBe(0);
    expect(zeroVp.height).toBe(0);
  });

  it('converts normalized and canvas coordinates bidirectionally', () => {
    const viewport = { x: 0, y: 50, width: 800, height: 450 };
    const normBox = { x: 0.1, y: 0.7, width: 0.8, height: 0.25 };

    const canvasBox = normalizedToCanvas(normBox, viewport);
    expect(canvasBox.x).toBe(80);
    expect(canvasBox.y).toBe(365);
    expect(canvasBox.width).toBe(640);
    expect(canvasBox.height).toBe(112.5);

    const roundTrip = canvasToNormalized(canvasBox, viewport);
    expect(roundTrip.x).toBeCloseTo(normBox.x, 3);
    expect(roundTrip.y).toBeCloseTo(normBox.y, 3);
    expect(roundTrip.width).toBeCloseTo(normBox.width, 3);
    expect(roundTrip.height).toBeCloseTo(normBox.height, 3);

    const normPoint = screenToNormalizedPoint({ x: canvasBox.x, y: canvasBox.y }, viewport);
    expect(normPoint.x).toBeCloseTo(normBox.x, 3);
    expect(normPoint.y).toBeCloseTo(normBox.y, 3);
  });

  it('enforces safety clamping on extreme bounding boxes', () => {
    const viewport = { x: 0, y: 50, width: 800, height: 450 };
    const extremeBox = canvasToNormalized({ x: 9999, y: 9999, width: 500, height: 500 }, viewport);
    expect(extremeBox.x).toBeLessThanOrEqual(0.99);
    expect(extremeBox.y).toBeLessThanOrEqual(0.99);
    expect(Number((extremeBox.x + extremeBox.width).toFixed(4))).toBeLessThanOrEqual(1.0);
    expect(Number((extremeBox.y + extremeBox.height).toFixed(4))).toBeLessThanOrEqual(1.0);
  });

  it('calculates handle geometries and tests hit testing', () => {
    const viewport = { x: 0, y: 50, width: 800, height: 450 };
    const normBox = { x: 0.1, y: 0.7, width: 0.8, height: 0.25 };
    const canvasBox = normalizedToCanvas(normBox, viewport);
    const handles = getHandleGeometries(canvasBox);
    expect(handles.length).toBe(8);

    for (const h of handles) {
      const hit = hitTestHandle(h.x, h.y, handles, 8);
      expect(hit).not.toBeNull();
      expect(hit?.type).toBe(h.type);
    }

    const miss = hitTestHandle(0, 0, handles, 8);
    expect(miss).toBeNull();
  });

  it('verifies point inside box and viewport checks', () => {
    const viewport = { x: 0, y: 50, width: 800, height: 450 };
    const normBox = { x: 0.1, y: 0.7, width: 0.8, height: 0.25 };
    const canvasBox = normalizedToCanvas(normBox, viewport);

    expect(isPointInsideBox(canvasBox.x + 10, canvasBox.y + 10, canvasBox)).toBe(true);
    expect(isPointInsideBox(0, 0, canvasBox)).toBe(false);
    expect(isPointInsideViewport(400, 200, viewport)).toBe(true);
    expect(isPointInsideViewport(400, 10, viewport)).toBe(false);
  });
});
