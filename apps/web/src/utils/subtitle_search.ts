import type { SubtitleEntry } from '../types/api';

/**
 * 带有缓存加速的高精字幕区间二分查找器 (10Hz 时钟高频匹配)
 */
export class SubtitleSearcher {
  private lastHitIndex: number = -1;

  /**
   * 在字幕列表中二分查找落在 [start_ms, end_ms] 的条目 index
   * @param entries 字幕列表
   * @param timeMs 当前播放时间 (毫秒)
   * @returns 匹配的字幕 entry index 或 null
   */
  public findActiveIndex(entries: readonly SubtitleEntry[], timeMs: number): number | null {
    const len = entries.length;
    if (len === 0 || timeMs < 0) {
      this.lastHitIndex = -1;
      return null;
    }

    // 1. 快速缓存加速 (单调顺序播放 O(1) 命中)
    if (this.lastHitIndex >= 0 && this.lastHitIndex < len) {
      const cached = entries[this.lastHitIndex];
      if (timeMs >= cached.start_ms && timeMs <= cached.end_ms) {
        return cached.index;
      }
      // 检查紧随其后的下一条
      const nextIdx = this.lastHitIndex + 1;
      if (nextIdx < len) {
        const nextEntry = entries[nextIdx];
        if (timeMs >= nextEntry.start_ms && timeMs <= nextEntry.end_ms) {
          this.lastHitIndex = nextIdx;
          return nextEntry.index;
        }
      }
    }

    // 2. 二分查找 O(log N)
    let left = 0;
    let right = len - 1;
    let matchedEntry: SubtitleEntry | null = null;
    let matchedArrayIdx = -1;

    while (left <= right) {
      const mid = (left + right) >>> 1;
      const entry = entries[mid];

      if (timeMs >= entry.start_ms && timeMs <= entry.end_ms) {
        matchedEntry = entry;
        matchedArrayIdx = mid;
        break;
      } else if (timeMs < entry.start_ms) {
        right = mid - 1;
      } else {
        left = mid + 1;
      }
    }

    if (matchedEntry !== null) {
      this.lastHitIndex = matchedArrayIdx;
      return matchedEntry.index;
    }

    return null;
  }

  public resetCache(): void {
    this.lastHitIndex = -1;
  }
}

export const globalSubtitleSearcher = new SubtitleSearcher();
