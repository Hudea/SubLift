import { SubtitleSearcher } from './subtitle_search';
import {
  formatSrt,
  parseSrt,
  parseFlexibleTimecodeToMs,
  parseSrtTimestampToMs,
} from './srt_formatter';
import type { SubtitleEntry } from '../types/api';

function assert(condition: boolean, msg: string) {
  if (!condition) {
    throw new Error(`Assertion failed: ${msg}`);
  }
}

function runTests() {
  console.log('--- Testing SubtitleSearcher (Binary Search & Monotonic Cache) ---');
  const searcher = new SubtitleSearcher();

  const mockEntries: SubtitleEntry[] = [
    { index: 1, start_ms: 1000, end_ms: 3000, text: '第一句字幕', confidence: 0.95 },
    { index: 2, start_ms: 3500, end_ms: 5000, text: '第二句字幕', confidence: 0.92 },
    { index: 3, start_ms: 6000, end_ms: 8000, text: '第三句字幕', confidence: 0.88 },
    { index: 4, start_ms: 8200, end_ms: 10000, text: '第四句字幕', confidence: 0.99 },
  ];

  // 1. 空列表与负时间
  assert(searcher.findActiveIndex([], 2000) === null, 'Empty list should return null');
  assert(searcher.findActiveIndex(mockEntries, -100) === null, 'Negative time should return null');

  // 2. 外部极值 (Before first entry / After last entry)
  assert(searcher.findActiveIndex(mockEntries, 500) === null, 'Time before first entry should be null');
  assert(searcher.findActiveIndex(mockEntries, 15000) === null, 'Time after last entry should be null');

  // 3. 精确命中边界
  assert(searcher.findActiveIndex(mockEntries, 1000) === 1, 'Hit start boundary of entry 1');
  assert(searcher.findActiveIndex(mockEntries, 2000) === 1, 'Hit middle of entry 1');
  assert(searcher.findActiveIndex(mockEntries, 3000) === 1, 'Hit end boundary of entry 1');

  // 4. Gap 间隙
  assert(searcher.findActiveIndex(mockEntries, 3200) === null, 'Gap between 1 and 2 should be null');

  // 5. 连续播放与缓存命中
  assert(searcher.findActiveIndex(mockEntries, 3600) === 2, 'Sequential step to entry 2');
  assert(searcher.findActiveIndex(mockEntries, 3800) === 2, 'Cache hit for entry 2');
  assert(searcher.findActiveIndex(mockEntries, 6100) === 3, 'Step to entry 3');

  // 6. Seek 倒退与跳跃
  assert(searcher.findActiveIndex(mockEntries, 9000) === 4, 'Seek to entry 4');
  assert(searcher.findActiveIndex(mockEntries, 1500) === 1, 'Seek backward to entry 1');

  // 7. 缓存重置
  searcher.resetCache();
  assert(searcher.findActiveIndex(mockEntries, 1500) === 1, 'Re-search after resetCache');
  console.log('✓ SubtitleSearcher binary search & caching verified');

  console.log('--- Testing Flexible Timecode Parsing (Fix F-01) ---');
  // 1. Full standard timestamp
  assert(parseFlexibleTimecodeToMs('01:02:03,456') === 3723456, 'Parse full HH:MM:SS,mmm');
  assert(parseFlexibleTimecodeToMs('01:02:03.456') === 3723456, 'Parse full HH:MM:SS.mmm');
  assert(parseSrtTimestampToMs('01:02:03,456') === 3723456, 'parseSrtTimestampToMs alias');

  // 2. Short display format MM:SS.d or MM:SS.mmm
  assert(parseFlexibleTimecodeToMs('00:01.2') === 1200, 'Parse MM:SS.d format');
  assert(parseFlexibleTimecodeToMs('02:30.500') === 150500, 'Parse MM:SS.mmm format');
  assert(parseFlexibleTimecodeToMs('02:30,500') === 150500, 'Parse MM:SS,mmm format');
  assert(parseFlexibleTimecodeToMs('01:15') === 75000, 'Parse MM:SS format');

  // 3. Pure seconds
  assert(parseFlexibleTimecodeToMs('12.5') === 12500, 'Parse pure seconds float');
  assert(parseFlexibleTimecodeToMs('45') === 45000, 'Parse pure seconds integer');

  // 4. Fallback / Invalid
  assert(parseFlexibleTimecodeToMs('') === 0, 'Empty string returns 0');
  assert(parseFlexibleTimecodeToMs('invalid') === 0, 'Invalid string returns 0');
  console.log('✓ Flexible timecode parsing verified across all formats');

  console.log('--- Testing srt_formatter (format, sort & multi-line parse) ---');
  // 1. Unordered input -> Auto-sorted with continuous 1-based index
  const unorderedEntries: SubtitleEntry[] = [
    { index: 99, start_ms: 5000, end_ms: 7000, text: '第二句', confidence: 0.9 },
    { index: 12, start_ms: 1000, end_ms: 3000, text: '第一句\n含换行', confidence: 0.95 },
  ];

  const srtOutput = formatSrt(unorderedEntries);
  assert(
    srtOutput.startsWith('1\n00:00:01,000 --> 00:00:03,000\n第一句\n含换行\n\n2\n00:00:05,000 --> 00:00:07,000\n第二句'),
    'Auto sort and format mismatch'
  );

  // 2. Round-trip Parse with multi-line text
  const parsed = parseSrt(srtOutput);
  assert(parsed.length === 2, `Parsed length expected 2, got ${parsed.length}`);
  assert(parsed[0].text === '第一句\n含换行', 'Multi-line parsed text mismatch');
  assert(parsed[0].start_ms === 1000 && parsed[0].end_ms === 3000, 'Parsed time mismatch on entry 1');
  assert(parsed[1].text === '第二句', 'Parsed text mismatch on entry 2');
  console.log('✓ srt_formatter sorting, formatting, and multi-line round-trip parsing verified');

  console.log('All transcript sync & srt tests passed successfully!');
}

runTests();
