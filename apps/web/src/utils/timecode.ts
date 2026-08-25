/**
 * Formats seconds (float) to "HH:MM:SS.mmm"
 */
export function formatTimecode(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) {
    return '00:00:00.000';
  }

  const totalMs = Math.floor(seconds * 1000);
  const ms = totalMs % 1000;
  const totalSec = Math.floor(totalMs / 1000);
  const s = totalSec % 60;
  const m = Math.floor(totalSec / 60) % 60;
  const h = Math.floor(totalSec / 3600);

  const pad = (n: number, width = 2) => String(n).padStart(width, '0');
  return `${pad(h)}:${pad(m)}:${pad(s)}.${pad(ms, 3)}`;
}

/**
 * Formats milliseconds (integer) to "HH:MM:SS,mmm" (SRT format)
 */
export function formatSrtTimestamp(ms: number): string {
  if (!Number.isFinite(ms) || ms < 0) {
    return '00:00:00,000';
  }

  const totalMs = Math.floor(ms);
  const millis = totalMs % 1000;
  const totalSec = Math.floor(totalMs / 1000);
  const s = totalSec % 60;
  const m = Math.floor(totalSec / 60) % 60;
  const h = Math.floor(totalSec / 3600);

  const pad = (n: number, width = 2) => String(n).padStart(width, '0');
  return `${pad(h)}:${pad(m)}:${pad(s)},${pad(millis, 3)}`;
}

/**
 * Formats milliseconds to short display string "MM:SS.mmm"
 */
export function formatDisplayMs(ms: number): string {
  if (!Number.isFinite(ms) || ms < 0) {
    return '00:00.000';
  }
  const totalSec = Math.floor(ms / 1000);
  const s = totalSec % 60;
  const m = Math.floor(totalSec / 60) % 60;
  const millis = Math.floor(ms % 1000);
  const pad = (n: number, width = 2) => String(n).padStart(width, '0');
  return `${pad(m)}:${pad(s)}.${pad(millis, 3)}`;
}

/**
 * Flexible timecode parser for parsing various user input formats into milliseconds
 * Supports:
 * - "01:02:03.456", "01:02:03,456"
 * - "02:03.456", "02:03,456", "02:03.4", "02:03"
 * - "123.45", "123"
 */
export function parseFlexibleTimecode(timeStr: string): number | null {
  if (!timeStr || typeof timeStr !== 'string') {
    return null;
  }

  const clean = timeStr.trim().replace(',', '.');
  if (!clean) return null;

  // Split by colon
  const parts = clean.split(':');

  if (parts.length === 3) {
    // HH:MM:SS.mmm
    const h = Number(parts[0]);
    const m = Number(parts[1]);
    const s = Number(parts[2]);
    if (!Number.isFinite(h) || !Number.isFinite(m) || !Number.isFinite(s)) return null;
    if (h < 0 || m < 0 || m >= 60 || s < 0 || s >= 60) return null;
    return Math.max(0, Math.round((h * 3600 + m * 60 + s) * 1000));
  } else if (parts.length === 2) {
    // MM:SS.mmm
    const m = Number(parts[0]);
    const s = Number(parts[1]);
    if (!Number.isFinite(m) || !Number.isFinite(s)) return null;
    if (m < 0 || s < 0 || s >= 60) return null;
    return Math.max(0, Math.round((m * 60 + s) * 1000));
  } else if (parts.length === 1) {
    // Pure seconds
    const s = Number(parts[0]);
    if (!Number.isFinite(s) || s < 0) return null;
    return Math.max(0, Math.round(s * 1000));
  }

  return null;
}

export interface TimecodeValidationResult {
  valid: boolean;
  start_ms?: number;
  end_ms?: number;
  error?: string;
}

/**
 * Validates a start and end millisecond range
 */
export function validateTimecodeRange(start_ms: number, end_ms: number): { valid: boolean; error?: string } {
  if (!Number.isFinite(start_ms) || !Number.isFinite(end_ms)) {
    return { valid: false, error: '时间码必须为有效数值' };
  }
  if (start_ms < 0 || end_ms < 0) {
    return { valid: false, error: '时间码不能为负数' };
  }
  if (start_ms >= end_ms) {
    return {
      valid: false,
      error: `开始时间 (${formatDisplayMs(start_ms)}) 必须早于结束时间 (${formatDisplayMs(end_ms)})`,
    };
  }
  return { valid: true };
}

