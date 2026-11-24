import { validateTimeRange } from './timecode';
import type { ProofreadCue, SubtitleEntry, WorkbenchEngine } from './types';

export * from './history';

export const DEFAULT_LOW_CONF_THRESHOLD = 0.60;
export const APPLE_VISION_LOW_CONF_THRESHOLD = 0.45;
export const VLM_LOW_CONF_THRESHOLD = 0.01;
export const PLACEHOLDER_TEXT = '[待补录]';

/**
 * 按提取引擎返回低置信度判定阈值：
 * - Apple Vision 对准确中文/混排常给出离散 0.50，0.45 可过滤 0.30/0.40 真实疑点而避免大面积误报
 * - VLM 以 [待补录]（confidence=0.0）为主
 * - PaddleOCR 沿用 0.60
 */
export function resolveLowConfidenceThreshold(
  engine?: WorkbenchEngine | null
): number {
  switch (engine) {
    case 'apple_vision':
      return APPLE_VISION_LOW_CONF_THRESHOLD;
    case 'vlm':
      return VLM_LOW_CONF_THRESHOLD;
    case 'paddle':
    default:
      return DEFAULT_LOW_CONF_THRESHOLD;
  }
}

/**
 * 过滤纯空白文本噪点条目（与 C++ SRT 导出格式化器行为对齐）。
 */
export function filterNonBlankEntries<T extends Pick<SubtitleEntry, 'text'>>(
  entries: readonly T[]
): T[] {
  return entries.filter((entry) => entry.text.trim().length > 0);
}

/**
 * 判断字幕条目是否属于低置信度或待补录占位条。
 */
export function isLowConfidenceCue(
  cue: Pick<SubtitleEntry, 'confidence' | 'text'>,
  threshold: number = DEFAULT_LOW_CONF_THRESHOLD
): boolean {
  return cue.confidence < threshold || cue.text.includes(PLACEHOLDER_TEXT);
}

export function filterLowConfidenceCues<T extends Pick<SubtitleEntry, 'confidence' | 'text'>>(
  cues: readonly T[],
  threshold: number = DEFAULT_LOW_CONF_THRESHOLD
): T[] {
  return cues.filter((cue) => isLowConfidenceCue(cue, threshold));
}

export function countLowConfidenceCues<T extends Pick<SubtitleEntry, 'confidence' | 'text'>>(
  cues: readonly T[],
  threshold: number = DEFAULT_LOW_CONF_THRESHOLD
): number {
  let count = 0;
  for (const cue of cues) {
    if (isLowConfidenceCue(cue, threshold)) {
      count += 1;
    }
  }
  return count;
}

/**
 * 铸造全局唯一递增数值 ID，确保拆分后的 SubtitleEntry.id 不与现有条目冲突。
 */
export function mintNextCueId(cues: readonly Pick<SubtitleEntry, 'id'>[]): number {
  let maxId = 0;
  for (const cue of cues) {
    if (Number.isSafeInteger(cue.id) && cue.id > maxId) {
      maxId = cue.id;
    }
  }
  return maxId + 1;
}

/**
 * 生成前端稳定行级 local_uid（避免与展示序号混淆）。
 */
export function mintLocalUid(prefix: string = 'cue', seq?: number): string {
  const randomPart = Math.random().toString(36).slice(2, 8);
  return seq !== undefined ? `${prefix}_${seq}_${randomPart}` : `${prefix}_${Date.now()}_${randomPart}`;
}

export function toProofreadCues(
  entries: readonly SubtitleEntry[],
  existing?: readonly ProofreadCue[]
): ProofreadCue[] {
  const existingById = new Map<number, ProofreadCue>();
  if (existing) {
    for (const cue of existing) {
      existingById.set(cue.id, cue);
    }
  }
  return entries.map((entry, idx) => {
    const prev = existingById.get(entry.id);
    return {
      ...entry,
      local_uid: prev?.local_uid ?? `cue_${entry.id}_${idx}`,
      category: prev?.category,
      re_recognized: prev?.re_recognized,
    };
  });
}

export function toSubtitleEntries(cues: readonly SubtitleEntry[]): SubtitleEntry[] {
  return cues.map((c) => ({
    id: c.id,
    start_ms: c.start_ms,
    end_ms: c.end_ms,
    text: c.text,
    confidence: c.confidence,
  }));
}

