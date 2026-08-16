import { defineStore } from 'pinia';
import { ref, computed } from 'vue';
import { SubLiftApiClient } from '../api/client';
import { useSystemStore } from './system';
import type {
  OcrEngineName,
  NormalizedRegionBox,
  SubtitleEntry,
  SseProgressData,
} from '../types/api';

export type WorkbenchState = 'Empty' | 'Ready' | 'Processing' | 'Review';

export const useWorkbenchStore = defineStore('workbench', () => {
  const systemStore = useSystemStore();

  // 1. State machine
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
  const canStart = computed(() => state.value === 'Ready' && !!videoPath.value && systemStore.isFfmpegReady);
  const canExport = computed(() => entries.value.length > 0);

  function initEngine() {
    if (systemStore.preferredEngine) {
      selectedEngine.value = systemStore.preferredEngine;
    }
  }

  function loadVideo(path: string, name?: string) {
    if (isLocked.value) return;
    videoPath.value = path.trim();
    videoName.value = name || videoPath.value.split(/[/\\]/).pop() || 'video.mp4';
    entries.value = [];
    activeJobId.value = null;
    initEngine();
    state.value = 'Ready';
  }

  function updateRegionBox(box: NormalizedRegionBox) {
    if (isLocked.value) return;
    regionBox.value = { ...box };
  }

  function resetDefaultBottomRoi() {
    updateRegionBox({ x: 0.0, y: 0.7, width: 1.0, height: 0.3 });
  }

  function updatePlaybackTime(timeMs: number) {
    currentTimeMs.value = timeMs;
    const matched = entries.value.find((e) => timeMs >= e.start_ms && timeMs <= e.end_ms);
    activeEntryIndex.value = matched ? matched.index : null;
  }

  async function startExtraction() {
    if (!canStart.value) return;

    state.value = 'Processing';
    entries.value = [];
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
    } catch (err: any) {
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
    }
  }

  function removeSubtitleEntry(index: number) {
    entries.value = entries.value.filter((e) => e.index !== index);
  }

  async function downloadSrt() {
    if (activeJobId.value && state.value === 'Review') {
      try {
        const srtContent = await SubLiftApiClient.exportSrt(activeJobId.value);
        triggerDownload(srtContent, `${videoName.value.replace(/\.[^/.]+$/, '')}.srt`);
        return;
      } catch {
        // Fallback to local entries formatting
      }
    }

    if (entries.value.length === 0) return;

    const formatMs = (ms: number) => {
      const h = Math.floor(ms / 3600000);
      const m = Math.floor((ms % 3600000) / 60000);
      const s = Math.floor((ms % 60000) / 1000);
      const mil = ms % 1000;
      const pad = (n: number, w = 2) => String(n).padStart(w, '0');
      return `${pad(h)}:${pad(m)}:${pad(s)},${pad(mil, 3)}`;
    };

    let srtText = '';
    entries.value.forEach((e, i) => {
      srtText += `${i + 1}\n${formatMs(e.start_ms)} --> ${formatMs(e.end_ms)}\n${e.text}\n\n`;
    });

    triggerDownload(srtText, `${videoName.value.replace(/\.[^/.]+$/, '')}.srt`);
  }

  function triggerDownload(content: string, filename: string) {
    const blob = new Blob([content], { type: 'text/plain;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
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
    downloadSrt,
    reset,
  };
});
