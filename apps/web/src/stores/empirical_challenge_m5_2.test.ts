import { describe, it, expect, beforeEach, vi, afterEach } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import { useWorkbenchStore } from './workbench';
import { useSystemStore } from './system';
import {
  formatSrtTimestamp,
  parseFlexibleTimecode,
  validateTimecodeRange,
  validateTimecodeString,
  detectTimelineConflicts,
} from '../utils/timecode';
import {
  formatSrt,
  parseSrt,
  exportBatchZip,
  type BatchSrtItem,
} from '../utils/srt_formatter';
import type { SubtitleEntry } from '../types/api';

describe('Empirical Challenge 2 - Phase 12 Milestone 5 (Feature 12515: 字幕审阅草稿与无损校对)', () => {
  let localStorageMock: Record<string, string> = {};

  beforeEach(() => {
    setActivePinia(createPinia());
    localStorageMock = {};

    vi.stubGlobal('localStorage', {
      getItem: (key: string) => localStorageMock[key] ?? null,
      setItem: (key: string, val: string) => {
        localStorageMock[key] = String(val);
      },
      removeItem: (key: string) => {
        delete localStorageMock[key];
      },
      clear: () => {
        localStorageMock = {};
      },
    });

    const systemStore = useSystemStore();
    systemStore.systemInfo = {
      version: 'v0.1.0-challenge2',
      runtime: 'cpp',
      capabilities: ['mock', 'vision'],
      engines: [{ name: 'vision', available: true, detail: 'Apple Vision' }],
      ffmpeg: { available: true, path: '/usr/bin/ffmpeg' },
    };
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  describe('1. Timecode Parser & String Validation Edge Cases', () => {
    it('rejects malformed strings and invalid inputs with null or structured error', () => {
      // Malformed strings
      expect(parseFlexibleTimecode('abc')).toBeNull();
      expect(parseFlexibleTimecode('xyz:12:34')).toBeNull();
      expect(parseFlexibleTimecode('')).toBeNull();
      expect(parseFlexibleTimecode('   ')).toBeNull();
      expect(parseFlexibleTimecode(undefined as unknown as string)).toBeNull();
      expect(parseFlexibleTimecode(null as unknown as string)).toBeNull();
      expect(parseFlexibleTimecode('NaN')).toBeNull();
      expect(parseFlexibleTimecode('Infinity')).toBeNull();
      expect(parseFlexibleTimecode('-Infinity')).toBeNull();

      // Negative values
      expect(parseFlexibleTimecode('-5')).toBeNull();
      expect(parseFlexibleTimecode('-01:00.000')).toBeNull();
      expect(parseFlexibleTimecode('00:-05.000')).toBeNull();
      expect(parseFlexibleTimecode('-01:00:00.000')).toBeNull();

      // Invalid field values (out of 60s/60m bounds)
      expect(parseFlexibleTimecode('00:99:99,999')).toBeNull();
      expect(parseFlexibleTimecode('00:60:00.000')).toBeNull();
      expect(parseFlexibleTimecode('00:00:60.000')).toBeNull();
      expect(parseFlexibleTimecode('00:60.000')).toBeNull();

      // Valid boundary values
      expect(parseFlexibleTimecode('00:59:59.999')).toBe(3599999);
      expect(parseFlexibleTimecode('59:59.999')).toBe(3599999);
      expect(parseFlexibleTimecode('00:00:00.000')).toBe(0);
      expect(parseFlexibleTimecode('0')).toBe(0);

      // Large hour count (for long videos / continuous recordings)
      expect(parseFlexibleTimecode('100:00:00.000')).toBe(360000000);
    });

    it('validates timecode string ranges with multiple supported separators', () => {
      const separators = [' - ', ' --> ', ' -> ', ' ~ ', ' 至 '];

      for (const sep of separators) {
        const input = `00:01.000${sep}00:03.500`;
        const res = validateTimecodeString(input);
        expect(res.valid).toBe(true);
        expect(res.start_ms).toBe(1000);
        expect(res.end_ms).toBe(3500);
        expect(res.error).toBeUndefined();
      }
    });

    it('rejects timecode string with missing, unsupported or multiple separators', () => {
      // Missing separator
      const resNoSep = validateTimecodeString('00:01.000 00:03.500');
      expect(resNoSep.valid).toBe(false);
      expect(resNoSep.error).toContain('缺少分隔符');

      // Unsupported separator
      const resBadSep = validateTimecodeString('00:01.000 / 00:03.500');
      expect(resBadSep.valid).toBe(false);
      expect(resBadSep.error).toContain('缺少分隔符');

      // Multiple separators / too many segments
      const resMultiSep = validateTimecodeString('00:01.000 - 00:02.000 - 00:03.000');
      expect(resMultiSep.valid).toBe(false);
      expect(resMultiSep.error).toContain('必须包含开始和结束两个时间戳');
    });

    it('rejects timecode string when either start or end time is malformed', () => {
      const resBadStart = validateTimecodeString('bad_time - 00:03.500');
      expect(resBadStart.valid).toBe(false);
      expect(resBadStart.error).toContain('开始时间码格式错误');

      const resBadEnd = validateTimecodeString('00:01.000 - bad_time');
      expect(resBadEnd.valid).toBe(false);
      expect(resBadEnd.error).toContain('结束时间码格式错误');
    });
  });

  describe('2. Inverted Timestamps, Zero-Duration & 1-Millisecond Entries', () => {
    it('validates inverted timestamps (start > end)', () => {
      // 1. validateTimecodeRange
      const rangeRes = validateTimecodeRange(5000, 2000);
      expect(rangeRes.valid).toBe(false);
      expect(rangeRes.error).toContain('必须早于结束时间');

      // 2. validateTimecodeString
      const strRes = validateTimecodeString('00:00:05,000 --> 00:00:02,000');
      expect(strRes.valid).toBe(false);
      expect(strRes.start_ms).toBe(5000);
      expect(strRes.end_ms).toBe(2000);
      expect(strRes.error).toContain('必须早于结束时间');

      // 3. detectTimelineConflicts
      const entries = [
        { index: 1, start_ms: 5000, end_ms: 2000, text: 'Inverted' },
      ];
      const conflicts = detectTimelineConflicts(entries);
      expect(conflicts.has(1)).toBe(true);
      const warnings = conflicts.get(1)!;
      expect(warnings.some((w) => w.type === 'invalid_duration')).toBe(true);
      expect(warnings[0].message).toContain('时长无效');
    });

    it('validates zero-duration entries (start === end)', () => {
      // 1. validateTimecodeRange
      const rangeRes = validateTimecodeRange(3000, 3000);
      expect(rangeRes.valid).toBe(false);
      expect(rangeRes.error).toContain('必须早于结束时间');

      // 2. validateTimecodeString
      const strRes = validateTimecodeString('00:03.000 - 00:03.000');
      expect(strRes.valid).toBe(false);
      expect(strRes.start_ms).toBe(3000);
      expect(strRes.end_ms).toBe(3000);
      expect(strRes.error).toContain('必须早于结束时间');

      // 3. detectTimelineConflicts
      const entries = [
        { index: 1, start_ms: 3000, end_ms: 3000, text: 'Zero duration' },
      ];
      const conflicts = detectTimelineConflicts(entries);
      expect(conflicts.has(1)).toBe(true);
      expect(conflicts.get(1)![0].type).toBe('invalid_duration');
    });

    it('accepts 1-millisecond duration entries (start + 1 === end)', () => {
      // 1. validateTimecodeRange
      const rangeRes = validateTimecodeRange(3000, 3001);
      expect(rangeRes.valid).toBe(true);
      expect(rangeRes.error).toBeUndefined();

      // 2. validateTimecodeString
      const strRes = validateTimecodeString('00:03.000 - 00:03.001');
      expect(strRes.valid).toBe(true);
      expect(strRes.start_ms).toBe(3000);
      expect(strRes.end_ms).toBe(3001);

      // 3. detectTimelineConflicts
      const entries = [
        { index: 1, start_ms: 3000, end_ms: 3001, text: '1ms entry' },
      ];
      const conflicts = detectTimelineConflicts(entries);
      expect(conflicts.size).toBe(0);

      // 4. formatSrtTimestamp
      expect(formatSrtTimestamp(3000)).toBe('00:00:03,000');
      expect(formatSrtTimestamp(3001)).toBe('00:00:03,001');
    });
  });

  describe('3. Timeline Collision Warnings & Overlap Detection Oracles', () => {
    it('detects overlapping adjacent entries (Entry 1 end=10s, Entry 2 start=8s)', () => {
      const entries = [
        { index: 101, start_ms: 0, end_ms: 10000, text: 'Entry 1 (0-10s)' },
        { index: 102, start_ms: 8000, end_ms: 15000, text: 'Entry 2 (8-15s)' },
      ];

      const conflicts = detectTimelineConflicts(entries);

      // Entry 101 should have overlap_next warning
      expect(conflicts.has(101)).toBe(true);
      const w101 = conflicts.get(101)!;
      expect(w101.some((w) => w.type === 'overlap_next')).toBe(true);
      expect(w101[0].message).toContain('与后一条 (#102) 时间重叠');

      // Entry 102 should have overlap_prev warning
      expect(conflicts.has(102)).toBe(true);
      const w102 = conflicts.get(102)!;
      expect(w102.some((w) => w.type === 'overlap_prev')).toBe(true);
      expect(w102[0].message).toContain('与前一条 (#101) 时间重叠');
    });

    it('does NOT warn on clean boundary touching entries (Entry 1 end=10000, Entry 2 start=10000)', () => {
      const entries = [
        { index: 1, start_ms: 5000, end_ms: 10000, text: 'Entry 1' },
        { index: 2, start_ms: 10000, end_ms: 15000, text: 'Entry 2' },
        { index: 3, start_ms: 15000, end_ms: 20000, text: 'Entry 3' },
      ];

      const conflicts = detectTimelineConflicts(entries);
      expect(conflicts.size).toBe(0);
    });

    it('handles out-of-order array input correctly and maps warnings to original indices', () => {
      // Unsorted array: Entry 3 (start 8s), Entry 1 (start 1s), Entry 2 (start 4s)
      const entries = [
        { index: 3, start_ms: 8000, end_ms: 12000, text: 'Third (8-12s)' },
        { index: 1, start_ms: 1000, end_ms: 5000, text: 'First (1-5s)' },
        { index: 2, start_ms: 4000, end_ms: 9000, text: 'Second (4-9s, overlaps with 1 & 3)' },
      ];

      const conflicts = detectTimelineConflicts(entries);

      // Entry 1 overlaps with Entry 2 (5000 > 4000)
      expect(conflicts.has(1)).toBe(true);
      expect(conflicts.get(1)?.some((w) => w.type === 'overlap_next')).toBe(true);

      // Entry 2 overlaps with Entry 1 (prev) and Entry 3 (next: 9000 > 8000)
      expect(conflicts.has(2)).toBe(true);
      const w2 = conflicts.get(2)!;
      expect(w2.some((w) => w.type === 'overlap_prev')).toBe(true);
      expect(w2.some((w) => w.type === 'overlap_next')).toBe(true);

      // Entry 3 overlaps with Entry 2 (prev)
      expect(conflicts.has(3)).toBe(true);
      expect(conflicts.get(3)?.some((w) => w.type === 'overlap_prev')).toBe(true);
    });

    it('detects complete containment collision (Entry A covers Entry B completely)', () => {
      const entries = [
        { index: 1, start_ms: 1000, end_ms: 10000, text: 'Outer wrapper (1-10s)' },
        { index: 2, start_ms: 3000, end_ms: 6000, text: 'Inner contained (3-6s)' },
      ];

      const conflicts = detectTimelineConflicts(entries);
      expect(conflicts.has(1)).toBe(true);
      expect(conflicts.get(1)?.some((w) => w.type === 'overlap_next')).toBe(true);

      expect(conflicts.has(2)).toBe(true);
      expect(conflicts.get(2)?.some((w) => w.type === 'overlap_prev')).toBe(true);
    });

    it('handles multi-way cascade overlaps gracefully', () => {
      const entries = [
        { index: 1, start_ms: 1000, end_ms: 4000, text: 'Line 1' },
        { index: 2, start_ms: 2000, end_ms: 5000, text: 'Line 2' },
        { index: 3, start_ms: 3000, end_ms: 6000, text: 'Line 3' },
        { index: 4, start_ms: 7000, end_ms: 8000, text: 'Line 4 (Isolated)' },
      ];

      const conflicts = detectTimelineConflicts(entries);
      expect(conflicts.has(1)).toBe(true);
      expect(conflicts.has(2)).toBe(true);
      expect(conflicts.has(3)).toBe(true);
      expect(conflicts.has(4)).toBe(false); // Entry 4 is clean
    });
  });

  describe('4. Export Fidelity & Round-Trip SRT Integrity', () => {
    it('accurately exports user-edited draft timestamps and texts into standard UTF-8 SRT', () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/test_export_fidelity.mp4');

      store.entries = [
        { index: 1, start_ms: 1250, end_ms: 4750, text: '第一行字幕：支持中文与标点！', confidence: 0.99 },
        { index: 2, start_ms: 5000, end_ms: 8200, text: 'Second line with English & symbols: <test> "quotes" & more', confidence: 0.92 },
        { index: 3, start_ms: 8500, end_ms: 11000, text: '第三行字幕 🚀 含 Emoji 符号', confidence: 0.88 },
      ];

      const exportedSrt = formatSrt(store.entries);

      expect(exportedSrt).toBe(
        '1\n00:00:01,250 --> 00:00:04,750\n第一行字幕：支持中文与标点！\n\n' +
        '2\n00:00:05,000 --> 00:00:08,200\nSecond line with English & symbols: <test> "quotes" & more\n\n' +
        '3\n00:00:08,500 --> 00:00:11,000\n第三行字幕 🚀 含 Emoji 符号\n'
      );
    });

    it('ensures exported SRT is sorted monotonically by start_ms regardless of store entry array order', () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/test_unsorted_export.mp4');

      // Add entries in reverse order
      store.entries = [
        { index: 3, start_ms: 15000, end_ms: 18000, text: 'Third (15s)', confidence: 0.9 },
        { index: 1, start_ms: 1000, end_ms: 4000, text: 'First (1s)', confidence: 0.95 },
        { index: 2, start_ms: 6000, end_ms: 9000, text: 'Second (6s)', confidence: 0.85 },
      ];

      const exportedSrt = formatSrt(store.entries);

      // Indices in output SRT MUST be 1, 2, 3 in ascending time order
      const parsed = parseSrt(exportedSrt);
      expect(parsed.length).toBe(3);
      expect(parsed[0].start_ms).toBe(1000);
      expect(parsed[0].text).toBe('First (1s)');
      expect(parsed[1].start_ms).toBe(6000);
      expect(parsed[1].text).toBe('Second (6s)');
      expect(parsed[2].start_ms).toBe(15000);
      expect(parsed[2].text).toBe('Third (15s)');
    });

    it('performs lossless round-trip parse/format on complex multiline entries', () => {
      const originalEntries: SubtitleEntry[] = [
        { index: 1, start_ms: 0, end_ms: 3500, text: 'Line 1\nSecond Line of Entry 1', confidence: 1.0 },
        { index: 2, start_ms: 4000, end_ms: 7250, text: 'Entry 2 Simple', confidence: 1.0 },
        { index: 3, start_ms: 8000, end_ms: 12000, text: 'Line A\nLine B\nLine C', confidence: 1.0 },
      ];

      const formatted = formatSrt(originalEntries);
      const parsed = parseSrt(formatted);

      expect(parsed.length).toBe(3);
      expect(parsed[0].start_ms).toBe(0);
      expect(parsed[0].end_ms).toBe(3500);
      expect(parsed[0].text).toBe('Line 1\nSecond Line of Entry 1');

      expect(parsed[1].start_ms).toBe(4000);
      expect(parsed[1].end_ms).toBe(7250);
      expect(parsed[1].text).toBe('Entry 2 Simple');

      expect(parsed[2].start_ms).toBe(8000);
      expect(parsed[2].end_ms).toBe(12000);
      expect(parsed[2].text).toBe('Line A\nLine B\nLine C');
    });

    it('reflects active Undo and Redo draft states in exported SRT', () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/undo_export.mp4');

      store.entries = [
        { index: 1, start_ms: 1000, end_ms: 3000, text: 'Initial Text', confidence: 1.0 },
      ];

      // Initial export
      expect(formatSrt(store.entries)).toContain('Initial Text');

      // Edit 1
      store.updateSubtitleEntry(1, { text: 'Edited Text v1' });
      expect(formatSrt(store.entries)).toContain('Edited Text v1');

      // Edit 2
      store.updateSubtitleEntry(1, { text: 'Edited Text v2' });
      expect(formatSrt(store.entries)).toContain('Edited Text v2');

      // Undo -> v1
      store.undo();
      expect(formatSrt(store.entries)).toContain('Edited Text v1');

      // Undo -> Initial
      store.undo();
      expect(formatSrt(store.entries)).toContain('Initial Text');

      // Redo -> v1
      store.redo();
      expect(formatSrt(store.entries)).toContain('Edited Text v1');

      // Redo -> v2
      store.redo();
      expect(formatSrt(store.entries)).toContain('Edited Text v2');
    });

    it('verifies exportBatchZip formats and bundles all tasks accurately into single ZIP', () => {
      const items: BatchSrtItem[] = [
        {
          name: 'video_01.mp4',
          entries: [
            { index: 1, start_ms: 1000, end_ms: 3000, text: 'V1 Subtitle 1', confidence: 0.9 },
            { index: 2, start_ms: 4000, end_ms: 6000, text: 'V1 Subtitle 2', confidence: 0.85 },
          ],
        },
        {
          name: 'video_02.mkv',
          entries: [
            { index: 1, start_ms: 500, end_ms: 2500, text: 'V2 Only Subtitle', confidence: 0.95 },
          ],
        },
      ];

      // Mock DOM download functions
      let downloadedBlob: Blob | null = null;
      let clicked = false;
      let appendedNode: any = null;

      const originalDocument = (globalThis as any).document;
      const originalUrl = (globalThis as any).URL;

      (globalThis as any).URL = {
        createObjectURL: vi.fn((blob: Blob) => {
          downloadedBlob = blob;
          return 'blob:mock-url';
        }),
        revokeObjectURL: vi.fn(),
      };

      const mockAnchor = {
        href: '',
        download: '',
        click: vi.fn(() => {
          clicked = true;
        }),
      };

      (globalThis as any).document = {
        createElement: vi.fn((tagName: string) => {
          if (tagName === 'a') return mockAnchor;
          return {};
        }),
        body: {
          appendChild: vi.fn((node: any) => {
            appendedNode = node;
            return node;
          }),
          removeChild: vi.fn((node: any) => node),
        },
      };

      const success = exportBatchZip(items, 'all_subtitles.zip');
      expect(success).toBe(true);
      expect(downloadedBlob).not.toBeNull();
      expect((downloadedBlob as any)?.type).toBe('application/zip');
      expect(clicked).toBe(true);
      expect(appendedNode?.download).toBe('all_subtitles.zip');

      (globalThis as any).document = originalDocument;
      (globalThis as any).URL = originalUrl;
    });

    it('handles empty entries gracefully without throwing', () => {
      expect(formatSrt([])).toBe('');
      expect(parseSrt('')).toEqual([]);
      expect(exportBatchZip([])).toBe(false);
      expect(exportBatchZip([{ name: 'empty.mp4', entries: [] }])).toBe(false);
    });
  });
});