export type CueEditResult =
  | { ok: true; cues: ProofreadCue[] }
  | { ok: false; reason: string };

export function updateCueText(
  cues: readonly ProofreadCue[],
  localUid: string,
  text: string
): ProofreadCue[] {
  return cues.map((cue) => (cue.local_uid === localUid ? { ...cue, text } : cue));
}

export function updateCueTimes(
  cues: readonly ProofreadCue[],
  localUid: string,
  startMs: number,
  endMs: number
): CueEditResult {
  const check = validateTimeRange(startMs, endMs);
  if (!check.valid) {
    return { ok: false, reason: check.reason ?? '起止时间非法' };
  }
  return {
    ok: true,
    cues: cues.map((cue) =>
      cue.local_uid === localUid
        ? { ...cue, start_ms: startMs, end_ms: endMs }
        : cue
    ),
  };
}

export function adjustCueTime(
  cues: readonly ProofreadCue[],
  localUid: string,
  field: 'start_ms' | 'end_ms',
  deltaMs: number
): CueEditResult {
  const target = cues.find((c) => c.local_uid === localUid);
  if (!target) {
    return { ok: false, reason: '未找到目标字幕条目' };
  }
  const delta = Math.round(deltaMs);
  const nextStart =
    field === 'start_ms' ? Math.max(0, target.start_ms + delta) : target.start_ms;
  const nextEnd =
    field === 'end_ms' ? Math.max(0, target.end_ms + delta) : target.end_ms;
  return updateCueTimes(cues, localUid, nextStart, nextEnd);
}

export function deleteCueById(
  cues: readonly ProofreadCue[],
  localUid: string
): ProofreadCue[] {
  return cues.filter((cue) => cue.local_uid !== localUid);
}

export function joinSubtitleLines(a: string, b: string): string {
  return [a.trim(), b.trim()].filter(Boolean).join('\n');
}

/**
 * 与下一条相邻字幕合并：保留较低置信度 Math.min(a.confidence, b.confidence)。
 */
export function mergeAdjacentCues(
  cues: readonly ProofreadCue[],
  index: number
): CueEditResult {
  if (index < 0 || index + 1 >= cues.length) {
    return { ok: false, reason: '当前条目无下一条相邻字幕可合并' };
  }
  const a = cues[index];
  const b = cues[index + 1];
  const merged: ProofreadCue = {
    ...a,
    start_ms: Math.min(a.start_ms, b.start_ms),
    end_ms: Math.max(a.end_ms, b.end_ms),
    text: joinSubtitleLines(a.text, b.text),
    confidence: Math.min(a.confidence, b.confidence),
    re_recognized: Boolean(a.re_recognized || b.re_recognized),
  };
  return {
    ok: true,
    cues: [...cues.slice(0, index), merged, ...cues.slice(index + 2)],
  };
}

interface GraphemeSegmentRange {
  start: number;
  end: number;
}

const graphemeSegmenter =
  typeof Intl !== 'undefined' && typeof Intl.Segmenter === 'function'
    ? new Intl.Segmenter(undefined, { granularity: 'grapheme' })
    : null;

function splitsSurrogatePair(text: string, pos: number): boolean {
  if (pos <= 0 || pos >= text.length) {
    return false;
  }
  const prev = text.charCodeAt(pos - 1);
  const curr = text.charCodeAt(pos);
  return prev >= 0xd800 && prev <= 0xdbff && curr >= 0xdc00 && curr <= 0xdfff;
}

function findEnclosingGrapheme(
  text: string,
  pos: number
): GraphemeSegmentRange | null {
  if (pos <= 0 || pos >= text.length) {
    return null;
  }
  if (graphemeSegmenter) {
    for (const seg of graphemeSegmenter.segment(text)) {
      const start = seg.index;
      const end = start + seg.segment.length;
      if (pos > start && pos < end) {
        return { start, end };
      }
      if (start >= pos) {
        break;
      }
    }
    return null;
  }
  if (splitsSurrogatePair(text, pos)) {
    return { start: pos - 1, end: pos + 1 };
  }
  return null;
}

export type SplitCueResult =
  | { ok: true; cues: ProofreadCue[]; splitCueId: number; splitLocalUid: string }
  | { ok: false; reason: string };

