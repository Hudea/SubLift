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
} from '../types/api';

export type WorkbenchState = 'Empty' | 'Ready' | 'Processing' | 'Review';

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

  // 3. Extraction configuration
  const selectedEngine = ref<OcrEngineName>('paddle');
  const targetFps = ref<number>(2.0);
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

  // Computed state locks
  const isLocked = computed(() => state.value === 'Processing');
  const canStart = computed(
    () => (state.value === 'Ready' || state.value === 'Review') && !!videoPath.value && systemStore.isFfmpegReady
  );
  const canExport = computed(() => entries.value.length > 0);

  // Actions
  function loadVideo(path: string, name?: string) {
    if (isLocked.value) return;

    videoPath.value = path.trim();
    videoName.value = name || videoPath.value.split(/[/\\]/).pop() || 'video.mp4';
    entries.value = [];
    activeJobId.value = null;
    globalSubtitleSearcher.resetCache();
    state.value = 'Ready';
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

  async function startExtraction() {
    if (!canStart.value) return;

    state.value = 'Processing';
    entries.value = [];
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
          state.value = 'Ready';
          if (sseUnsubscribe) {
            sseUnsubscribe();
            sseUnsubscribe = null;
          }
        },
      });
    } catch (err: unknown) {
      console.error('[Start Job Failed]', err);
      state.value = 'Ready';
      throw err;
    }
  }

  async function cancelExtraction() {
    if (state.value !== 'Processing' || !activeJobId.value) return;

    try {
      await SubLiftApiClient.cancelJob(activeJobId.value);
    } finally {
      if (sseUnsubscribe) {
        sseUnsubscribe();
        sseUnsubscribe = null;
      }
      state.value = 'Ready';
    }
  }

  function updateSubtitleEntry(index: number, updated: Partial<SubtitleEntry>) {
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
    entries.value = entries.value.filter((e) => e.index !== index);
    globalSubtitleSearcher.resetCache();
    if (activeEntryIndex.value === index) {
      activeEntryIndex.value = null;
    }
  }

  function insertEntryAfter(targetIndex: number) {
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
    currentTimeMs,
    selectedEngine,
    targetFps,
    confidenceThreshold,
    regionBox,
    progress,
    entries,
    activeEntryIndex,
    isLocked,
    canStart,
    canExport,
    loadVideo,
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
