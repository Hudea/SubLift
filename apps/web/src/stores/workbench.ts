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
import {
  type ExtractionConfig,
  type SamplingQuality,
  type RoiPolicy,
  DEFAULT_BOTTOM_ROI,
  DEFAULT_CONFIDENCE_THRESHOLD,
  qualityToFps,
  fpsToQuality,
} from '../types/config';

export type WorkbenchState =
  | 'Empty'
  | 'Ready'
  | 'Processing'
  | 'Review'
  | 'Failed'
  | 'Cancelled';

export type DraftStatus = 'saved' | 'dirty' | 'saving' | 'failed';

export interface VersionedSubtitleDraft {
  version: 1;
  videoPath: string;
  jobId?: string | null;
  entries: SubtitleEntry[];
  savedAt: number;
}

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

  // 3. Extraction configuration (Harmonized with Feature 12513 truth source)
  const selectedEngine = ref<OcrEngineName>(systemStore.preferredEngine);
  const quality = ref<SamplingQuality>('fast');
  const targetFps = computed<number>({
    get: () => qualityToFps(quality.value),
    set: (v: number) => {
      quality.value = fpsToQuality(v);
    },
  });
  const confidenceThreshold = ref<number>(DEFAULT_CONFIDENCE_THRESHOLD);
  const roiPolicy = ref<RoiPolicy>('auto');
  const regionBox = ref<NormalizedRegionBox>({ ...DEFAULT_BOTTOM_ROI });
  const script = ref<string | undefined>(undefined);
  const activeConfigSnapshot = ref<Readonly<ExtractionConfig> | null>(null);

  // 3.5 Region Detection State (Feature 12509)
  const isDetectingRegion = ref<boolean>(false);
  const roiModifiedByUser = ref<boolean>(false);
  const roiDetectionFeedback = ref<string | null>(null);
  let detectionGeneration = 0;

  // 4. Progress and Live Subtitle Entries
  const progress = ref<SseProgressData>({
    stage: 'idle',
    pct: 0,
    eta_ms: 0,
  });
  const progressPct = computed(() => Math.round((progress.value.pct ?? 0) * 100));
  const entries = ref<SubtitleEntry[]>([]);
  const activeEntryIndex = ref<number | null>(null);

  const errorMessage = ref<string | null>(null);

  // 5. Versioned Draft State Machine & History Stack (Feature 12515)
  const draftStatus = ref<DraftStatus>('saved');
  const draftSavedAt = ref<number | null>(null);
  const draftError = ref<string | null>(null);
  const hasUserEdits = ref<boolean>(false);

  const undoStack = ref<SubtitleEntry[][]>([]);
  const redoStack = ref<SubtitleEntry[][]>([]);
  const maxHistorySize = 50;
  let autoSaveTimer: ReturnType<typeof setTimeout> | null = null;

  const canUndo = computed(() => undoStack.value.length > 0 && !isLocked.value);
  const canRedo = computed(() => redoStack.value.length > 0 && !isLocked.value);
  const hasDraft = computed(() => entries.value.length > 0 || hasUserEdits.value);

  // Persistence helpers
  function getDraftStorageKey(keyPath: string): string {
    return `sublift_draft:${keyPath.trim()}`;
  }

  function saveDraftToStorage(targetPath?: string): boolean {
    const path = (targetPath || videoPath.value || '').trim();
    if (!path) return false;

    draftStatus.value = 'saving';
    const payload: VersionedSubtitleDraft = {
      version: 1,
      videoPath: path,
      jobId: activeJobId.value,
      entries: JSON.parse(JSON.stringify(entries.value)),
      savedAt: Date.now(),
    };

    try {
      if (typeof localStorage !== 'undefined') {
        localStorage.setItem(getDraftStorageKey(path), JSON.stringify(payload));
      }
      draftStatus.value = 'saved';
      draftSavedAt.value = payload.savedAt;
      draftError.value = null;
      return true;
    } catch (err: any) {
      draftStatus.value = 'failed';
      draftError.value = err?.message || '草稿自动保存失败';
      return false;
    }
  }

  function loadDraftFromStorage(targetPath?: string): VersionedSubtitleDraft | null {
    const path = (targetPath || videoPath.value || '').trim();
    if (!path || typeof localStorage === 'undefined') return null;

    try {
      const raw = localStorage.getItem(getDraftStorageKey(path));
      if (!raw) return null;
      const data = JSON.parse(raw);
      if (data && data.version === 1 && Array.isArray(data.entries)) {
        return data as VersionedSubtitleDraft;
      }
    } catch {
      return null;
    }
    return null;
  }

  function clearDraftFromStorage(targetPath?: string): void {
    const path = (targetPath || videoPath.value || '').trim();
    if (!path || typeof localStorage === 'undefined') return;
    try {
      localStorage.removeItem(getDraftStorageKey(path));
    } catch {
      // Ignore errors
    }
  }

  function hasPersistedDraft(targetPath?: string): boolean {
    return !!loadDraftFromStorage(targetPath);
  }

  function scheduleAutoSave(debounceMs = 300): void {
    if (autoSaveTimer) {
      clearTimeout(autoSaveTimer);
      autoSaveTimer = null;
    }
    draftStatus.value = 'dirty';
    autoSaveTimer = setTimeout(() => {
      saveDraftToStorage();
    }, debounceMs);
  }

  function saveDraftNow(): boolean {
    if (autoSaveTimer) {
      clearTimeout(autoSaveTimer);
      autoSaveTimer = null;
    }
    return saveDraftToStorage();
  }

  function restoreDraft(targetPath?: string): boolean {
    const draft = loadDraftFromStorage(targetPath);
    if (!draft) return false;

    entries.value = JSON.parse(JSON.stringify(draft.entries));
    draftStatus.value = 'saved';
    draftSavedAt.value = draft.savedAt;
    draftError.value = null;
    hasUserEdits.value = true;
    clearHistory();
    globalSubtitleSearcher.resetCache();
    if (state.value === 'Ready' || state.value === 'Empty') {
      state.value = 'Review';
    }
    return true;
  }

  function discardDraft(targetPath?: string): void {
    clearDraftFromStorage(targetPath);
    entries.value = [];
    clearHistory();
    hasUserEdits.value = false;
    draftStatus.value = 'saved';
    draftSavedAt.value = null;
    draftError.value = null;
    globalSubtitleSearcher.resetCache();
    if (state.value === 'Review') {
      state.value = 'Ready';
    }
  }

  // History Stack operations
  function recordHistory(): void {
    const snapshot = JSON.parse(JSON.stringify(entries.value));
    undoStack.value.push(snapshot);
    if (undoStack.value.length > maxHistorySize) {
      undoStack.value.shift();
    }
    redoStack.value = [];
    hasUserEdits.value = true;
    scheduleAutoSave();
  }

  function undo(): boolean {
    if (!canUndo.value) return false;
    const currentSnapshot = JSON.parse(JSON.stringify(entries.value));
    redoStack.value.push(currentSnapshot);
    const prev = undoStack.value.pop()!;
    entries.value = prev;
    globalSubtitleSearcher.resetCache();
    scheduleAutoSave();
    return true;
  }

  function redo(): boolean {
    if (!canRedo.value) return false;
    const currentSnapshot = JSON.parse(JSON.stringify(entries.value));
    undoStack.value.push(currentSnapshot);
    const next = redoStack.value.pop()!;
    entries.value = next;
    globalSubtitleSearcher.resetCache();
    scheduleAutoSave();
    return true;
  }

  function clearHistory(): void {
    undoStack.value = [];
    redoStack.value = [];
  }

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
    activeJobId.value = null;
    errorMessage.value = null;
    roiDetectionFeedback.value = null;
    roiModifiedByUser.value = false;
    detectionGeneration++;
    resetDefaultBottomRoi();
    globalSubtitleSearcher.resetCache();
    clearHistory();

    // Check for saved draft for this video
    const draft = loadDraftFromStorage(videoPath.value);
    if (draft && draft.entries.length > 0) {
      entries.value = JSON.parse(JSON.stringify(draft.entries));
      draftStatus.value = 'saved';
      draftSavedAt.value = draft.savedAt;
      hasUserEdits.value = true;
      state.value = 'Review';
    } else {
      entries.value = [];
      draftStatus.value = 'saved';
      draftSavedAt.value = null;
      hasUserEdits.value = false;
      state.value = 'Ready';
    }

    if (
      systemStore.availableEngines.length > 0 &&
      !systemStore.availableEngines.some((e) => e.name === selectedEngine.value) &&
      selectedEngine.value !== 'mock'
    ) {
      selectedEngine.value = systemStore.preferredEngine;
    }

    // 载入视频后自动静默触发一次多点智能识别 (Feature 12509 方案 A)
    autoDetectSubtitleRegion({ silent: true });
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
    roiDetectionFeedback.value = null;
    roiModifiedByUser.value = false;
    detectionGeneration++;
    resetDefaultBottomRoi();
    globalSubtitleSearcher.resetCache();
    state.value = 'Ready';

    if (fingerprint) {
      pendingFingerprint = fingerprint;
      SubLiftApiClient.resolveVideoPath(fingerprint)
        .then((path) => {
          if (path && previewSrc.value === blobUrl) {
            videoPath.value = path;
            autoDetectSubtitleRegion({ silent: true });
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
    autoDetectSubtitleRegion({ silent: true });
  }

  /** 智能字幕区域自动识别 (Feature 12509) */
  async function autoDetectSubtitleRegion(options: { timeSec?: number; silent?: boolean } = {}) {
    if (isLocked.value || !videoPath.value) return;

    const currentGen = ++detectionGeneration;
    isDetectingRegion.value = true;
    roiDetectionFeedback.value = null;

    try {
      const res = await SubLiftApiClient.detectSubtitleRegion(
        videoPath.value,
        options.timeSec,
        selectedEngine.value
      );

      // 竞态防御 1：切视频或重置后丢弃迟到回包
      if (currentGen !== detectionGeneration) return;

      // 竞态防御 2：静默初识模式下，若用户已手动调整过选区，不强制覆盖
      if (options.silent && roiModifiedByUser.value) {
        return;
      }

      if (res.detected && res.suggested_box) {
        updateRegionBox(res.suggested_box);
        roiDetectionFeedback.value = res.preview_text
          ? `✨ 已识别字幕：“${res.preview_text}”`
          : '✨ 已自动吸附字幕区域';
      } else {
        if (!options.silent) {
          roiDetectionFeedback.value = '💡 当前画面未检测到明显字幕，已保留当前选区';
        }
      }
    } catch (err: any) {
      if (currentGen !== detectionGeneration) return;
      if (!options.silent) {
        roiDetectionFeedback.value = '识别失败，已保持当前选区';
      }
    } finally {
      if (currentGen === detectionGeneration) {
        isDetectingRegion.value = false;
      }
    }
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

  function getExtractionConfig(): ExtractionConfig {
    return {
      engine: selectedEngine.value,
      quality: quality.value,
      confidence_threshold: confidenceThreshold.value,
      roi_policy: roiPolicy.value,
      region_box: { ...regionBox.value },
      script: script.value,
    };
  }

  let cancelRequested = false;

  async function startExtraction(options?: { force?: boolean }) {
    if (!canStart.value) return;

    // 0. 审阅草稿覆盖防护（Feature 12515）：当存在已编辑条目或已落盘草稿时，必须显式确认 force: true
    const existingDraft = loadDraftFromStorage(videoPath.value);
    const hasExistingDraft =
      (existingDraft && existingDraft.entries.length > 0) ||
      entries.value.length > 0 ||
      hasUserEdits.value;

    if (hasExistingDraft && !options?.force && (state.value === 'Review' || hasUserEdits.value || draftStatus.value === 'dirty')) {
      const err = '当前存在已编辑的字幕审阅草稿，重新提取需要显式确认以避免覆盖草稿';
      errorMessage.value = err;
      throw new Error(err);
    }

    // 1. 严格 Fail-Closed 引擎可用性校验（杜绝静默回退/切换）
    const isEngineAvailable =
      selectedEngine.value === 'mock' ||
      (!systemStore.systemInfo ||
        systemStore.systemInfo.engines.some((e) => e.name === selectedEngine.value && e.available));

    if (!isEngineAvailable) {
      const err = `所选 OCR 引擎不可用: ${selectedEngine.value} (严格禁止静默回退)`;
      errorMessage.value = err;
      state.value = 'Failed';
      throw new Error(err);
    }

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

    // 2. 冻结不可变配置快照
    const frozenConfig: Readonly<ExtractionConfig> = Object.freeze({
      engine: selectedEngine.value,
      quality: quality.value,
      confidence_threshold: confidenceThreshold.value,
      roi_policy: roiPolicy.value,
      region_box: Object.freeze({ ...regionBox.value }),
      script: script.value,
    });
    activeConfigSnapshot.value = frozenConfig;

    // 清除既有草稿与历史记录（显式重新提取）
    clearDraftFromStorage(videoPath.value);
    clearHistory();
    hasUserEdits.value = false;
    draftStatus.value = 'saved';
    draftSavedAt.value = null;

    cancelRequested = false;
    state.value = 'Processing';
    entries.value = [];
    errorMessage.value = null;
    globalSubtitleSearcher.resetCache();
    progress.value = { stage: 'starting', pct: 0, eta_ms: 0 };

    try {
      const resp = await SubLiftApiClient.createJob({
        video_path: videoPath.value,
        engine: frozenConfig.engine,
        fps: qualityToFps(frozenConfig.quality),
        confidence_threshold: frozenConfig.confidence_threshold,
        region_box: frozenConfig.region_box ? { ...frozenConfig.region_box } : null,
        script: frozenConfig.script,
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
          // Ownership Separation & Late Event Isolation (Feature 12515):
          // 如果用户已进入 Review 状态或已发生用户手动编辑，丢弃迟到的 SSE 推送，绝不覆盖用户草稿
          if (state.value === 'Review' || hasUserEdits.value) {
            return;
          }

          if (data && data.entry) {
            const entry = data.entry;
            const exists = entries.value.some(
              (e) =>
                e.index === entry.index ||
                (e.start_ms === entry.start_ms && e.end_ms === entry.end_ms && e.text === entry.text)
            );
            if (!exists) {
              entries.value.push(entry);
            }
          }
        },
        onDone: (_data) => {
          state.value = 'Review';
          if (sseUnsubscribe) {
            sseUnsubscribe();
            sseUnsubscribe = null;
          }
          // 提取完成后自动固化初始版本草稿
          if (videoPath.value && entries.value.length > 0) {
            saveDraftToStorage();
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
      recordHistory();
      Object.assign(target, updated);
      if (updated.start_ms !== undefined || updated.end_ms !== undefined) {
        entries.value.sort((a, b) => a.start_ms - b.start_ms);
        globalSubtitleSearcher.resetCache();
      }
      scheduleAutoSave();
    }
  }

  function removeSubtitleEntry(index: number) {
    if (isLocked.value) return;
    const exists = entries.value.some((e) => e.index === index);
    if (exists) {
      recordHistory();
      entries.value = entries.value.filter((e) => e.index !== index);
      globalSubtitleSearcher.resetCache();
      if (activeEntryIndex.value === index) {
        activeEntryIndex.value = null;
      }
      scheduleAutoSave();
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

    recordHistory();
    if (idx >= 0) {
      entries.value.splice(idx + 1, 0, newEntry);
    } else {
      entries.value.push(newEntry);
    }
    entries.value.sort((a, b) => a.start_ms - b.start_ms);
    globalSubtitleSearcher.resetCache();
    scheduleAutoSave();
    return newEntry;
  }

  function mergeWithNext(targetIndex: number) {
    if (isLocked.value) return;
    const idx = entries.value.findIndex((e) => e.index === targetIndex);
    if (idx < 0 || idx >= entries.value.length - 1) return;
    const cur = entries.value[idx];
    const next = entries.value[idx + 1];

    recordHistory();
    cur.end_ms = Math.max(cur.end_ms, next.end_ms);
    cur.text = `${cur.text.trim()} ${next.text.trim()}`;
    cur.confidence = Math.round(((cur.confidence + next.confidence) / 2) * 100) / 100;

    entries.value.splice(idx + 1, 1);
    globalSubtitleSearcher.resetCache();
    scheduleAutoSave();
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
    if (autoSaveTimer) {
      clearTimeout(autoSaveTimer);
      autoSaveTimer = null;
    }
    releasePreviewSrc();
    state.value = 'Empty';
    videoPath.value = '';
    videoName.value = '';
    entries.value = [];
    activeJobId.value = null;
    currentTimeMs.value = 0;
    errorMessage.value = null;
    roiDetectionFeedback.value = null;
    isDetectingRegion.value = false;
    roiModifiedByUser.value = false;
    selectedEngine.value = systemStore.preferredEngine;
    quality.value = 'fast';
    confidenceThreshold.value = DEFAULT_CONFIDENCE_THRESHOLD;
    roiPolicy.value = 'auto';
    activeConfigSnapshot.value = null;
    detectionGeneration++;
    resetDefaultBottomRoi();
    globalSubtitleSearcher.resetCache();

    // Reset draft and history state
    clearHistory();
    hasUserEdits.value = false;
    draftStatus.value = 'saved';
    draftSavedAt.value = null;
    draftError.value = null;
  }

  return {
    state,
    videoPath,
    videoName,
    previewSrc,
    needsServerPath,
    currentTimeMs,
    selectedEngine,
    quality,
    targetFps,
    confidenceThreshold,
    roiPolicy,
    regionBox,
    script,
    activeConfigSnapshot,
    isDetectingRegion,
    roiModifiedByUser,
    roiDetectionFeedback,
    progress,
    progressPct,
    entries,
    activeEntryIndex,
    errorMessage,
    isLocked,
    canStart,
    canExport,
    // Draft & History exports (Feature 12515)
    draftStatus,
    draftSavedAt,
    draftError,
    hasUserEdits,
    hasDraft,
    canUndo,
    canRedo,
    undoStack,
    redoStack,
    getDraftStorageKey,
    saveDraftToStorage,
    loadDraftFromStorage,
    clearDraftFromStorage,
    hasPersistedDraft,
    saveDraftNow,
    restoreDraft,
    discardDraft,
    undo,
    redo,
    clearHistory,
    // Core actions
    loadVideo,
    loadVideoBlob,
    attachServerPath,
    updateRegionBox,
    resetDefaultBottomRoi,
    autoDetectSubtitleRegion,
    updatePlaybackTime,
    getExtractionConfig,
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
