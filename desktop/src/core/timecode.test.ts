import { describe, expect, it } from 'vitest';
import {
  clampMs,
  clampMsToRange,
  computeCueMidpointMs,
  formatCompactDuration,
  formatHudTimecode,
  formatSrtTimecode,
  parseSrtTimecode,
  validateTimeRange,
} from './timecode';

describe('core/timecode', () => {
  it('clamps non-finite and negative milliseconds to 0', () => {
    expect(clampMs(-500)).toBe(0);
    expect(clampMs(Number.NaN)).toBe(0);
    expect(clampMs(Number.POSITIVE_INFINITY)).toBe(0);
    expect(clampMs(Number.NEGATIVE_INFINITY)).toBe(0);
    expect(clampMs(123.6)).toBe(124);
  });

  it('clamps to upper bound range safely', () => {
    expect(clampMsToRange(5000, 3000)).toBe(3000);
    expect(clampMsToRange(-50, 3000)).toBe(0);
    expect(clampMsToRange(1200, undefined)).toBe(1200);
  });

  it('formats SRT and HUD timecodes identically to C++ format_timestamp_srt', () => {
    expect(formatSrtTimecode(0)).toBe('00:00:00,000');
    expect(formatSrtTimecode(-500)).toBe('00:00:00,000');
    expect(formatSrtTimecode(3_661_005)).toBe('01:01:01,005');
    expect(formatSrtTimecode(360_000_000)).toBe('100:00:00,000');
    expect(formatHudTimecode(3_661_005)).toBe('01:01:01.005');
    expect(formatCompactDuration(65_000)).toBe('01:05');
    expect(formatCompactDuration(3_661_000)).toBe('01:01:01');
  });

  it('parses valid SRT/HUD timecodes and rejects malformed inputs', () => {
    expect(parseSrtTimecode(' 01:01:01,005 ')).toBe(3_661_005);
    expect(parseSrtTimecode('01:01:01.005')).toBe(3_661_005);
    expect(parseSrtTimecode('01:01:01.005', { allowDot: false })).toBeNull();
    expect(parseSrtTimecode('100:00:00,000')).toBe(360_000_000);

    expect(parseSrtTimecode('00:60:00,000')).toBeNull();
    expect(parseSrtTimecode('00:00:60,000')).toBeNull();
    expect(parseSrtTimecode('00:00:01,5')).toBeNull();
    expect(parseSrtTimecode('-00:00:01,000')).toBeNull();
    expect(parseSrtTimecode('abc')).toBeNull();
  });

  it('validates start_ms < end_ms and computes integer midpoint', () => {
    expect(validateTimeRange(100, 200).valid).toBe(true);
    expect(validateTimeRange(200, 200).valid).toBe(false);
    expect(validateTimeRange(300, 200).reason).toContain('-100ms');
    expect(validateTimeRange(-1, 200).valid).toBe(false);
    expect(validateTimeRange(100.5, 200).valid).toBe(false);

    expect(computeCueMidpointMs(1000, 2001)).toBe(1500);
    expect(computeCueMidpointMs(-100, 400)).toBe(200);
  });

  it('guards MAX_SAFE_INTEGER overflow, 999h round-trip, -0 normalization, and NaN/Infinity ranges', () => {
    expect(parseSrtTimecode('99999999999999:00:00,000')).toBeNull();
    const max999 = parseSrtTimecode('999:59:59,999');
    expect(max999).toBe(3_599_999_999);
    expect(formatSrtTimecode(max999!)).toBe('999:59:59,999');

    expect(Object.is(clampMs(-0.4), 0)).toBe(true);
    expect(validateTimeRange(Number.NaN, 1000).valid).toBe(false);
    expect(validateTimeRange(0, Number.POSITIVE_INFINITY).valid).toBe(false);
  });
});
