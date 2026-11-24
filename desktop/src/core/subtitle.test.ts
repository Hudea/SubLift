import { describe, expect, it } from 'vitest';
import {
  canRedo,
  canUndo,
  createHistory,
  pushHistory,
  redoHistory,
  undoHistory,
} from './history';
import {
  adjustCueTime,
  APPLE_VISION_LOW_CONF_THRESHOLD,
  DEFAULT_LOW_CONF_THRESHOLD,
  filterLowConfidenceCues,
  filterNonBlankEntries,
  isLowConfidenceCue,
  mergeAdjacentCues,
  mintNextCueId,
  PLACEHOLDER_TEXT,
  replaceAllAcrossQueue,
  replaceAllInCues,
  replaceSingleMatch,
  resolveLowConfidenceThreshold,
  findCueMatches,
  findQueueMatches,
  splitCueAtCursor,
  updateCueTimes,
  VLM_LOW_CONF_THRESHOLD,
} from './subtitle';
import type { ProofreadCue } from './types';

function makeCue(
  id: number,
  start_ms: number,
  end_ms: number,
  text: string,
  confidence: number = 0.95
): ProofreadCue {
  return {
    id,
    local_uid: `cue_${id}`,
    start_ms,
    end_ms,
    text,
    confidence,
  };
}

