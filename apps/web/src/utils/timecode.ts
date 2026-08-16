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
