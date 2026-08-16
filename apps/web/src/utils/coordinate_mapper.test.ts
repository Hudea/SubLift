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

function assert(condition: boolean, msg: string) {
  if (!condition) {
    throw new Error(`Assertion failed: ${msg}`);
  }
}

function runTests() {
  console.log('--- Testing calculateVideoViewport (Letterbox / Pillarbox) ---');

  // 1. 16:9 video in 4:3 container -> Letterbox (Top/Bottom black bars)
  const lb = calculateVideoViewport(800, 600, { videoWidth: 1920, videoHeight: 1080, duration: 10, aspectRatio: 16 / 9 });
  assert(lb.x === 0, `lb.x should be 0, got ${lb.x}`);
  assert(lb.y === 75, `lb.y should be 75, got ${lb.y}`);
  assert(lb.width === 800, `lb.width should be 800, got ${lb.width}`);
  assert(lb.height === 450, `lb.height should be 450, got ${lb.height}`);
  console.log('✓ Letterbox calculation verified');

  // 2. 9:16 vertical video in 16:9 container -> Pillarbox (Left/Right black bars)
  const pb = calculateVideoViewport(1280, 720, { videoWidth: 1080, videoHeight: 1920, duration: 10, aspectRatio: 9 / 16 });
  assert(pb.x === 437.5, `pb.x should be 437.5, got ${pb.x}`);
  assert(pb.y === 0, `pb.y should be 0, got ${pb.y}`);
  assert(pb.width === 405, `pb.width should be 405, got ${pb.width}`);
  assert(pb.height === 720, `pb.height should be 720, got ${pb.height}`);
  console.log('✓ Pillarbox calculation verified');

  // 4. 21:9 Ultrawide Video in 16:9 container -> Letterbox
  const uw = calculateVideoViewport(1920, 1080, { videoWidth: 2560, videoHeight: 1080, duration: 10, aspectRatio: 2560 / 1080 });
  assert(uw.x === 0, 'Ultrawide x should be 0');
  assert(uw.y > 0, 'Ultrawide should have top/bottom letterbox');
  assert(uw.width === 1920, 'Ultrawide width should be container width');
  console.log('✓ 21:9 Ultrawide letterbox verified');

  // 5. Zero / degraded dimensions
  const zeroVp = calculateVideoViewport(0, 0, { videoWidth: 0, videoHeight: 0, duration: 0, aspectRatio: 16 / 9 });
  assert(zeroVp.width === 0 && zeroVp.height === 0, 'Zero dimensions should degrade safely');
  console.log('✓ Zero viewport degradation verified');

  console.log('--- Testing Normalized <-> Canvas Coordinate Conversion ---');
  const viewport = { x: 0, y: 50, width: 800, height: 450 };
  const normBox = { x: 0.1, y: 0.7, width: 0.8, height: 0.25 };

  const canvasBox = normalizedToCanvas(normBox, viewport);
  assert(canvasBox.x === 80, `canvasBox.x should be 80, got ${canvasBox.x}`);
  assert(canvasBox.y === 50 + 0.7 * 450, `canvasBox.y should be 365, got ${canvasBox.y}`);
  assert(canvasBox.width === 640, `canvasBox.width should be 640, got ${canvasBox.width}`);
  assert(canvasBox.height === 112.5, `canvasBox.height should be 112.5, got ${canvasBox.height}`);

  const roundTrip = canvasToNormalized(canvasBox, viewport);
  assert(Math.abs(roundTrip.x - normBox.x) < 0.001, 'Round-trip x mismatch');
  assert(Math.abs(roundTrip.y - normBox.y) < 0.001, 'Round-trip y mismatch');
  assert(Math.abs(roundTrip.width - normBox.width) < 0.001, 'Round-trip width mismatch');
  assert(Math.abs(roundTrip.height - normBox.height) < 0.001, 'Round-trip height mismatch');

  const normPoint = screenToNormalizedPoint({ x: canvasBox.x, y: canvasBox.y }, viewport);
  assert(Math.abs(normPoint.x - normBox.x) < 0.001, 'screenToNormalizedPoint x mismatch');
  assert(Math.abs(normPoint.y - normBox.y) < 0.001, 'screenToNormalizedPoint y mismatch');
  console.log('✓ Round-trip coordinate conversion & point mapping verified');

  console.log('--- Testing Extreme Bounding Box & Clamping Safety ---');
  // Test extreme right-bottom dragging
  const extremeBox = canvasToNormalized({ x: 9999, y: 9999, width: 500, height: 500 }, viewport);
  assert(extremeBox.x <= 0.99, 'Extreme x must not exceed 0.99');
  assert(extremeBox.y <= 0.99, 'Extreme y must not exceed 0.99');
  assert(Number((extremeBox.x + extremeBox.width).toFixed(4)) <= 1.0, `x + width must be <= 1.0, got ${extremeBox.x + extremeBox.width}`);
  assert(Number((extremeBox.y + extremeBox.height).toFixed(4)) <= 1.0, `y + height must be <= 1.0, got ${extremeBox.y + extremeBox.height}`);
  console.log('✓ Extreme clamp safety (x+w<=1.0, y+h<=1.0) verified');

  console.log('--- Testing 8 Handle Geometries & Hit Testing ---');
  const handles = getHandleGeometries(canvasBox);
  assert(handles.length === 8, `Expected 8 handles, got ${handles.length}`);

  // Test hitting all 8 handles
  for (const h of handles) {
    const hit = hitTestHandle(h.x, h.y, handles, 8);
    assert(hit !== null && hit.type === h.type, `Failed to hit handle ${h.type}`);
  }

  // Test miss outside
  const miss = hitTestHandle(0, 0, handles, 8);
  assert(miss === null, 'Hit test should miss at (0, 0)');
  console.log('✓ All 8 Handle geometries & hit testing verified');

  console.log('--- Testing Inside Box & Viewport ---');
  assert(isPointInsideBox(canvasBox.x + 10, canvasBox.y + 10, canvasBox), 'Point should be inside box');
  assert(!isPointInsideBox(0, 0, canvasBox), 'Point (0, 0) should be outside box');
  assert(isPointInsideViewport(400, 200, viewport), 'Point should be inside viewport');
  assert(!isPointInsideViewport(400, 10, viewport), 'Point should be in letterbox, outside viewport');
  console.log('✓ Point inside checks verified');

  console.log('All coordinate_mapper tests passed successfully!');
}

runTests();
