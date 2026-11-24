import { describe, expect, it } from 'vitest';
import {
  clampPointToContainContent,
  clampSourceBox,
  computeContainRect,
  containPointsToSourceBox,
  createDefaultBandBox,
  isFullWidthBand,
  isPointInContainContent,
  nudgeSourceBox,
  roundAndClampEdges,
  scaleSourceBox,
  sourceBoxToContainRect,
  toBandSourceBox,
} from './roi';

describe('core/roi', () => {
  it('returns null when stage or source dimensions are non-positive or non-finite', () => {
    expect(computeContainRect(0, 600, 1920, 1080)).toBeNull();
    expect(computeContainRect(800, 0, 1920, 1080)).toBeNull();
    expect(computeContainRect(800, 600, -1, 1080)).toBeNull();
    expect(computeContainRect(800, 600, 1920, Number.NaN)).toBeNull();
  });

  it('computes contain rect and guards letterbox black-bar hits', () => {
    // 1920x1080 in 800x600 => scale = 800/1920 = 0.41666..., drawW = 800, drawH = 450, offsetY = 75
    const verticalBars = computeContainRect(800, 600, 1920, 1080)!;
    expect(verticalBars.drawW).toBeCloseTo(800, 5);
    expect(verticalBars.drawH).toBeCloseTo(450, 5);
    expect(verticalBars.offsetX).toBeCloseTo(0, 5);
    expect(verticalBars.offsetY).toBeCloseTo(75, 5);

    expect(isPointInContainContent(400, 20, verticalBars)).toBe(false); // top letterbox
    expect(isPointInContainContent(400, 560, verticalBars)).toBe(false); // bottom letterbox
    expect(isPointInContainContent(400, 300, verticalBars)).toBe(true);

    // 1080x1920 in 1200x600 => left/right pillarbox
    const horizontalBars = computeContainRect(1200, 600, 1080, 1920)!;
    expect(horizontalBars.offsetX).toBeGreaterThan(400);
    expect(isPointInContainContent(50, 300, horizontalBars)).toBe(false);
    expect(isPointInContainContent(600, 300, horizontalBars)).toBe(true);
  });

  it('rounds [x0, x1] edges without overflowing or shrinking to 0', () => {
    const edge1x1 = roundAndClampEdges(1919.8, 1079.9, 1925, 1085, 1920, 1080)!;
    expect(edge1x1).toEqual({ x: 1919, y: 1079, width: 1, height: 1 });
    expect(edge1x1.x + edge1x1.width).toBeLessThanOrEqual(1920);
    expect(edge1x1.y + edge1x1.height).toBeLessThanOrEqual(1080);

    const swapped = roundAndClampEdges(500.4, 400.2, 100.6, 200.8, 1920, 1080)!;
    expect(swapped).toEqual({ x: 101, y: 201, width: 399, height: 199 });
  });

  it('clamps active drag points outside content strictly inside source bounds', () => {
    const contain = computeContainRect(800, 600, 1920, 1080)!;
    const clamped = clampPointToContainContent(-50, 999, contain);
    expect(clamped.x).toBe(0);
    expect(clamped.y).toBeCloseTo(525, 5);

    const box = containPointsToSourceBox(
      { x: 0, y: 75 },
      { x: 9999, y: 9999 },
      contain,
      1920,
      1080
    )!;
    expect(box).toEqual({ x: 0, y: 0, width: 1920, height: 1080 });

    const roundtrip = sourceBoxToContainRect(box, contain);
    expect(roundtrip.left).toBeCloseTo(0, 5);
    expect(roundtrip.top).toBeCloseTo(75, 5);
    expect(roundtrip.width).toBeCloseTo(800, 5);
    expect(roundtrip.height).toBeCloseTo(450, 5);
  });

  it('enforces full-width band invariants and cross-resolution scaling', () => {
    const band = createDefaultBandBox(1920, 1080, 0.30)!;
    expect(band).toEqual({ x: 0, y: 756, width: 1920, height: 324 });
    expect(isFullWidthBand(band, 1920)).toBe(true);

    const forcedBand = toBandSourceBox(
      { x: 120, y: 700, width: 500, height: 200 },
      1920,
      1080
    )!;
    expect(forcedBand).toEqual({ x: 0, y: 700, width: 1920, height: 200 });

    const scaled720 = scaleSourceBox(band, 1920, 1080, 1280, 720)!;
    expect(scaled720).toEqual({ x: 0, y: 504, width: 1280, height: 216 });
    expect(isFullWidthBand(scaled720, 1280)).toBe(true);

    const scaledPortrait = scaleSourceBox(band, 1920, 1080, 1080, 1920)!;
    expect(scaledPortrait.x).toBe(0);
    expect(scaledPortrait.width).toBe(1080);
    expect(scaledPortrait.y + scaledPortrait.height).toBeLessThanOrEqual(1920);
  });

  it('nudges and clamps SourceBox without violating bounds', () => {
    const box = { x: 1800, y: 900, width: 100, height: 100 };
    const moved = nudgeSourceBox(box, 1920, 1080, { dx: 50, dy: 200 })!;
    expect(moved).toEqual({ x: 1820, y: 980, width: 100, height: 100 });

    const resizedH = nudgeSourceBox(moved, 1920, 1080, { dh: -30 })!;
    expect(resizedH.height).toBe(70);

    const clamped = clampSourceBox({ x: -10, y: -5, width: 5000, height: 2000 }, 1920, 1080)!;
    expect(clamped).toEqual({ x: 0, y: 0, width: 1920, height: 1080 });

    // Non-finite deltas or dimensions return null; fractional deltas round consistently
    expect(nudgeSourceBox(box, 1920, 1080, { dx: Number.NaN })).toBeNull();
    expect(nudgeSourceBox(box, 1920, 1080, { dy: Number.POSITIVE_INFINITY })).toBeNull();
    expect(nudgeSourceBox(box, 1920, 1080, { dh: Number.NEGATIVE_INFINITY })).toBeNull();
    expect(nudgeSourceBox(box, Number.NaN, 1080, { dx: 1 })).toBeNull();
    const rounded = nudgeSourceBox({ x: 100, y: 100, width: 50, height: 50 }, 1920, 1080, {
      dx: 1.6,
      dy: -2.4,
    })!;
    expect(rounded).toEqual({ x: 102, y: 98, width: 50, height: 50 });
  });

  it('handles extreme aspect ratios, 1x1 minimums, sub-1px consistency, and non-finite inputs', () => {
    // 10000x10 inside 400x400 -> top/bottom letterbox
    const wide = computeContainRect(400, 400, 10000, 10)!;
    expect(wide.drawW).toBeCloseTo(400, 5);
    expect(wide.drawH).toBeCloseTo(0.4, 5);
    expect(wide.offsetY).toBeCloseTo(199.8, 5);
    expect(isPointInContainContent(200, 100, wide)).toBe(false);
    expect(isPointInContainContent(200, 200, wide)).toBe(true);

    // 10x10000 inside 400x400 -> left/right pillarbox
    const tall = computeContainRect(400, 400, 10, 10000)!;
    expect(tall.drawH).toBeCloseTo(400, 5);
    expect(tall.drawW).toBeCloseTo(0.4, 5);
    expect(tall.offsetX).toBeCloseTo(199.8, 5);
    expect(isPointInContainContent(100, 200, tall)).toBe(false);
    expect(isPointInContainContent(200, 200, tall)).toBe(true);

    // 1x1 minimum source dimensions
    expect(computeContainRect(800, 600, 1, 1)).not.toBeNull();
    expect(roundAndClampEdges(-5, -5, 10, 10, 1, 1)).toEqual({
      x: 0,
      y: 0,
      width: 1,
      height: 1,
    });
    expect(createDefaultBandBox(1, 1, 0.3)).toEqual({ x: 0, y: 0, width: 1, height: 1 });
    expect(scaleSourceBox({ x: 0, y: 756, width: 1920, height: 324 }, 1920, 1080, 1, 1)).toEqual({
      x: 0,
      y: 0,
      width: 1,
      height: 1,
    });
    expect(nudgeSourceBox({ x: 0, y: 0, width: 1, height: 1 }, 1, 1, { dx: 1, dy: 1 })).toEqual({
      x: 0,
      y: 0,
      width: 1,
      height: 1,
    });

    // 0 < srcW < 1 consistency between computeContainRect and roundAndClampEdges
    expect(computeContainRect(800, 600, 0.5, 1080)).toBeNull();
    expect(roundAndClampEdges(0, 0, 1, 1, 0.5, 1080)).toBeNull();
    expect(computeContainRect(800, 600, 1920, 0.99)).toBeNull();
    expect(roundAndClampEdges(0, 0, 1, 1, 1920, 0.99)).toBeNull();

    // Non-finite inputs
    const validContain = computeContainRect(800, 600, 1920, 1080)!;
    expect(clampPointToContainContent(Number.NaN, Number.POSITIVE_INFINITY, validContain)).toEqual({
      x: validContain.offsetX,
      y: validContain.offsetY,
    });
    expect(scaleSourceBox({ x: 0, y: 0, width: 100, height: 50 }, Number.NaN, 1080, 1280, 720)).toBeNull();
    expect(createDefaultBandBox(Number.POSITIVE_INFINITY, 1080, 0.3)).toBeNull();
    expect(toBandSourceBox({ x: 0, y: 500, width: 1920, height: Number.NaN }, 1920, 1080)).toBeNull();
  });
});
