import type { SubtitleEntry } from '../types/api';
import { formatSrtTimestamp } from './timecode';
import { createZipArchive, downloadZipBlob } from './zip';

/**
 * 将 SubtitleEntry 列表格式化为标准规范的 UTF-8 SRT 字幕文本
 */
export function formatSrt(entries: SubtitleEntry[]): string {
  if (!entries || entries.length === 0) {
    return '';
  }

  // 保证按时间戳递增排序
  const sorted = [...entries].sort((a, b) => a.start_ms - b.start_ms);

  const blocks: string[] = [];
  sorted.forEach((entry, idx) => {
    const indexNum = idx + 1;
    const startStr = formatSrtTimestamp(Math.max(0, entry.start_ms));
    const endStr = formatSrtTimestamp(Math.max(entry.start_ms, entry.end_ms));
    const text = (entry.text || '').trim();

    blocks.push(`${indexNum}\n${startStr} --> ${endStr}\n${text}`);
  });

  return blocks.join('\n\n') + '\n';
}

/**
 * 弹性时码解析器：支持各类用户输入或展示格式并转换为毫秒整数
 * 支持：
 * - 完整时码: "01:02:03,456" / "01:02:03.456"
 * - 分秒时码: "02:03.456" / "02:03,456" / "02:03.4" / "02:03"
 * - 纯秒数: "123.45" / "123"
 */
export function parseFlexibleTimecodeToMs(timeStr: string): number {
  if (!timeStr || typeof timeStr !== 'string') {
    return 0;
  }

  const clean = timeStr.trim().replace(',', '.');
  if (!clean) return 0;

  // 1. 尝试按冒号切分段落 (HH:MM:SS 或 MM:SS)
  const parts = clean.split(':');

  if (parts.length === 3) {
    // HH:MM:SS.mmm
    const h = parseInt(parts[0], 10) || 0;
    const m = parseInt(parts[1], 10) || 0;
    const s = parseFloat(parts[2]) || 0;
    return Math.max(0, Math.round((h * 3600 + m * 60 + s) * 1000));
  } else if (parts.length === 2) {
    // MM:SS.mmm
    const m = parseInt(parts[0], 10) || 0;
    const s = parseFloat(parts[1]) || 0;
    return Math.max(0, Math.round((m * 60 + s) * 1000));
  } else if (parts.length === 1) {
    // 纯秒数
    const s = parseFloat(parts[0]) || 0;
    return Math.max(0, Math.round(s * 1000));
  }

  return 0;
}

/**
 * 兼容别名：将时间戳字符串解析为毫秒数
 */
export function parseSrtTimestampToMs(timestampStr: string): number {
  return parseFlexibleTimecodeToMs(timestampStr);
}

/**
 * 解析 SRT 文本为 SubtitleEntry 数组
 */
export function parseSrt(srtText: string): SubtitleEntry[] {
  if (!srtText || !srtText.trim()) {
    return [];
  }

  const entries: SubtitleEntry[] = [];
  const normalizedText = srtText.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
  const rawBlocks = normalizedText.split(/\n\s*\n/);

  for (const block of rawBlocks) {
    const lines = block.trim().split('\n');
    if (lines.length < 2) continue;

    let timeLineIdx = 0;
    // 首行可能是数字序号
    if (/^\d+$/.test(lines[0].trim())) {
      timeLineIdx = 1;
    }

    if (lines.length <= timeLineIdx) continue;

    const timeLine = lines[timeLineIdx];
    const timeMatch = timeLine.match(/(\d{2}:\d{2}:\d{2}[,. ]\d{3})\s*-->\s*(\d{2}:\d{2}:\d{2}[,. ]\d{3})/);
    if (!timeMatch) continue;

    const startMs = parseFlexibleTimecodeToMs(timeMatch[1]);
    const endMs = parseFlexibleTimecodeToMs(timeMatch[2]);
    const textLines = lines.slice(timeLineIdx + 1);
    const text = textLines.join('\n').trim();

    entries.push({
      index: entries.length + 1,
      start_ms: startMs,
      end_ms: endMs,
      text,
      confidence: 1.0,
    });
  }

  return entries;
}

/**
 * 触发浏览器客户端直接下载单个 SRT 文件 (Blob URL)
 */
export function exportSrtFile(entries: SubtitleEntry[], baseFilename: string): void {
  const content = formatSrt(entries);
  const cleanName = (baseFilename || 'subtitles').replace(/\.[^/.]+$/, '');
  const filename = `${cleanName}.srt`;

  const blob = new Blob([content], { type: 'text/plain;charset=utf-8' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

export interface BatchSrtItem {
  name: string;
  entries: SubtitleEntry[];
}

/**
 * 将多个任务的字幕打包为单一 ZIP 归档并触发单次浏览器下载（杜绝连续触发多个下载的 hack）
 */
export function exportBatchZip(items: BatchSrtItem[], zipFilename: string = 'subtitles_batch.zip'): boolean {
  const validItems = items.filter((item) => item.entries && item.entries.length > 0);
  if (validItems.length === 0) {
    return false;
  }

  const zipFiles = validItems.map((item) => {
    const cleanName = (item.name || 'subtitles').replace(/\.[^/.]+$/, '');
    const srtText = formatSrt(item.entries);
    return {
      name: `${cleanName}.srt`,
      content: srtText,
    };
  });

  const zipBlob = createZipArchive(zipFiles);
  downloadZipBlob(zipBlob, zipFilename);
  return true;
}
