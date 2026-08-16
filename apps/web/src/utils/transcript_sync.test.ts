import { describe, it, expect } from 'vitest';
import { SubtitleSearcher } from './subtitle_search';
import {
  formatSrt,
  parseSrt,
  parseFlexibleTimecodeToMs,
  parseSrtTimestampToMs,
} from './srt_formatter';
import type { SubtitleEntry } from '../types/api';

describe('transcript_sync & srt_formatter', () => {
  it('searches active subtitle entries using binary search and monotonic cache', () => {
    const searcher = new SubtitleSearcher();

    const mockEntries: SubtitleEntry[] = [
      { index: 1, start_ms: 1000, end_ms: 3000, text: '第一句字幕', confidence: 0.95 },
      { index: 2, start_ms: 3500, end_ms: 5000, text: '第二句字幕', confidence: 0.92 },
      { index: 3, start_ms: 6000, end_ms: 8000, text: '第三句字幕', confidence: 0.88 },
      { index: 4, start_ms: 8200, end_ms: 10000, text: '第四句字幕', confidence: 0.99 },
    ];

    // 1. 空列表与负时间
    expect(searcher.findActiveIndex([], 2000)).toBeNull();
    expect(searcher.findActiveIndex(mockEntries, -100)).toBeNull();

    // 2. 外部极值
    expect(searcher.findActiveIndex(mockEntries, 500)).toBeNull();
    expect(searcher.findActiveIndex(mockEntries, 15000)).toBeNull();

    // 3. 精确命中边界
    expect(searcher.findActiveIndex(mockEntries, 1000)).toBe(1);
    expect(searcher.findActiveIndex(mockEntries, 2000)).toBe(1);
    expect(searcher.findActiveIndex(mockEntries, 3000)).toBe(1);

    // 4. Gap 间隙
    expect(searcher.findActiveIndex(mockEntries, 3200)).toBeNull();

    // 5. 连续播放与缓存命中
    expect(searcher.findActiveIndex(mockEntries, 3600)).toBe(2);
    expect(searcher.findActiveIndex(mockEntries, 3800)).toBe(2);
    expect(searcher.findActiveIndex(mockEntries, 6100)).toBe(3);

    // 6. Seek 倒退与跳跃
    expect(searcher.findActiveIndex(mockEntries, 9000)).toBe(4);
    expect(searcher.findActiveIndex(mockEntries, 1500)).toBe(1);

    // 7. 缓存重置
    searcher.resetCache();
    expect(searcher.findActiveIndex(mockEntries, 1500)).toBe(1);
  });

  it('parses flexible timecodes across formats', () => {
    // Full standard timestamp
    expect(parseFlexibleTimecodeToMs('01:02:03,456')).toBe(3723456);
    expect(parseFlexibleTimecodeToMs('01:02:03.456')).toBe(3723456);
    expect(parseSrtTimestampToMs('01:02:03,456')).toBe(3723456);

    // Short display format MM:SS.d or MM:SS.mmm
    expect(parseFlexibleTimecodeToMs('00:01.2')).toBe(1200);
    expect(parseFlexibleTimecodeToMs('02:30.500')).toBe(150500);
    expect(parseFlexibleTimecodeToMs('02:30,500')).toBe(150500);
    expect(parseFlexibleTimecodeToMs('01:15')).toBe(75000);

    // Pure seconds
    expect(parseFlexibleTimecodeToMs('12.5')).toBe(12500);
    expect(parseFlexibleTimecodeToMs('45')).toBe(45000);

    // Fallback / Invalid
    expect(parseFlexibleTimecodeToMs('')).toBe(0);
    expect(parseFlexibleTimecodeToMs('invalid')).toBe(0);
  });

  it('formats SRT with auto-sorting and supports multi-line round-trip parsing', () => {
    const unorderedEntries: SubtitleEntry[] = [
      { index: 99, start_ms: 5000, end_ms: 7000, text: '第二句', confidence: 0.9 },
      { index: 12, start_ms: 1000, end_ms: 3000, text: '第一句\n含换行', confidence: 0.95 },
    ];

    const srtOutput = formatSrt(unorderedEntries);
    expect(
      srtOutput.startsWith('1\n00:00:01,000 --> 00:00:03,000\n第一句\n含换行\n\n2\n00:00:05,000 --> 00:00:07,000\n第二句')
    ).toBe(true);

    const parsed = parseSrt(srtOutput);
    expect(parsed.length).toBe(2);
    expect(parsed[0].text).toBe('第一句\n含换行');
    expect(parsed[0].start_ms).toBe(1000);
    expect(parsed[0].end_ms).toBe(3000);
    expect(parsed[1].text).toBe('第二句');
  });
});