/**
 * 字素簇（含 UTF-16 代理对与 ZWJ 序列）安全的光标比例拆分：
 * - 若 cursorOffset 落在单个字素簇内部，优先尝试向前对齐到簇尾；若右侧 trim 后为空，回退到簇首。
 * - 两侧均执行 .trim()，且任一侧为空或时长 < 2ms 时拒绝拆分。
 * - 铸造全局唯一递增数值 id 与新 local_uid。
 */
export function splitCueAtCursor(
  cues: readonly ProofreadCue[],
  localUid: string,
  cursorOffset: number
): SplitCueResult {
  const index = cues.findIndex((c) => c.local_uid === localUid);
  if (index < 0) {
    return { ok: false, reason: '未找到待拆分字幕条目' };
  }
  const cue = cues[index];
  if (cue.end_ms - cue.start_ms < 2) {
    return { ok: false, reason: '条目时长不足 2ms，无法拆分' };
  }
  const text = cue.text;
  if (text.length < 2) {
    return { ok: false, reason: '文本长度不足，无法拆分' };
  }
  if (!Number.isFinite(cursorOffset)) {
    return { ok: false, reason: '无效光标位置' };
  }

  const clampedOffset = Math.max(0, Math.min(text.length, Math.trunc(cursorOffset)));
  let candidate = clampedOffset;
  let left = '';
  let right = '';

  const enclosing = findEnclosingGrapheme(text, clampedOffset);
  if (enclosing) {
    const plusLeft = text.slice(0, enclosing.end).trim();
    const plusRight = text.slice(enclosing.end).trim();
    if (plusRight.length > 0) {
      candidate = enclosing.end;
      left = plusLeft;
      right = plusRight;
    } else {
      candidate = enclosing.start;
      left = text.slice(0, enclosing.start).trim();
      right = text.slice(enclosing.start).trim();
    }
  } else {
    left = text.slice(0, candidate).trim();
    right = text.slice(candidate).trim();
  }

  if (left.length === 0 || right.length === 0) {
    return { ok: false, reason: '拆分后两侧文本均不能为空' };
  }

  const leftRatio = candidate / text.length;
  const rawSplitMs = Math.round(
    cue.start_ms + (cue.end_ms - cue.start_ms) * leftRatio
  );
  const splitMs = Math.max(
    cue.start_ms + 1,
    Math.min(cue.end_ms - 1, rawSplitMs)
  );

  const newId = mintNextCueId(cues);
  const newLocalUid = mintLocalUid('split', newId);

  const firstCue: ProofreadCue = {
    ...cue,
    end_ms: splitMs,
    text: left,
  };
  const secondCue: ProofreadCue = {
    ...cue,
    id: newId,
    local_uid: newLocalUid,
    start_ms: splitMs,
    end_ms: cue.end_ms,
    text: right,
  };

  return {
    ok: true,
    cues: [...cues.slice(0, index), firstCue, secondCue, ...cues.slice(index + 1)],
    splitCueId: newId,
    splitLocalUid: newLocalUid,
  };
}

// ==================== 查找与替换（纯字面量匹配，免疫正则元字符） ====================

export interface CueMatch {
  taskId: string;
  localUid: string;
  cueIndex: number;
  startIndex: number;
  length: number;
}

export interface LiteralMatchSpan {
  startIndex: number;
  length: number;
}