describe('core/subtitle & core/history', () => {
  it('flags low-confidence cues (< 0.60) and [待补录] placeholders', () => {
    expect(DEFAULT_LOW_CONF_THRESHOLD).toBe(0.6);
    expect(isLowConfidenceCue(makeCue(1, 0, 1000, '你好', 0.59))).toBe(true);
    expect(isLowConfidenceCue(makeCue(2, 0, 1000, '你好', 0.6))).toBe(false);
    expect(isLowConfidenceCue(makeCue(3, 0, 1000, `前段 ${PLACEHOLDER_TEXT}`, 0.99))).toBe(true);

    const list = [
      makeCue(1, 0, 1000, 'A', 0.4),
      makeCue(2, 1000, 2000, 'B', 0.9),
      makeCue(3, 2000, 3000, PLACEHOLDER_TEXT, 0.8),
    ];
    expect(filterLowConfidenceCues(list).map((c) => c.id)).toEqual([1, 3]);
  });

  it('validates times, micro-adjusts, and merges adjacent cues keeping min confidence', () => {
    const cues = [
      makeCue(1, 1000, 2000, '第一行', 0.92),
      makeCue(2, 2200, 3400, '第二行', 0.48),
    ];

    expect(updateCueTimes(cues, 'cue_1', 2500, 2000).ok).toBe(false);
    const nudged = adjustCueTime(cues, 'cue_1', 'start_ms', -100);
    expect(nudged.ok).toBe(true);
    if (nudged.ok) {
      expect(nudged.cues[0].start_ms).toBe(900);
    }

    const merged = mergeAdjacentCues(cues, 0);
    expect(merged.ok).toBe(true);
    if (merged.ok) {
      expect(merged.cues).toHaveLength(1);
      expect(merged.cues[0].text).toBe('第一行\n第二行');
      expect(merged.cues[0].start_ms).toBe(1000);
      expect(merged.cues[0].end_ms).toBe(3400);
      expect(merged.cues[0].confidence).toBe(0.48);
    }
  });

  it('splits cues safely across UTF-16 surrogate pairs with +1 -> -1 fallback and unique IDs', () => {
    // "a😀" has length 3: 'a'(0), high(1), low(2). Offset 2 sits inside surrogate pair!
    // +1 yields offset 3 -> right="" (empty), so falls back to offset 1 -> "a" | "😀"
    const cues1 = [makeCue(10, 1000, 4000, 'a😀')];
    const res1 = splitCueAtCursor(cues1, 'cue_10', 2);
    expect(res1.ok).toBe(true);
    if (res1.ok) {
      expect(res1.cues).toHaveLength(2);
      expect(res1.cues[0].text).toBe('a');
      expect(res1.cues[1].text).toBe('😀');
      expect(res1.cues[1].id).toBe(11);
      expect(res1.cues[0].end_ms).toBe(res1.cues[1].start_ms);
      expect(res1.cues[0].end_ms).toBeGreaterThan(1000);
      expect(res1.cues[0].end_ms).toBeLessThan(4000);
    }

    // "😀a" at offset 1 -> +1 yields offset 2 -> "😀" | "a"
    const cues2 = [makeCue(5, 1000, 4000, '😀a')];
    const res2 = splitCueAtCursor(cues2, 'cue_5', 1);
    expect(res2.ok).toBe(true);
    if (res2.ok) {
      expect(res2.cues[0].text).toBe('😀');
      expect(res2.cues[1].text).toBe('a');
    }

    // "Hello 😀 世界" at offset 7 (inside 😀)
    const cues3 = [makeCue(1, 0, 1000, 'Hello 😀 世界')];
    const res3 = splitCueAtCursor(cues3, 'cue_1', 7);
    expect(res3.ok).toBe(true);
    if (res3.ok) {
      expect(res3.cues[0].text).toBe('Hello 😀');
      expect(res3.cues[1].text).toBe('世界');
    }

    // Rejects trailing space after emoji ("😀 ") and duration < 2ms
    expect(splitCueAtCursor([makeCue(1, 0, 1000, '😀 ')], 'cue_1', 1).ok).toBe(false);
    expect(splitCueAtCursor([makeCue(1, 100, 101, 'ab')], 'cue_1', 1).ok).toBe(false);

    // 2ms boundary cue splits cleanly into [start, start+1] and [start+1, start+2]
    const minSplit = splitCueAtCursor([makeCue(1, 100, 102, 'ab')], 'cue_1', 1);
    expect(minSplit.ok).toBe(true);
    if (minSplit.ok) {
      expect(minSplit.cues[0].end_ms).toBe(101);
      expect(minSplit.cues[1].start_ms).toBe(101);
      expect(minSplit.cues[1].end_ms).toBe(102);
    }

    expect(mintNextCueId([makeCue(3, 0, 10, 'a'), makeCue(99, 10, 20, 'b')])).toBe(100);

    // ZWJ grapheme cluster protection: never tear across \u200D
    const zwj = '👨‍👩‍👧a';
    for (let offset = 1; offset < zwj.length - 1; offset += 1) {
      const zwjSplit = splitCueAtCursor([makeCue(1, 0, 1000, zwj)], 'cue_1', offset);
      expect(zwjSplit.ok).toBe(true);
      if (zwjSplit.ok) {
        expect(zwjSplit.cues[0].text).toBe('👨‍👩‍👧');
        expect(zwjSplit.cues[1].text).toBe('a');
        expect(zwjSplit.cues[1].text.startsWith('\u200D')).toBe(false);
      }
    }
    const zwjRight = splitCueAtCursor([makeCue(1, 0, 1000, 'a👨‍👩‍👧')], 'cue_1', 4);
    expect(zwjRight.ok).toBe(true);
    if (zwjRight.ok) {
      expect(zwjRight.cues[0].text).toBe('a');
      expect(zwjRight.cues[1].text).toBe('👨‍👩‍👧');
    }
    expect(splitCueAtCursor([makeCue(1, 0, 1000, 'ab')], 'cue_1', Number.NaN).ok).toBe(false);
  });

  it('performs literal single and cross-queue find & replace safely with regex metacharacters', () => {
    const taskACues = [
      makeCue(1, 0, 1000, '匹配.*[()]这里'),
      makeCue(2, 1000, 2000, 'ABC abc'),
    ];
    const taskBCues = [makeCue(1, 0, 1000, '另一集.*[()]结尾')];

    const matches = findCueMatches('t1', taskACues, '.*[()]');
    expect(matches).toHaveLength(1);
    const singleReplaced = replaceSingleMatch(taskACues, matches[0], '已替换');
    expect(singleReplaced[0].text).toBe('匹配已替换这里');

    const caseInsensitive = findCueMatches('t1', taskACues, 'abc', false);
    expect(caseInsensitive).toHaveLength(2);
    const caseSensitive = findCueMatches('t1', taskACues, 'abc', true);
    expect(caseSensitive).toHaveLength(1);

    const allInA = replaceAllInCues(taskACues, 'abc', 'XYZ', false);
    expect(allInA.replacedCount).toBe(2);
    expect(allInA.cues[1].text).toBe('XYZ XYZ');

    const queueMatches = findQueueMatches(
      [
        { taskId: 't1', cues: taskACues },
        { taskId: 't2', cues: taskBCues },
      ],
      '.*[()]'
    );
    expect(queueMatches).toHaveLength(2);

    const queueReplace = replaceAllAcrossQueue(
      [
        { taskId: 't1', cues: taskACues },
        { taskId: 't2', cues: taskBCues },
      ],
      '.*[()]',
      'OK'
    );
    expect(queueReplace.totalReplaced).toBe(2);
    expect(queueReplace.updatedTasks).toHaveLength(2);

    // Length-expanding Unicode case-folding ('İ' -> 'i\u0307') must not shift replacement offsets
    const turkishCue = [makeCue(1, 0, 1000, 'İstanbul ABC xyz')];
    const turkishMatches = findCueMatches('t1', turkishCue, 'abc', false);
    expect(turkishMatches).toHaveLength(1);
    expect(turkishMatches[0].startIndex).toBe(9);
    expect(turkishMatches[0].length).toBe(3);
    const turkishReplaced = replaceAllInCues(turkishCue, 'abc', '123', false);
    expect(turkishReplaced.cues[0].text).toBe('İstanbul 123 xyz');
    // Searching 'i' must not split 'İ'
    expect(findCueMatches('t1', [makeCue(1, 0, 1000, 'İstanbul')], 'i', false)).toHaveLength(0);
    // Searching 'İ' matches full 'İ' code unit
    expect(replaceAllInCues([makeCue(1, 0, 1000, 'İstanbul')], 'İ', 'X', false).cues[0].text).toBe('Xstanbul');
  });

  it('maintains immutable undo/redo history with maxDepth and future truncation', () => {
    let hist = createHistory([1], 2);
    expect(canUndo(hist)).toBe(false);
    hist = pushHistory(hist, [1, 2]);
    hist = pushHistory(hist, [1, 2, 3]);
    hist = pushHistory(hist, [1, 2, 3, 4]); // maxDepth 2 -> past holds [1,2], [1,2,3]
    expect(hist.past).toHaveLength(2);

    hist = undoHistory(hist);
    expect(hist.present).toEqual([1, 2, 3]);
    expect(canRedo(hist)).toBe(true);

    hist = redoHistory(hist);
    expect(hist.present).toEqual([1, 2, 3, 4]);

    hist = undoHistory(hist);
    hist = pushHistory(hist, [99]);
    expect(canRedo(hist)).toBe(false);
  });

  it('handles $ replacement literals, overlapping matches, flag/combining graphemes, negative times, and history boundaries', () => {
    // Replacement special tokens ($&, $1, $$, $') are treated literally
    const dollarCues = [makeCue(1, 0, 1000, 'foo bar foo')];
    const m = findCueMatches('t1', dollarCues, 'foo');
    expect(replaceSingleMatch(dollarCues, m[0], '$&$1$$$\'')[0].text).toBe('$&$1$$$\' bar foo');
    expect(replaceAllInCues(dollarCues, 'foo', '$1$$$&', false).cues[0].text).toBe(
      '$1$$$& bar $1$$$&'
    );

    // Non-overlapping literal scans: 'aaaa' matched by 'aa' produces 2 hits
    const overlapCues = [makeCue(1, 0, 1000, 'aaaa')];
    expect(findCueMatches('t1', overlapCues, 'aa', true)).toHaveLength(2);
    expect(findCueMatches('t1', overlapCues, 'AA', false)).toHaveLength(2);
    expect(replaceAllInCues(overlapCues, 'aa', 'b', false).cues[0].text).toBe('bb');

    // Regional-indicator flag ('🇨🇳a', 4 UTF-16 units for flag) and combining mark ('e\u0301a')
    const flagRes = splitCueAtCursor([makeCue(1, 0, 1000, '🇨🇳a')], 'cue_1', 2);
    expect(flagRes.ok).toBe(true);
    if (flagRes.ok) {
      expect(flagRes.cues[0].text).toBe('🇨🇳');
      expect(flagRes.cues[1].text).toBe('a');
    }
    const combRes = splitCueAtCursor([makeCue(1, 0, 1000, 'e\u0301a')], 'cue_1', 1);
    expect(combRes.ok).toBe(true);
    if (combRes.ok) {
      expect(combRes.cues[0].text).toBe('e\u0301');
      expect(combRes.cues[1].text).toBe('a');
    }

    // Negative start_ms cues rejected by updateCueTimes; adjustCueTime clamps start_ms to 0
    expect(updateCueTimes([makeCue(1, 500, 1500, 'hi')], 'cue_1', -1, 1500).ok).toBe(false);
    const clampedNudge = adjustCueTime([makeCue(1, 500, 1500, 'hi')], 'cue_1', 'start_ms', -5000);
    expect(clampedNudge.ok).toBe(true);
    if (clampedNudge.ok) {
      expect(clampedNudge.cues[0].start_ms).toBe(0);
    }

    // History invalid maxDepth (0, -1, NaN) falls back to 100; empty undo/redo & Object.is push are idempotent
    for (const badDepth of [0, -1, Number.NaN]) {
      const h = createHistory([1], badDepth);
      expect(h.maxDepth).toBe(100);
      expect(undoHistory(h)).toBe(h);
      expect(redoHistory(h)).toBe(h);
      expect(pushHistory(h, h.present)).toBe(h);
    }
  });

  it('resolves per-engine low-confidence thresholds and filters blank noise entries', () => {
    expect(resolveLowConfidenceThreshold('apple_vision')).toBe(
      APPLE_VISION_LOW_CONF_THRESHOLD
    );
    expect(resolveLowConfidenceThreshold('paddle')).toBe(DEFAULT_LOW_CONF_THRESHOLD);
    expect(resolveLowConfidenceThreshold('vlm')).toBe(VLM_LOW_CONF_THRESHOLD);
    expect(resolveLowConfidenceThreshold(undefined)).toBe(DEFAULT_LOW_CONF_THRESHOLD);

    const sample = [
      makeCue(1, 0, 1000, '准确率0.50正常中文', 0.5),
      makeCue(2, 1000, 2000, '真实模糊行', 0.3),
      makeCue(3, 2000, 3000, '高亮行', 0.65),
      makeCue(4, 3000, 4000, PLACEHOLDER_TEXT, 0.0),
    ];

    expect(
      filterLowConfidenceCues(sample, resolveLowConfidenceThreshold('apple_vision')).map(
        (c) => c.id
      )
    ).toEqual([2, 4]);
    expect(
      filterLowConfidenceCues(sample, resolveLowConfidenceThreshold('paddle')).map(
        (c) => c.id
      )
    ).toEqual([1, 2, 4]);
    expect(
      filterLowConfidenceCues(sample, resolveLowConfidenceThreshold('vlm')).map(
        (c) => c.id
      )
    ).toEqual([4]);

    const rawEntries = [
      { id: 1, start_ms: 0, end_ms: 500, text: '', confidence: 0.0 },
      { id: 2, start_ms: 500, end_ms: 1000, text: '   \n\t ', confidence: 0.3 },
      { id: 3, start_ms: 1000, end_ms: 2000, text: '有效台词', confidence: 0.5 },
    ];
    expect(filterNonBlankEntries(rawEntries).map((e) => e.id)).toEqual([3]);
  });
});
