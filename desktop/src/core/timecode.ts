/**
 * 将毫秒值规范化为非负整数；非有限数字回退为 0。
 */
export function clampMs(ms: number): number {
  if (!Number.isFinite(ms)) {
    return 0;
  }
  return Math.max(0, Math.round(ms));
}

/**
 * 将毫秒值钳位到 [0, maxMs] 区间。
 */
export function clampMsToRange(ms: number, maxMs?: number): number {
  const v = clampMs(ms);
  if (maxMs !== undefined && Number.isFinite(maxMs) && maxMs >= 0) {
    return Math.min(v, Math.round(maxMs));
  }
  return v;
}

/**
 * 与 C++ sublift::text::format_timestamp_srt 完全对齐的 SRT 时码格式化：HH:MM:SS,mmm。
 */
export function formatSrtTimecode(ms: number): string {
  const total = clampMs(ms);
  const hours = Math.floor(total / 3_600_000);
  const minutes = Math.floor((total % 3_600_000) / 60_000);
  const seconds = Math.floor((total % 60_000) / 1000);
  const millis = total % 1000;
  return `${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')},${String(millis).padStart(3, '0')}`;
}

/**
 * HUD 播控与画布时间码格式化：HH:MM:SS.mmm。
 */
export function formatHudTimecode(ms: number): string {
  const total = clampMs(ms);
  const hours = Math.floor(total / 3_600_000);
  const minutes = Math.floor((total % 3_600_000) / 60_000);
  const seconds = Math.floor((total % 60_000) / 1000);
  const millis = total % 1000;
  return `${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}.${String(millis).padStart(3, '0')}`;
}

/**
 * 紧凑队列卡片时长格式化：MM:SS 或 HH:MM:SS。
 */
export function formatCompactDuration(ms: number): string {
  const totalSec = Math.floor(clampMs(ms) / 1000);
  const hours = Math.floor(totalSec / 3600);
  const minutes = Math.floor((totalSec % 3600) / 60);
  const seconds = totalSec % 60;
  if (hours > 0) {
    return `${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`;
  }
  return `${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`;
}

const SRT_TIMECODE_ANY_RE = /^\s*(\d{2,}):([0-5]\d):([0-5]\d)([,.])(\d{3})\s*$/;
const SRT_TIMECODE_COMMA_RE = /^\s*(\d{2,}):([0-5]\d):([0-5]\d)(,)(\d{3})\s*$/;

/**
 * 解析 SRT/HUD 时码字符串为毫秒；格式非法返回 null。
 */
export function parseSrtTimecode(
  input: string,
  opts?: { allowDot?: boolean }
): number | null {
  const allowDot = opts?.allowDot ?? true;
  const re = allowDot ? SRT_TIMECODE_ANY_RE : SRT_TIMECODE_COMMA_RE;
  const match = re.exec(input);
  if (!match) {
    return null;
  }
  const h = Number(match[1]);
  const m = Number(match[2]);
  const s = Number(match[3]);
  const ms = Number(match[5]);
  const total = ((h * 60 + m) * 60 + s) * 1000 + ms;
  return Number.isSafeInteger(total) ? total : null;
}

export interface TimeRangeValidation {
  valid: boolean;
  reason?: string;
}

/**
 * 严格校验字幕条起止毫秒时间范围：0 <= startMs < endMs（均为有限整数）。
 */
export function validateTimeRange(
  startMs: number,
  endMs: number
): TimeRangeValidation {
  if (!Number.isFinite(startMs) || !Number.isFinite(endMs)) {
    return { valid: false, reason: '起止时间必须为有效数字' };
  }
  if (!Number.isInteger(startMs) || !Number.isInteger(endMs)) {
    return { valid: false, reason: '起止时间必须为整数毫秒' };
  }
  if (startMs < 0) {
    return { valid: false, reason: '开始时间不能小于 0ms' };
  }
  if (endMs <= startMs) {
    const diff = endMs - startMs;
    return {
      valid: false,
      reason: `结束时间必须大于开始时间（当前相差 ${diff}ms）`,
    };
  }
  return { valid: true };
}

/**
 * 计算字幕条目中点时刻（毫秒整数）。
 */
export function computeCueMidpointMs(startMs: number, endMs: number): number {
  return Math.floor((clampMs(startMs) + clampMs(endMs)) / 2);
}
