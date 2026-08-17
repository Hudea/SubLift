import { defineStore } from 'pinia';
import { ref, computed } from 'vue';
import { SubLiftApiClient } from '../api/client';
import { useSystemStore } from './system';
import { globalSubtitleSearcher } from '../utils/subtitle_search';
import { exportSrtFile } from '../utils/srt_formatter';
import type {
  OcrEngineName,
  NormalizedRegionBox,
  SubtitleEntry,
  SseProgressData,
  FileFingerprintDTO,
} from '../types/api';

export type WorkbenchState =
  | 'Empty'
  | 'Ready'
  | 'Processing'
  | 'Review'
  | 'Failed'
  | 'Cancelled';

export const useWorkbenchStore = defineStore('workbench', () => {
  const systemStore = useSystemStore();

  // 1. State machine & Lifecycle
  const state = ref<WorkbenchState>('Empty');
  const activeJobId = ref<string | null>(null);
  let sseUnsubscribe: (() => void) | null = null;

  // 2. Video source info
  const videoPath = ref<string>('');
  const videoName = ref<string>('');
  const currentTimeMs = ref<number>(0);
  /** blob 预览源（Feature 12507）：拖入/点选的本地文件直接由浏览器播放；
   *  非空时播放器使用它，videoPath 仅在提取时需要。 */
  const previewSrc = ref<string>('');
  let pendingFingerprint: FileFingerprintDTO | null = null;

  // 3. Extraction configuration
  const selectedEngine = ref<OcrEngineName>('vision');
  const targetFps = ref<number>(5.0);
  const confidenceThreshold = ref<number>(0.0);
  const regionBox = ref<NormalizedRegionBox>({
    x: 0.0,
    y: 0.7,
    width: 1.0,
    height: 0.3,
  });

  // 4. Progress and Live Subtitle Entries
  const progress = ref<SseProgressData>({
    stage: 'idle',
    pct: 0,
    eta_ms: 0,
  });
  const entries = ref<SubtitleEntry[]>([]);
  const activeEntryIndex = ref<number | null>(null);

  const errorMessage = ref<string | null>(null);

  // Computed state locks
  const isLocked = computed(() => state.value === 'Processing');
  const canStart = computed(
    () =>
      (state.value === 'Ready' ||
        state.value === 'Review' ||
        state.value === 'Failed' ||
        state.value === 'Cancelled') &&
      (!!videoPath.value || !!previewSrc.value) &&
      systemStore.isFfmpegReady
  );
  const canExport = computed(() => entries.value.length > 0);
  /** 预览就绪但服务端路径未定位（等待反查/用户粘贴） */
  const needsServerPath = computed(() => !videoPath.value && !!previewSrc.value);

  function releasePreviewSrc() {
    if (previewSrc.value) {
      URL.revokeObjectURL(previewSrc.value);
      previewSrc.value = '';
    }
    pendingFingerprint = null;
  }

  // Actions
  function loadVideo(path: string, name?: string) {
    if (isLocked.value) return;

    releasePreviewSrc();
    videoPath.value = path.trim();
    videoName.value = name || videoPath.value.split(/[/\\]/).pop() || 'video.mp4';
    entries.value = [];
    activeJobId.value = null;
    errorMessage.value = null;
    globalSubtitleSearcher.resetCache();
    state.value = 'Ready';
  }

  /**
   * 本地文件 blob 预览（Feature 12507）：立即零拷贝播放，
   * 同时异步反查服务端路径，命中则静默附加（提取零摩擦）。
   */
  function loadVideoBlob(blobUrl: string, name: string, fingerprint?: FileFingerprintDTO) {
    if (isLocked.value) return;

    releasePreviewSrc();
    previewSrc.value = blobUrl;
    videoPath.value = '';
    videoName.value = name;
    entries.value = [];
    activeJobId.value = null;
    errorMessage.value = null;
    globalSubtitleSearcher.resetCache();
    state.value = 'Ready';

    if (fingerprint) {
      pendingFingerprint = fingerprint;
      SubLiftApiClient.resolveVideoPath(fingerprint)
        .then((path) => {
          if (path && previewSrc.value === blobUrl) {
            videoPath.value = path;
          }
        })
        .catch(() => {
          // 反查失败保持无路径状态；UI 会引导粘贴路径
        });
    }
  }

  /** 为 blob 预览补挂服务端路径（反查命中或用户粘贴） */
  function attachServerPath(path: string) {
    if (isLocked.value) return;
    const clean = path.trim();
    if (!clean) return;
    videoPath.value = clean;
    errorMessage.value = null;
  }

  function updateRegionBox(box: Partial<NormalizedRegionBox>) {
    if (isLocked.value) return;
    regionBox.value = {
      x: box.x !== undefined ? Number(box.x.toFixed(4)) : regionBox.value.x,
      y: box.y !== undefined ? Number(box.y.toFixed(4)) : regionBox.value.y,
      width: box.width !== undefined ? Number(box.width.toFixed(4)) : regionBox.value.width,
      height: box.height !== undefined ? Number(box.height.toFixed(4)) : regionBox.value.height,
    };
  }

  function resetDefaultBottomRoi() {
    updateRegionBox({ x: 0.0, y: 0.7, width: 1.0, height: 0.3 });
  }

  function updatePlaybackTime(timeMs: number) {
    currentTimeMs.value = timeMs;
    // 毫秒级二分快速命中当前活跃发音行
    activeEntryIndex.value = globalSubtitleSearcher.findActiveIndex(entries.value, timeMs);
  }

  let cancelRequested = false;

  async function startExtraction() {
    if (!canStart.value) return;

    // blob 预览模式下先确保拿到服务端路径：优先用挂载的路径，
    // 其次用拖入时的指纹反查；都没有则明确引导，绝不拿空路径发起任务。
    if (!videoPath.value) {
      let resolved: string | null = null;
      if (pendingFingerprint) {
        try {
          resolved = await SubLiftApiClient.resolveVideoPath(pendingFingerprint);
        } catch {
          resolved = null;
        }
      }
      if (!resolved) {
        errorMessage.value =
          '尚未定位到该视频的服务端路径：请在左侧「服务端路径」输入框粘贴绝对路径后重试。';
        return;
      }
      videoPath.value = resolved;
    }

    cancelRequested = false;
    state.value = 'Processing';
    entries.value = [];
    errorMessage.value = null;
    globalSubtitleSearcher.resetCache();
    progress.value = { stage: 'starting', pct: 0, eta_ms: 0 };

    try {
      const resp = await SubLiftApiClient.createJob({
        video_path: videoPath.value,
        engine: selectedEngine.value,
        fps: targetFps.value,
        confidence_threshold: confidenceThreshold.value,
        region_box: regionBox.value,
      });

      // 取消窗口防御：createJob 往返期间用户点了取消（当时还没有 job id），
      // 任务一旦建立立即在服务端取消，不让它成为孤儿任务。
      if (cancelRequested) {
        await SubLiftApiClient.cancelJob(resp.job_id).catch(() => {});
        state.value = 'Cancelled';
        errorMessage.value = '已取消提取';
        return;
      }

      activeJobId.value = resp.job_id;

      // 订阅 SSE 实时流
      sseUnsubscribe = SubLiftApiClient.subscribeJobEvents(resp.job_id, {
        onProgress: (data) => {
          progress.value = data;
        },
        onPushEntry: (data) => {
          entries.value.push(data.entry);
        },
        onDone: (_data) => {
          state.value = 'Review';
          if (sseUnsubscribe) {
            sseUnsubscribe();
            sseUnsubscribe = null;
          }
        },
        onError: (err) => {
          console.error('[Job Error]', err);
          errorMessage.value = String(err);
          state.value = 'Failed';
          if (sseUnsubscribe) {
            sseUnsubscribe();
            sseUnsubscribe = null;
          }
        },
        onCancelled: (reason) => {
          errorMessage.value = reason || '任务已被取消';
          state.value = 'Cancelled';
          if (sseUnsubscribe) {
            sseUnsubscribe();
            sseUnsubscribe = null;
          }
        },
      });
    } catch (err: unknown) {
      console.error('[Start Job Failed]', err);
      errorMessage.value = err instanceof Error ? err.message : String(err);
      state.value = 'Failed';
      throw err;
    }
  }

  async function cancelExtraction() {
    if (state.value !== 'Processing') return;

    // createJob 尚未返回（还没有 job id）：标记意图，建立后立即取消
    if (!activeJobId.value) {
      cancelRequested = true;
      errorMessage.value = '正在取消…';
      return;
    }

    try {
      await SubLiftApiClient.cancelJob(activeJobId.value);
    } finally {
      if (sseUnsubscribe) {
        sseUnsubscribe();
        sseUnsubscribe = null;
      }
      errorMessage.value = '已取消提取';
      state.value = 'Cancelled';
    }
  }

  function updateSubtitleEntry(index: number, updated: Partial<SubtitleEntry>) {
    if (isLocked.value) return;
    const target = entries.value.find((e) => e.index === index);
    if (target) {
      Object.assign(target, updated);
      if (updated.start_ms !== undefined || updated.end_ms !== undefined) {
        entries.value.sort((a, b) => a.start_ms - b.start_ms);
        globalSubtitleSearcher.resetCache();
      }
    }
  }

  function removeSubtitleEntry(index: number) {
    if (isLocked.value) return;
    entries.value = entries.value.filter((e) => e.index !== index);
    globalSubtitleSearcher.resetCache();
    if (activeEntryIndex.value === index) {
      activeEntryIndex.value = null;
    }
  }

  function insertEntryAfter(targetIndex: number) {
    if (isLocked.value) return null;
    const idx = entries.value.findIndex((e) => e.index === targetIndex);
    const prev = idx >= 0 ? entries.value[idx] : null;
    const startMs = prev ? prev.end_ms + 100 : currentTimeMs.value;
    const endMs = startMs + 2000;
    const nextSeq = entries.value.length > 0 ? Math.max(...entries.value.map((e) => e.index)) + 1 : 1;

    const newEntry: SubtitleEntry = {
      index: nextSeq,
      start_ms: startMs,
      end_ms: endMs,
      text: '新字幕条目',
      confidence: 1.0,
    };

    if (idx >= 0) {
      entries.value.splice(idx + 1, 0, newEntry);
    } else {
      entries.value.push(newEntry);
    }
    entries.value.sort((a, b) => a.start_ms - b.start_ms);
    globalSubtitleSearcher.resetCache();
    return newEntry;
  }

  function mergeWithNext(targetIndex: number) {
    if (isLocked.value) return;
    const idx = entries.value.findIndex((e) => e.index === targetIndex);
    if (idx < 0 || idx >= entries.value.length - 1) return;
    const cur = entries.value[idx];
    const next = entries.value[idx + 1];

    cur.end_ms = Math.max(cur.end_ms, next.end_ms);
    cur.text = `${cur.text.trim()} ${next.text.trim()}`;
    cur.confidence = Math.round(((cur.confidence + next.confidence) / 2) * 100) / 100;

    entries.value.splice(idx + 1, 1);
    globalSubtitleSearcher.resetCache();
  }

  function downloadSrt() {
    const baseName = videoName.value ? videoName.value.replace(/\.[^/.]+$/, '') : 'subtitles';
    exportSrtFile(entries.value, baseName);
  }

  function reset() {
    if (isLocked.value) return;
    if (sseUnsubscribe) {
      sseUnsubscribe();
      sseUnsubscribe = null;
    }
    releasePreviewSrc();
    state.value = 'Empty';
    videoPath.value = '';
    videoName.value = '';
    entries.value = [];
    activeJobId.value = null;
    currentTimeMs.value = 0;
    globalSubtitleSearcher.resetCache();
  }

  return {
    state,
    videoPath,
    videoName,
    previewSrc,
    needsServerPath,
    currentTimeMs,
    selectedEngine,
    targetFps,
    confidenceThreshold,
    regionBox,
    progress,
    entries,
    activeEntryIndex,
    errorMessage,
    isLocked,
    canStart,
    canExport,
    loadVideo,
    loadVideoBlob,
    attachServerPath,
    updateRegionBox,
    resetDefaultBottomRoi,
    updatePlaybackTime,
    startExtraction,
    cancelExtraction,
    updateSubtitleEntry,
    removeSubtitleEntry,
    insertEntryAfter,
    mergeWithNext,
    downloadSrt,
    reset,
  };
});