export function findLiteralMatches(
  haystack: string,
  needle: string,
  caseSensitive: boolean
): LiteralMatchSpan[] {
  if (!needle || !haystack) {
    return [];
  }
  if (caseSensitive) {
    const spans: LiteralMatchSpan[] = [];
    let from = 0;
    while (from <= haystack.length - needle.length) {
      const pos = haystack.indexOf(needle, from);
      if (pos < 0) {
        break;
      }
      spans.push({ startIndex: pos, length: needle.length });
      from = pos + needle.length;
    }
    return spans;
  }

  const query = needle.toLowerCase();
  if (!query) {
    return [];
  }

  let lowerSource = '';
  const origStartMap: number[] = [];
  const origEndMap: number[] = [];

  for (let i = 0; i < haystack.length; ) {
    const cp = haystack.codePointAt(i)!;
    const cpLen = cp > 0xffff ? 2 : 1;
    const origStart = i;
    const origEnd = i + cpLen;
    const lowerPiece = haystack.slice(origStart, origEnd).toLowerCase();
    for (let k = 0; k < lowerPiece.length; k += 1) {
      origStartMap.push(origStart);
      origEndMap.push(origEnd);
    }
    lowerSource += lowerPiece;
    i = origEnd;
  }

  const spans: LiteralMatchSpan[] = [];
  let from = 0;
  while (from <= lowerSource.length - query.length) {
    const pos = lowerSource.indexOf(query, from);
    if (pos < 0) {
      break;
    }
    const endPos = pos + query.length;
    const startAligned =
      pos === 0 || origStartMap[pos] !== origStartMap[pos - 1];
    const endAligned =
      endPos === lowerSource.length ||
      origEndMap[endPos - 1] !== origEndMap[endPos];
    if (!startAligned || !endAligned) {
      from = pos + 1;
      continue;
    }
    const origStart = origStartMap[pos];
    const origEnd = origEndMap[endPos - 1];
    spans.push({ startIndex: origStart, length: origEnd - origStart });
    from = endPos;
  }
  return spans;
}

export function findCueMatches(
  taskId: string,
  cues: readonly ProofreadCue[],
  query: string,
  caseSensitive: boolean = false
): CueMatch[] {
  if (!query) {
    return [];
  }
  const matches: CueMatch[] = [];
  for (let i = 0; i < cues.length; i += 1) {
    const cue = cues[i];
    const spans = findLiteralMatches(cue.text, query, caseSensitive);
    for (const span of spans) {
      matches.push({
        taskId,
        localUid: cue.local_uid,
        cueIndex: i,
        startIndex: span.startIndex,
        length: span.length,
      });
    }
  }
  return matches;
}

export function findQueueMatches(
  items: readonly { taskId: string; cues: readonly ProofreadCue[] }[],
  query: string,
  caseSensitive: boolean = false
): CueMatch[] {
  if (!query) {
    return [];
  }
  const all: CueMatch[] = [];
  for (const item of items) {
    all.push(...findCueMatches(item.taskId, item.cues, query, caseSensitive));
  }
  return all;
}

export function replaceSingleMatch(
  cues: readonly ProofreadCue[],
  match: CueMatch,
  replacement: string
): ProofreadCue[] {
  return cues.map((cue) => {
    if (cue.local_uid !== match.localUid) {
      return cue;
    }
    const nextText =
      cue.text.slice(0, match.startIndex) +
      replacement +
      cue.text.slice(match.startIndex + match.length);
    return { ...cue, text: nextText };
  });
}

export function replaceAllInCues(
  cues: readonly ProofreadCue[],
  query: string,
  replacement: string,
  caseSensitive: boolean = false
): { cues: ProofreadCue[]; replacedCount: number } {
  if (!query) {
    return { cues: [...cues], replacedCount: 0 };
  }
  let total = 0;
  const next = cues.map((cue) => {
    const spans = findLiteralMatches(cue.text, query, caseSensitive);
    if (spans.length === 0) {
      return cue;
    }
    total += spans.length;
    let rebuilt = '';
    let cursor = 0;
    for (const span of spans) {
      rebuilt += cue.text.slice(cursor, span.startIndex) + replacement;
      cursor = span.startIndex + span.length;
    }
    rebuilt += cue.text.slice(cursor);
    return { ...cue, text: rebuilt };
  });
  return { cues: next, replacedCount: total };
}

export function replaceAllAcrossQueue(
  items: readonly { taskId: string; cues: readonly ProofreadCue[] }[],
  query: string,
  replacement: string,
  caseSensitive: boolean = false
): {
  updatedTasks: Array<{ taskId: string; cues: ProofreadCue[]; replacedCount: number }>;
  totalReplaced: number;
} {
  const updatedTasks: Array<{
    taskId: string;
    cues: ProofreadCue[];
    replacedCount: number;
  }> = [];
  let totalReplaced = 0;
  for (const item of items) {
    const res = replaceAllInCues(item.cues, query, replacement, caseSensitive);
    if (res.replacedCount > 0) {
      updatedTasks.push({
        taskId: item.taskId,
        cues: res.cues,
        replacedCount: res.replacedCount,
      });
      totalReplaced += res.replacedCount;
    }
  }
  return { updatedTasks, totalReplaced };
}