/**
 * Parses and validates a timecode string representing a range (e.g. "00:01.000 - 00:03.500" or "00:01.000 --> 00:03.500")
 */
export function validateTimecodeString(timeStr: string): TimecodeValidationResult {
  if (!timeStr || typeof timeStr !== 'string' || !timeStr.trim()) {
    return { valid: false, error: '时间码不能为空' };
  }

  let separator = '';
  if (timeStr.includes('-->')) {
    separator = '-->';
  } else if (timeStr.includes('->')) {
    separator = '->';
  } else if (timeStr.includes('~')) {
    separator = '~';
  } else if (timeStr.includes('至')) {
    separator = '至';
  } else if (timeStr.includes('-')) {
    separator = '-';
  } else {
    return {
      valid: false,
      error: '时间码格式错误：缺少分隔符，请输入类似于 "00:01.000 - 00:03.000" 的时间区间',
    };
  }

  const parts = timeStr.split(separator);
  if (parts.length !== 2) {
    return { valid: false, error: '时间码格式错误：必须包含开始和结束两个时间戳' };
  }

  const startMs = parseFlexibleTimecode(parts[0]);
  const endMs = parseFlexibleTimecode(parts[1]);

  if (startMs === null) {
    return { valid: false, error: `开始时间码格式错误: "${parts[0].trim()}"` };
  }
  if (endMs === null) {
    return { valid: false, error: `结束时间码格式错误: "${parts[1].trim()}"` };
  }

  const rangeCheck = validateTimecodeRange(startMs, endMs);
  if (!rangeCheck.valid) {
    return { valid: false, start_ms: startMs, end_ms: endMs, error: rangeCheck.error };
  }

  return { valid: true, start_ms: startMs, end_ms: endMs };
}

export interface TimelineConflictWarning {
  entryIndex: number;
  type: 'invalid_duration' | 'overlap_prev' | 'overlap_next';
  message: string;
}

/**
 * Analyzes a list of subtitle entries for timeline collisions, overlaps, and invalid durations
 */
export function detectTimelineConflicts(
  entries: Array<{ index: number; start_ms: number; end_ms: number; text: string }>
): Map<number, TimelineConflictWarning[]> {
  const conflictMap = new Map<number, TimelineConflictWarning[]>();

  if (!entries || entries.length === 0) {
    return conflictMap;
  }

  // Sort entries by start_ms
  const sorted = [...entries].sort((a, b) => a.start_ms - b.start_ms);

  for (let i = 0; i < sorted.length; i++) {
    const cur = sorted[i];
    const warnings: TimelineConflictWarning[] = [];

    // 1. Duration check (start >= end)
    if (cur.start_ms >= cur.end_ms) {
      warnings.push({
        entryIndex: cur.index,
        type: 'invalid_duration',
        message: `时长无效：开始时间 (${formatDisplayMs(cur.start_ms)}) 不小于结束时间 (${formatDisplayMs(cur.end_ms)})`,
      });
    }

    // 2. Overlap with previous entry
    if (i > 0) {
      const prev = sorted[i - 1];
      if (cur.start_ms < prev.end_ms) {
        warnings.push({
          entryIndex: cur.index,
          type: 'overlap_prev',
          message: `与前一条 (#${prev.index}) 时间重叠：前一条于 ${formatDisplayMs(prev.end_ms)} 结束，当前于 ${formatDisplayMs(cur.start_ms)} 开始`,
        });
      }
    }

    // 3. Overlap with next entry
    if (i < sorted.length - 1) {
      const next = sorted[i + 1];
      if (cur.end_ms > next.start_ms) {
        warnings.push({
          entryIndex: cur.index,
          type: 'overlap_next',
          message: `与后一条 (#${next.index}) 时间重叠：当前于 ${formatDisplayMs(cur.end_ms)} 结束，后一条于 ${formatDisplayMs(next.start_ms)} 开始`,
        });
      }
    }

    if (warnings.length > 0) {
      conflictMap.set(cur.index, warnings);
    }
  }

  return conflictMap;
}

