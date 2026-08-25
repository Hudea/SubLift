import { describe, it, expect } from 'vitest';
import {
  formatTimecode,
  formatSrtTimestamp,
  formatDisplayMs,
  parseFlexibleTimecode,
  validateTimecodeRange,
  validateTimecodeString,
  detectTimelineConflicts,
} from './timecode';

describe('timecode utils', () => {
  describe('formatTimecode', () => {
    it('formats positive seconds into HH:MM:SS.mmm', () => {
      expect(formatTimecode(0)).toBe('00:00:00.000');
      expect(formatTimecode(65.123)).toBe('00:01:05.123');
      expect(formatTimecode(3665.456)).toBe('01:01:05.456');
    });

    it('handles negative or invalid values gracefully', () => {
      expect(formatTimecode(-5)).toBe('00:00:00.000');
      expect(formatTimecode(NaN)).toBe('00:00:00.000');
      expect(formatTimecode(Infinity)).toBe('00:00:00.000');
    });
  });

  describe('formatSrtTimestamp', () => {
    it('formats milliseconds into HH:MM:SS,mmm', () => {
      expect(formatSrtTimestamp(0)).toBe('00:00:00,000');
      expect(formatSrtTimestamp(65123)).toBe('00:01:05,123');
      expect(formatSrtTimestamp(3665456)).toBe('01:01:05,456');
    });
  });

  describe('formatDisplayMs', () => {
    it('formats milliseconds into MM:SS.mmm', () => {
      expect(formatDisplayMs(1234)).toBe('00:01.234');
      expect(formatDisplayMs(65123)).toBe('01:05.123');
    });
  });

  describe('parseFlexibleTimecode', () => {
    it('parses standard HH:MM:SS.mmm and MM:SS.mmm and seconds', () => {
      expect(parseFlexibleTimecode('01:02:03.456')).toBe(3723456);
      expect(parseFlexibleTimecode('01:02:03,456')).toBe(3723456);
      expect(parseFlexibleTimecode('02:03.500')).toBe(123500);
      expect(parseFlexibleTimecode('10.5')).toBe(10500);
      expect(parseFlexibleTimecode('5')).toBe(5000);
    });

    it('returns null for invalid inputs', () => {
      expect(parseFlexibleTimecode('')).toBeNull();
      expect(parseFlexibleTimecode('abc')).toBeNull();
      expect(parseFlexibleTimecode('01:99:99')).toBeNull();
    });
  });

  describe('validateTimecodeRange', () => {
    it('validates start < end', () => {
      expect(validateTimecodeRange(1000, 2000).valid).toBe(true);
      expect(validateTimecodeRange(2000, 1000).valid).toBe(false);
      expect(validateTimecodeRange(1000, 1000).valid).toBe(false);
      expect(validateTimecodeRange(-10, 100).valid).toBe(false);
    });
  });

  describe('validateTimecodeString', () => {
    it('validates timecode range strings', () => {
      const res1 = validateTimecodeString('00:01.000 - 00:03.500');
      expect(res1.valid).toBe(true);
      expect(res1.start_ms).toBe(1000);
      expect(res1.end_ms).toBe(3500);

      const res2 = validateTimecodeString('00:01.000 --> 00:03.500');
      expect(res2.valid).toBe(true);
      expect(res2.start_ms).toBe(1000);
      expect(res2.end_ms).toBe(3500);
    });

    it('rejects inverted or equal timecodes without silent discard', () => {
      const res = validateTimecodeString('00:05.000 - 00:02.000');
      expect(res.valid).toBe(false);
      expect(res.error).toContain('必须早于结束时间');
      expect(res.start_ms).toBe(5000);
      expect(res.end_ms).toBe(2000);
    });

    it('rejects invalid format strings', () => {
      const res = validateTimecodeString('not a valid time');
      expect(res.valid).toBe(false);
      expect(res.error).toContain('缺少分隔符');
    });
  });

  describe('detectTimelineConflicts', () => {
    it('identifies overlaps and invalid durations', () => {
      const entries = [
        { index: 1, start_ms: 1000, end_ms: 3000, text: 'First' },
        { index: 2, start_ms: 2500, end_ms: 4000, text: 'Overlaps with 1' },
        { index: 3, start_ms: 5000, end_ms: 4500, text: 'Invalid duration' },
      ];

      const conflicts = detectTimelineConflicts(entries);
      expect(conflicts.has(1)).toBe(true);
      expect(conflicts.get(1)?.some((w) => w.type === 'overlap_next')).toBe(true);

      expect(conflicts.has(2)).toBe(true);
      expect(conflicts.get(2)?.some((w) => w.type === 'overlap_prev')).toBe(true);

      expect(conflicts.has(3)).toBe(true);
      expect(conflicts.get(3)?.some((w) => w.type === 'invalid_duration')).toBe(true);
    });

    it('returns empty map for clean non-overlapping entries', () => {
      const entries = [
        { index: 1, start_ms: 1000, end_ms: 2000, text: 'First' },
        { index: 2, start_ms: 2500, end_ms: 3500, text: 'Second' },
      ];
      const conflicts = detectTimelineConflicts(entries);
      expect(conflicts.size).toBe(0);
    });
  });
});
