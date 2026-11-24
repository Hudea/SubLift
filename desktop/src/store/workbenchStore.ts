import { create } from 'zustand';
import { createHistory } from '../core/history';
import {
  clampSourceBox,
  createDefaultBandBox,
  DEFAULT_AUTO_BOTTOM_RATIO,
  DEFAULT_BAND_BOTTOM_RATIO,
  scaleSourceBox,
  toBandSourceBox,
} from '../core/roi';
import {
  filterNonBlankEntries,
  toProofreadCues,
  toSubtitleEntries,
} from '../core/subtitle';
import { clampMsToRange } from '../core/timecode';
import { createTauriBridge, parseIpcError } from './bridge';
import { createProofreadActions } from './proofreadSlice';
import { createProviderActions, resolveDefaultProviderId } from './providerSlice';
import { selectActiveCommands } from './selectors';
import type {
  ApplyConfigOptions,
  AutoRegionInfoState,
  BatchExportConflictPolicy,
  BatchExportItemInput,
  BatchExportReport,
  ConfigSnapshot,
  IpcBridge,
  OcrEngineId,
  QueueTaskItem,
  RoiMode,
  SessionDraftDto,
  SourceBox,
  StartExtractionParams,
  SubtitleProfileDto,
  SubtitleScript,
  TaskEvent,
  ThemeMode,
  ToastItem,
  WorkbenchEngine,
  WorkbenchStore,
} from './types';

function extractFileName(filePath: string): string {
  const parts = filePath.split(/[/\\]/);
  return parts[parts.length - 1] || filePath;
}

function normalizeVideoPath(filePath: string): string {
  return filePath.trim();
}

function deriveSiblingSrtPath(videoPath: string): string {
  const lastDot = videoPath.lastIndexOf('.');
  const lastSep = Math.max(videoPath.lastIndexOf('/'), videoPath.lastIndexOf('\\'));
  const stem = lastDot > lastSep ? videoPath.slice(0, lastDot) : videoPath;
  return `${stem}.srt`;
}

function deriveSrtFileName(fileName: string): string {
  const lastDot = fileName.lastIndexOf('.');
  const stem = lastDot > 0 ? fileName.slice(0, lastDot) : fileName;
  return `${stem}.srt`;
}

let toastSeq = 0;
let taskSeq = 0;

function nextTaskId(): string {
  taskSeq += 1;
  return `task_${taskSeq}`;
}

function nextToastId(): string {
  toastSeq += 1;
  return `toast_${toastSeq}`;
}

function createInitialTaskItem(
  videoPath: string,
  defaultEngine: WorkbenchEngine,
  defaultProviderId: string | null
): QueueTaskItem {
  return {
    id: nextTaskId(),
    videoPath,
    fileName: extractFileName(videoPath),
    probe: null,
    probeLoading: true,
    probeError: null,
    capabilityWarning: null,
    engine: defaultEngine,
    providerId: defaultProviderId,
    script: 'auto',
    sampleFps: 5.0,
    roiMode: 'auto',
    regionBox: null,
    roiScaledFromQueue: false,
    pendingScaledRoi: null,
    autoRegionInfo: null,
    status: 'idle',
    subPhase: null,
    progress: null,
    activeRunConfig: null,
    error: null,
    metrics: null,
    failedSlotCount: null,
    cues: [],
    streamingPreviewCues: [],
    history: createHistory([]),
    activeCueId: null,
    selectedCueIds: [],
    previewTimestampMs: 0,
    playbackMode: 'still',
    streamBaseMs: 0,
    isAutoDetecting: false,
    isReRecognizing: false,
    isCancelling: false,
  };
}

export function createWorkbenchStore(bridge: IpcBridge) {
  // 每项独立代际令牌，确保切换 activeTaskId 绝不丢弃后台流式事件
  const runGenMap = new Map<string, number>();
  const activeRunTaskIds = new Map<string, string>();
  const preDetectTaskIds = new Set<string>();
  const probeGenMap = new Map<string, number>();
  const autoRegionGenMap = new Map<string, number>();
  const draftTimers = new Map<string, ReturnType<typeof setTimeout>>();
  const userTouchedTasks = new Set<string>();
  let queueRunGen = 0;

  return create<WorkbenchStore>((set, get) => {
    const updateTask = (
      taskId: string,
      updater: (item: QueueTaskItem) => QueueTaskItem
    ) => {
      set((state) => ({
        tasks: state.tasks.map((t) => (t.id === taskId ? updater(t) : t)),
      }));
    };

    const pushToast = (toast: Omit<ToastItem, 'id'>): string => {
      const id = nextToastId();
      set((state) => ({
        toasts: [...state.toasts.slice(-4), { ...toast, id }],
      }));
      return id;
    };

    const stopPlaybackIfPlaying = (taskId: string) => {
      const item = get().tasks.find((t) => t.id === taskId);
      if (item && item.playbackMode === 'playing') {
        updateTask(taskId, (t) => ({ ...t, playbackMode: 'still' }));
        void get().bridge.stopMediaStream().catch(() => {});
      }
    };

    const flushDraftSave = async (taskId: string): Promise<void> => {
      const existingTimer = draftTimers.get(taskId);
      if (existingTimer !== undefined) {
        clearTimeout(existingTimer);
        draftTimers.delete(taskId);
      }
      const item = get().tasks.find((t) => t.id === taskId);
      if (!item || item.status === 'extracting') {
        return;
      }
      const draft: SessionDraftDto = {
        video_path: item.videoPath,
        engine: item.engine,
        provider_id: item.providerId,
        script: item.script,
        sample_fps: item.sampleFps,
        roi_mode: item.roiMode,
        region_box: item.regionBox,
        status: item.status,
        entries: toSubtitleEntries(item.cues),
        updated_at_ms: Date.now(),
      };
      try {
        await get().bridge.saveSessionDraft(draft);
      } catch {
        // 草稿同步失败不阻塞主编辑流程
      }
    };

    const scheduleDraftSave = (taskId: string) => {
      const prev = draftTimers.get(taskId);
      if (prev !== undefined) {
        clearTimeout(prev);
      }
      const timer = setTimeout(() => {
        draftTimers.delete(taskId);
        void flushDraftSave(taskId);
      }, 500);
      draftTimers.set(taskId, timer);
    };

    const resolveLocalDetectEngine = (): OcrEngineId => {
      const engines = get().capabilities?.engines ?? ['paddle'];
      if (engines.includes('paddle')) {
        return 'paddle';
      }
      return engines[0] ?? 'paddle';
    };

    const providerActions = createProviderActions(set, get, {
      scheduleDraftSave,
    });
    const proofreadActions = createProofreadActions(set, get, {
      scheduleDraftSave,
      stopPlaybackIfPlaying,
    });

    const probeAndHydrateItem = async (taskId: string, videoPath: string) => {
      const gen = (probeGenMap.get(taskId) ?? 0) + 1;
      probeGenMap.set(taskId, gen);

      try {
        const probe = await get().bridge.probeVideo(videoPath);
        if (probeGenMap.get(taskId) !== gen) {
          return;
        }
        const defaultBand = probe.display_transform_ok
          ? createDefaultBandBox(probe.width, probe.height, DEFAULT_BAND_BOTTOM_RATIO)
          : null;

        updateTask(taskId, (item) => {
          let nextRegion = item.regionBox ?? defaultBand;
          let nextScaled = item.roiScaledFromQueue;
          if (item.pendingScaledRoi && probe.display_transform_ok) {
            const scaled = scaleSourceBox(
              item.pendingScaledRoi.box,
              item.pendingScaledRoi.fromW,
              item.pendingScaledRoi.fromH,
              probe.width,
              probe.height
            );
            if (scaled) {
              nextRegion = scaled;
              nextScaled =
                item.pendingScaledRoi.fromW !== probe.width ||
                item.pendingScaledRoi.fromH !== probe.height;
            }
          }
          return {
            ...item,
            probe,
            probeLoading: false,
            probeError: null,
            regionBox: nextRegion,
            roiScaledFromQueue: nextScaled,
            pendingScaledRoi: null,
          };
        });

        // 恢复 AppData 草稿（含 apple_vision 不支持时的显式警告保护）
        try {
          const draft = await get().bridge.loadSessionDraft(videoPath);
          if (probeGenMap.get(taskId) !== gen || !draft) {
            return;
          }
          const current = get().tasks.find((t) => t.id === taskId);
          if (
            !current ||
            userTouchedTasks.has(taskId) ||
            runGenMap.has(taskId) ||
            current.status !== 'idle'
          ) {
            return;
          }
          const caps = get().capabilities;
          const appleVisionUnsupported =
            draft.engine === 'apple_vision' &&
            caps !== null &&
            !caps.engines.includes('apple_vision');

          const providers = get().providers;
          const defaultProviderId = resolveDefaultProviderId(providers);
          const reconciledProviderId = get().initialized
            ? defaultProviderId ??
              (draft.provider_id && providers.some((p) => p.id === draft.provider_id)
                ? draft.provider_id
                : null)
            : current.providerId ?? draft.provider_id ?? null;

          const cues = toProofreadCues(filterNonBlankEntries(draft.entries));
          updateTask(taskId, (item) => {
            const nextBox =
              draft.region_box && probe.display_transform_ok
                ? clampSourceBox(draft.region_box, probe.width, probe.height)
                : item.regionBox;
            const restoredRunConfig: ConfigSnapshot | null =
              draft.status !== 'idle'
                ? {
                    engine: draft.engine,
                    providerId: reconciledProviderId,
                    script: draft.script,
                    sampleFps: draft.sample_fps,
                    roiMode: draft.roi_mode,
                    regionBox: nextBox ? { ...nextBox } : null,
                  }
                : item.activeRunConfig;
            return {
              ...item,
              engine: draft.engine,
              providerId: reconciledProviderId,
              script: draft.script,
              sampleFps: draft.sample_fps,
              roiMode: draft.roi_mode,
              regionBox: nextBox,
              activeRunConfig: restoredRunConfig,
              status: draft.status,
              cues,
              history: createHistory(cues),
              activeCueId: cues[0]?.local_uid ?? null,
              capabilityWarning: appleVisionUnsupported
                ? '当前环境不支持 Apple Vision，请切换为 PaddleOCR 或 VLM 后再启动提取'
                : null,
            };
          });
        } catch {
          // 草稿损坏或不存在时保持默认已探测态
        }
      } catch (err) {
        if (probeGenMap.get(taskId) !== gen) {
          return;
        }
        const parsed = parseIpcError(err);
        updateTask(taskId, (item) => ({
          ...item,
          probeLoading: false,
          probeError: parsed.message,
        }));
        pushToast({
          severity: 'error',
          title: `探测视频「${extractFileName(videoPath)}」失败`,
          reason: parsed.message,
          nextStep: '请确认文件格式完整且未被占用',
        });
      }
    };

    const runSingleTaskExtraction = async (taskId: string): Promise<void> => {
      const state = get();
      const item = state.tasks.find((t) => t.id === taskId);
      if (!item || item.status === 'extracting' || item.isReRecognizing) {
        return;
      }
      if (!item.probe || !item.probe.display_transform_ok) {
        pushToast({
          severity: 'warning',
          title: '无法启动提取',
          reason:
            item.probe?.transform_note ?? '视频尚未探测完成或显示变换异常',
        });
        return;
      }
      if (
        item.engine === 'apple_vision' &&
        state.capabilities &&
        !state.capabilities.engines.includes('apple_vision')
      ) {
        pushToast({
          severity: 'warning',
          title: '当前环境不支持 Apple Vision',
          reason: item.capabilityWarning ?? '当前运行平台未启用 Apple Vision 引擎',
          nextStep: '请先切换为 PaddleOCR 或 VLM 后再启动提取',
        });
        return;
      }

      const matchedProvider = item.providerId
        ? state.providers.find((p) => p.id === item.providerId)
        : undefined;
      const effectiveProviderId =
        item.engine === 'vlm'
          ? matchedProvider?.id ?? resolveDefaultProviderId(state.providers)
          : matchedProvider?.id ?? item.providerId;

      if (item.engine === 'vlm' && !effectiveProviderId) {
        pushToast({
          severity: 'warning',
          title: '请先配置 VLM 服务',
          reason: '当前已选择 VLM 引擎，但尚未存在可用 Provider 档案',
          nextStep: '请点击打开 VLM 设置完成配置',
          actionLabel: '打开 VLM 设置',
          actionType: 'open_vlm_settings',
        });
        return;
      }

      if (item.playbackMode === 'playing') {
        stopPlaybackIfPlaying(item.id);
      }

      userTouchedTasks.add(taskId);
      const gen = (runGenMap.get(taskId) ?? 0) + 1;
      runGenMap.set(taskId, gen);
      autoRegionGenMap.set(taskId, (autoRegionGenMap.get(taskId) ?? 0) + 1);
      const backendTaskId = `${taskId}:${gen}`;
      activeRunTaskIds.set(taskId, backendTaskId);

      const snapshot: ConfigSnapshot = {
        engine: item.engine,
        providerId: effectiveProviderId ?? null,
        script: item.script,
        sampleFps: item.sampleFps,
        roiMode: item.roiMode,
        regionBox: item.regionBox ? { ...item.regionBox } : null,
      };

      const cachedAutoValid =
        item.roiMode === 'auto' &&
        item.autoRegionInfo !== null &&
        item.autoRegionInfo.detected &&
        item.autoRegionInfo.script === item.script;
      const needsPreAutoDetect =
        item.roiMode === 'auto' && !cachedAutoValid;

      updateTask(taskId, (t) => ({
        ...t,
        status: 'extracting',
        subPhase: 'probing',
        activeRunConfig: snapshot,
        error: null,
        metrics: null,
        failedSlotCount: null,
        streamingPreviewCues: [],
        isAutoDetecting: needsPreAutoDetect,
        isCancelling: false,
        progress: {
          currentMs: 0,
          totalMs: t.probe?.duration_ms ?? 0,
          fps: 0,
          completedSheets: 0,
          totalSheets: 0,
          retriedSplits: 0,
          startedAtMs: Date.now(),
        },
      }));

      let effectiveRegionBox: SourceBox | null = snapshot.regionBox;
      let effectiveProfile: SubtitleProfileDto | null = cachedAutoValid
        ? (item.autoRegionInfo?.profile ?? null)
        : null;

      if (needsPreAutoDetect) {
        const supportedEngines = get().capabilities?.engines ?? ['paddle'];
        const detectEngine: OcrEngineId =
          item.engine !== 'vlm' && supportedEngines.includes(item.engine)
            ? item.engine
            : resolveLocalDetectEngine();
        const fallbackBand = createDefaultBandBox(
          item.probe.width,
          item.probe.height,
          DEFAULT_AUTO_BOTTOM_RATIO
        );

        const applyPreDetectOutcome = (
          nextBox: SourceBox | null,
          nextAutoInfo?: AutoRegionInfoState
        ) => {
          updateTask(taskId, (t) =>
            t.status !== 'extracting'
              ? t
              : {
                  ...t,
                  isAutoDetecting: false,
                  regionBox: nextBox,
                  roiScaledFromQueue: false,
                  activeRunConfig: {
                    ...snapshot,
                    regionBox: nextBox ? { ...nextBox } : null,
                  },
                  ...(nextAutoInfo ? { autoRegionInfo: nextAutoInfo } : {}),
                }
          );
        };

        preDetectTaskIds.add(taskId);
        try {
          const autoRes = await get().bridge.detectAutoRegion(
            item.videoPath,
            detectEngine,
            item.script,
            DEFAULT_AUTO_BOTTOM_RATIO
          );
          if (runGenMap.get(taskId) !== gen) {
            return;
          }
          if (autoRes.detected) {
            const clampedBox = clampSourceBox(
              autoRes.region_box,
              item.probe.width,
              item.probe.height
            );
            effectiveRegionBox = clampedBox ?? effectiveRegionBox ?? fallbackBand;
            effectiveProfile = autoRes.profile ?? null;
            applyPreDetectOutcome(effectiveRegionBox, {
              detected: true,
              confidence: autoRes.confidence,
              previewText: autoRes.preview_text,
              sampleTimestampMs: autoRes.sample_timestamp_ms,
              errorMessage: autoRes.error_message,
              profile: autoRes.profile ?? null,
              script: item.script,
            });
          } else {
            effectiveRegionBox = fallbackBand;
            effectiveProfile = null;
            applyPreDetectOutcome(fallbackBand, {
              detected: false,
              confidence: autoRes.confidence,
              previewText: autoRes.preview_text,
              sampleTimestampMs: autoRes.sample_timestamp_ms,
              errorMessage: autoRes.error_message,
              profile: null,
              script: item.script,
            });
          }
        } catch {
          if (runGenMap.get(taskId) !== gen) {
            return;
          }
          effectiveRegionBox = fallbackBand;
          effectiveProfile = null;
          applyPreDetectOutcome(fallbackBand);
        } finally {
          preDetectTaskIds.delete(taskId);
        }
      }

      if (runGenMap.get(taskId) !== gen) {
        return;
      }

      const params: StartExtractionParams = {
        video_path: item.videoPath,
        mode: snapshot.engine === 'vlm' ? 'pure_timing_strips' : 'local_ocr',
        engine: snapshot.engine === 'vlm' ? null : snapshot.engine,
        provider_id: snapshot.engine === 'vlm' ? snapshot.providerId : null,
        region_box: effectiveRegionBox,
        config: {
          sample_fps: snapshot.sampleFps,
          region_bottom_ratio:
            snapshot.roiMode === 'auto'
              ? DEFAULT_AUTO_BOTTOM_RATIO
              : DEFAULT_BAND_BOTTOM_RATIO,
          subtitle_script: snapshot.script,
          ...(effectiveProfile ? { predetected_profile: effectiveProfile } : {}),
        },
        script: snapshot.script,
      };

      let settledTerminal = false;

      const finalizeCompleted = (
        entries: QueueTaskItem['streamingPreviewCues'],
        metrics: QueueTaskItem['metrics'],
        failedSlotCount?: number
      ) => {
        if (settledTerminal || runGenMap.get(taskId) !== gen) {
          return;
        }
        settledTerminal = true;
        activeRunTaskIds.delete(taskId);
        const cues = toProofreadCues(filterNonBlankEntries(entries));
        updateTask(taskId, (t) => ({
          ...t,
          status: 'ready',
          subPhase: null,
          isCancelling: false,
          cues,
          streamingPreviewCues: [],
          history: createHistory(cues),
          activeCueId: cues[0]?.local_uid ?? null,
          selectedCueIds: [],
          metrics,
          failedSlotCount: failedSlotCount ?? null,
        }));
        if (!get().queueRunActive) {
          const sampleFps = snapshot.sampleFps > 0 ? snapshot.sampleFps : 5.0;
          const wallSec =
            metrics && metrics.wall_time_ms > 0
              ? (metrics.wall_time_ms / 1000).toFixed(1)
              : null;
          const speedStr =
            metrics && metrics.effective_fps > 0
              ? `平均 ${Math.round(metrics.effective_fps)} fps (${(
                  metrics.effective_fps / sampleFps
                ).toFixed(1)}×)`
              : null;
          const reasonParts = [wallSec ? `耗时 ${wallSec}s` : null, speedStr].filter(
            Boolean
          );
          pushToast({
            severity: 'success',
            title: `提取完成：共 ${cues.length} 条字幕`,
            ...(reasonParts.length > 0 ? { reason: reasonParts.join(' · ') } : {}),
            autoDismissMs: 4500,
          });
        }
        void flushDraftSave(taskId);
      };

      const finalizeCancelled = () => {
        if (settledTerminal || runGenMap.get(taskId) !== gen) {
          return;
        }
        settledTerminal = true;
        activeRunTaskIds.delete(taskId);
        updateTask(taskId, (t) => ({
          ...t,
          status: 'cancelled',
          subPhase: null,
          isCancelling: false,
        }));
        void flushDraftSave(taskId);
      };

      const finalizeFailed = (code: string, message: string, retryable: boolean) => {
        if (settledTerminal || runGenMap.get(taskId) !== gen) {
          return;
        }
        settledTerminal = true;
        activeRunTaskIds.delete(taskId);
        updateTask(taskId, (t) => ({
          ...t,
          status: 'failed',
          subPhase: null,
          isCancelling: false,
          error: { code, message, retryable },
        }));
        pushToast({
          severity: 'error',
          title: `提取失败：${extractFileName(item.videoPath)}`,
          reason: message,
          nextStep: retryable ? '可检查参数或网络后点击重试' : '请检查媒体文件与引擎配置',
        });
        void flushDraftSave(taskId);
      };

      const onEvent = (ev: TaskEvent) => {
        if (runGenMap.get(taskId) !== gen || settledTerminal) {
          return;
        }
        switch (ev.type) {
          case 'phase':
            updateTask(taskId, (t) => ({ ...t, subPhase: ev.sub_phase }));
            break;
          case 'progress':
            updateTask(taskId, (t) => ({
              ...t,
              progress: {
                currentMs: ev.current_ms,
                totalMs: ev.total_ms,
                fps: ev.fps,
                completedSheets: t.progress?.completedSheets ?? 0,
                totalSheets: t.progress?.totalSheets ?? 0,
                retriedSplits: t.progress?.retriedSplits ?? 0,
                startedAtMs: t.progress?.startedAtMs,
              },
            }));
            break;
          case 'subtitle_entry':
            if (ev.entry.text.trim().length === 0) {
              break;
            }
            updateTask(taskId, (t) => ({
              ...t,
              streamingPreviewCues: [...t.streamingPreviewCues, ev.entry],
            }));
            break;
          case 'vlm_sheet_progress':
            updateTask(taskId, (t) => ({
              ...t,
              progress: {
                currentMs: t.progress?.currentMs ?? 0,
                totalMs: t.progress?.totalMs ?? (t.probe?.duration_ms ?? 0),
                fps: t.progress?.fps ?? 0,
                completedSheets: ev.completed_sheets,
                totalSheets: ev.total_sheets,
                retriedSplits: ev.retried_splits,
                startedAtMs: t.progress?.startedAtMs,
              },
            }));
            break;
          case 'completed':
            finalizeCompleted(
              ev.result.entries,
              ev.result.metrics,
              ev.result.failed_slot_count
            );
            break;
          case 'cancelled':
            finalizeCancelled();
            break;
          case 'failed':
            finalizeFailed(ev.code, ev.message, ev.retryable);
            break;
          case 'timing_slot':
            break;
        }
      };

      try {
        const result = await get().bridge.startExtraction(backendTaskId, params, onEvent);
        finalizeCompleted(
          result.entries,
          result.metrics,
          result.failed_slot_count
        );
      } catch (err) {
        const parsed = parseIpcError(err);
        if (parsed.code === 'Aborted' || parsed.code === 'Cancelled') {
          finalizeCancelled();
        } else {
          finalizeFailed(parsed.code, parsed.message, true);
        }
      }
    };

    return {
      bridge,
      initialized: false,
      capabilities: null,
      capabilitiesError: null,
      theme: 'dark' as ThemeMode,

      tasks: [],
      activeTaskId: null,
      queueCollapsed: true,
      queuePinned: false,
      queueRunActive: false,

      isDirtyInlineEdit: false,
      isInlineTextFocused: false,
      editingCueUid: null,
      findReplace: {
        isOpen: false,
        showReplace: false,
        query: '',
        replacement: '',
        scope: 'current',
        caseSensitive: false,
        activeMatchIndex: 0,
        focusRequestId: 0,
      },

      providers: [],
      providersLoading: false,
      providersImporting: false,
      providersError: null,
      vlmSettingsOpen: false,

      recentSessions: [],
      toasts: [],
      lastExporting: false,
      lastBatchReport: null,

      pushToast,

      dismissToast(id: string): void {
        set((state) => ({
          toasts: state.toasts.filter((t) => t.id !== id),
        }));
      },

      setTheme(theme: ThemeMode): void {
        set({ theme });
      },

      setVlmSettingsOpen(open: boolean): void {
        set({ vlmSettingsOpen: open });
      },

      async initialize(): Promise<void> {
        if (get().initialized) {
          return;
        }
        try {
          const activeBridge = get().bridge;
          const [caps, providers, recent] = await Promise.all([
            activeBridge.getCapabilities(),
            activeBridge.listProviders().catch(() => []),
            activeBridge.listRecentSessions().catch(() => []),
          ]);
          const defaultId = resolveDefaultProviderId(providers);
          set((state) => ({
            initialized: true,
            capabilities: caps,
            capabilitiesError: null,
            providers,
            recentSessions: recent,
            tasks: state.tasks.map((task) => {
              const appleUnsupported =
                task.engine === 'apple_vision' &&
                !caps.engines.includes('apple_vision');
              const validProvider =
                defaultId ??
                (task.providerId && providers.some((p) => p.id === task.providerId)
                  ? task.providerId
                  : null);
              return {
                ...task,
                providerId: validProvider,
                capabilityWarning: appleUnsupported
                  ? '当前环境不支持 Apple Vision，请切换为 PaddleOCR 或 VLM 后再启动提取'
                  : task.capabilityWarning,
              };
            }),
          }));
        } catch (err) {
          const parsed = parseIpcError(err);
          set({
            initialized: true,
            capabilities: {
              engines: ['paddle'],
              ffmpeg_available: true,
              hwaccel_modes: [],
            },
            capabilitiesError: parsed.message,
          });
          pushToast({
            severity: 'warning',
            title: '获取本地算力能力失败',
            reason: parsed.message,
            nextStep: '已默认回退显示 PaddleOCR 选项，可检查工作进程状态',
          });
        }
      },

      async pickAndAddVideos(): Promise<void> {
        try {
          const paths = await get().bridge.pickVideoFiles();
          if (paths.length > 0) {
            await get().addVideoPaths(paths);
          }
        } catch (err) {
          const parsed = parseIpcError(err);
          pushToast({
            severity: 'error',
            title: '打开文件选择器失败',
            reason: parsed.message,
          });
        }
      },

      async addVideoPaths(paths: string[]): Promise<void> {
        const state = get();
        const existingPaths = new Set(
          state.tasks.map((t) => normalizeVideoPath(t.videoPath))
        );
        const defaultEngine: WorkbenchEngine =
          state.capabilities?.engines.includes('paddle') ?? true
            ? 'paddle'
            : (state.capabilities?.engines[0] ?? 'paddle');
        const defaultProviderId = resolveDefaultProviderId(state.providers);

        const newItems: QueueTaskItem[] = [];
        for (const raw of paths) {
          const norm = normalizeVideoPath(raw);
          if (!norm || existingPaths.has(norm)) {
            continue;
          }
          existingPaths.add(norm);
          newItems.push(
            createInitialTaskItem(norm, defaultEngine, defaultProviderId)
          );
        }

        if (newItems.length === 0) {
          return;
        }

        set((s) => {
          const combined = [...s.tasks, ...newItems];
          const nextActiveId = s.activeTaskId ?? newItems[0].id;
          // 单视频自动收起、多视频自动展开（尊重用户手动固定 queuePinned）
          const nextCollapsed = s.queuePinned
            ? s.queueCollapsed
            : combined.length <= 1;
          return {
            tasks: combined,
            activeTaskId: nextActiveId,
            queueCollapsed: nextCollapsed,
          };
        });

        await Promise.all(
          newItems.map((item) => probeAndHydrateItem(item.id, item.videoPath))
        );
      },

      selectActiveTask(taskId: string): void {
        const state = get();
        if (state.isDirtyInlineEdit) {
          pushToast({
            severity: 'warning',
            title: '存在未提交的行内编辑',
            nextStep: '请先按 Enter 提交或按 Esc 放弃当前修改后再切换剧集',
          });
          return;
        }
        if (state.activeTaskId === taskId) {
          return;
        }
        if (state.activeTaskId) {
          stopPlaybackIfPlaying(state.activeTaskId);
        }
        set({ activeTaskId: taskId });
      },

      removeTask(taskId: string): void {
        const state = get();
        const target = state.tasks.find((t) => t.id === taskId);
        if (!target || target.status === 'extracting') {
          return;
        }
        userTouchedTasks.delete(taskId);
        const timer = draftTimers.get(taskId);
        if (timer !== undefined) {
          clearTimeout(timer);
          draftTimers.delete(taskId);
        }
        const removingActive = state.activeTaskId === taskId;
        if (removingActive) {
          stopPlaybackIfPlaying(taskId);
        }
        const remaining = state.tasks.filter((t) => t.id !== taskId);
        const nextActiveId = removingActive
          ? remaining[0]?.id ?? null
          : state.activeTaskId;
        set((s) => ({
          tasks: remaining,
          activeTaskId: nextActiveId,
          queueCollapsed: s.queuePinned
            ? s.queueCollapsed
            : remaining.length <= 1,
          ...(removingActive
            ? {
                isDirtyInlineEdit: false,
                isInlineTextFocused: false,
                editingCueUid: null,
              }
            : {}),
        }));
      },

      toggleQueueSidebar(): void {
        set((state) => ({
          queueCollapsed: !state.queueCollapsed,
          queuePinned: true,
        }));
      },

      setQueueCollapsed(collapsed: boolean, pin: boolean = false): void {
        set((state) => ({
          queueCollapsed: collapsed,
          queuePinned: pin ? true : state.queuePinned,
        }));
      },

      setTaskEngine(engine: WorkbenchEngine, taskId?: string): void {
        const id = taskId ?? get().activeTaskId;
        if (!id) {
          return;
        }
        userTouchedTasks.add(id);
        const caps = get().capabilities;
        const unsupported =
          engine === 'apple_vision' &&
          caps !== null &&
          !caps.engines.includes('apple_vision');
        updateTask(id, (item) => {
          if (item.status === 'extracting') {
            return item;
          }
          return {
            ...item,
            engine,
            capabilityWarning: unsupported
              ? '当前环境不支持 Apple Vision，请切换为 PaddleOCR 或 VLM 后再启动提取'
              : null,
          };
        });
        scheduleDraftSave(id);
      },

      setTaskProviderId(providerId: string | null, taskId?: string): void {
        const id = taskId ?? get().activeTaskId;
        if (!id) {
          return;
        }
        userTouchedTasks.add(id);
        updateTask(id, (item) =>
          item.status === 'extracting' ? item : { ...item, providerId }
        );
        scheduleDraftSave(id);
      },

      setTaskScript(script: SubtitleScript, taskId?: string): void {
        const id = taskId ?? get().activeTaskId;
        if (!id) {
          return;
        }
        userTouchedTasks.add(id);
        updateTask(id, (item) =>
          item.status === 'extracting'
            ? item
            : {
                ...item,
                script,
                autoRegionInfo:
                  item.script === script ? item.autoRegionInfo : null,
              }
        );
        scheduleDraftSave(id);
      },

      setTaskSampleFps(fps: number, taskId?: string): void {
        const id = taskId ?? get().activeTaskId;
        if (!id || !Number.isFinite(fps)) {
          return;
        }
        userTouchedTasks.add(id);
        const clamped = Math.max(1.0, Math.min(15.0, Math.round(fps * 10) / 10));
        updateTask(id, (item) =>
          item.status === 'extracting' ? item : { ...item, sampleFps: clamped }
        );
        scheduleDraftSave(id);
      },

      setTaskRoiMode(mode: RoiMode, taskId?: string): void {
        const id = taskId ?? get().activeTaskId;
        if (!id) {
          return;
        }
        const current = get().tasks.find((t) => t.id === id);
        if (!current || current.status === 'extracting') {
          return;
        }
        userTouchedTasks.add(id);
        stopPlaybackIfPlaying(id);
        updateTask(id, (item) => {
          if (item.status === 'extracting') {
            return item;
          }
          let nextBox = item.regionBox;
          if (item.probe && item.probe.display_transform_ok) {
            if (mode === 'band') {
              nextBox = item.regionBox
                ? toBandSourceBox(item.regionBox, item.probe.width, item.probe.height)
                : createDefaultBandBox(
                    item.probe.width,
                    item.probe.height,
                    DEFAULT_BAND_BOTTOM_RATIO
                  );
            } else if ((mode === 'rect' || mode === 'auto') && !nextBox) {
              nextBox = createDefaultBandBox(
                item.probe.width,
                item.probe.height,
                DEFAULT_BAND_BOTTOM_RATIO
              );
            }
          }
          return {
            ...item,
            roiMode: mode,
            regionBox: nextBox,
            roiScaledFromQueue: false,
            autoRegionInfo: null,
          };
        });
        scheduleDraftSave(id);
      },

      setTaskRegionBox(box: SourceBox | null, taskId?: string): void {
        const id = taskId ?? get().activeTaskId;
        if (!id) {
          return;
        }
        userTouchedTasks.add(id);
        updateTask(id, (item) => {
          if (item.status === 'extracting') {
            return item;
          }
          if (!box || !item.probe) {
            return {
              ...item,
              regionBox: box,
              roiScaledFromQueue: false,
              autoRegionInfo: null,
            };
          }
          const clamped =
            item.roiMode === 'band'
              ? toBandSourceBox(box, item.probe.width, item.probe.height)
              : clampSourceBox(box, item.probe.width, item.probe.height);
          let nextMode: RoiMode = item.roiMode;
          if (item.roiMode === 'auto' && clamped) {
            nextMode =
              clamped.x === 0 && clamped.width === Math.floor(item.probe.width)
                ? 'band'
                : 'rect';
          }
          return {
            ...item,
            roiMode: nextMode,
            regionBox: clamped,
            roiScaledFromQueue: false,
            autoRegionInfo: null,
          };
        });
        scheduleDraftSave(id);
      },

      async detectAutoRegionForTask(taskId?: string): Promise<void> {
        const id = taskId ?? get().activeTaskId;
        if (!id) {
          return;
        }
        const item = get().tasks.find((t) => t.id === id);
        if (!item || !item.probe || !item.probe.display_transform_ok) {
          return;
        }
        if (item.status === 'extracting' || item.isAutoDetecting) {
          return;
        }
        userTouchedTasks.add(id);
        stopPlaybackIfPlaying(id);

        const gen = (autoRegionGenMap.get(id) ?? 0) + 1;
        autoRegionGenMap.set(id, gen);
        // 若当前为 vlm 或当前环境不支持该本地引擎，detect_auto_region 回退本地可用引擎
        const supportedEngines = get().capabilities?.engines ?? ['paddle'];
        const detectEngine: OcrEngineId =
          item.engine !== 'vlm' && supportedEngines.includes(item.engine)
            ? item.engine
            : resolveLocalDetectEngine();

        updateTask(id, (t) => ({ ...t, isAutoDetecting: true }));

        try {
          const res = await get().bridge.detectAutoRegion(
            item.videoPath,
            detectEngine,
            item.script,
            DEFAULT_AUTO_BOTTOM_RATIO
          );
          const latest = get().tasks.find((t) => t.id === id);
          if (
            autoRegionGenMap.get(id) !== gen ||
            !latest ||
            !latest.probe ||
            latest.status === 'extracting'
          ) {
            if (autoRegionGenMap.get(id) === gen && latest?.isAutoDetecting) {
              updateTask(id, (t) => ({ ...t, isAutoDetecting: false }));
            }
            return;
          }
          const clampedBox = clampSourceBox(
            res.region_box,
            latest.probe.width,
            latest.probe.height
          );
          const seekMs = clampMsToRange(
            res.sample_timestamp_ms,
            latest.probe.duration_ms
          );

          updateTask(id, (t) =>
            t.status === 'extracting'
              ? { ...t, isAutoDetecting: false }
              : {
                  ...t,
                  isAutoDetecting: false,
                  roiMode: 'auto',
                  regionBox: clampedBox ?? t.regionBox,
                  roiScaledFromQueue: false,
                  previewTimestampMs: res.detected ? seekMs : t.previewTimestampMs,
                  autoRegionInfo: {
                    detected: res.detected,
                    confidence: res.confidence,
                    previewText: res.preview_text,
                    sampleTimestampMs: res.sample_timestamp_ms,
                    errorMessage: res.error_message,
                    profile: res.profile ?? null,
                    script: latest.script,
                  },
                }
          );

          if (!res.detected) {
            pushToast({
              severity: 'warning',
              title: '自动探测未命中明显字幕行，已回退默认字幕带',
              reason: res.error_message || '采样关键帧中未识别到稳定字幕聚类',
              nextStep: '可拖拽时间轴至有字幕的画面后重试，或切换为「字幕带 / 自由矩形」手动画框',
            });
          }
          scheduleDraftSave(id);
        } catch (err) {
          if (autoRegionGenMap.get(id) !== gen) {
            return;
          }
          const latest = get().tasks.find((t) => t.id === id);
          updateTask(id, (t) => ({ ...t, isAutoDetecting: false }));
          if (latest?.status === 'extracting') {
            return;
          }
          const parsed = parseIpcError(err);
          pushToast({
            severity: 'error',
            title: '自动探测字幕区域失败',
            reason: parsed.message,
            nextStep: '可切换为「字幕带」或「自由矩形」手动框选区域',
          });
        }
      },

      applyConfigToQueue(options?: ApplyConfigOptions) {
        const state = get();
        const sourceId =
          options?.sourceTaskId ?? state.activeTaskId ?? state.tasks[0]?.id;
        const source = state.tasks.find((t) => t.id === sourceId);
        if (!source || !source.probe || !source.probe.display_transform_ok) {
          return {
            updatedCount: 0,
            skippedCompletedCount: 0,
            skippedExtractingCount: 0,
          };
        }
        const includeCompleted = options?.includeCompleted ?? false;
        const sourceBox =
          source.regionBox ??
          createDefaultBandBox(
            source.probe.width,
            source.probe.height,
            DEFAULT_BAND_BOTTOM_RATIO
          );

        let updatedCount = 0;
        let skippedCompletedCount = 0;
        let skippedExtractingCount = 0;
        const updatedIds: string[] = [];

        set((s) => ({
          tasks: s.tasks.map((target) => {
            if (target.id === source.id) {
              return target;
            }
            if (target.status === 'extracting' || target.isReRecognizing) {
              skippedExtractingCount += 1;
              return target;
            }
            if (
              !includeCompleted &&
              (target.status === 'ready' || target.status === 'exported')
            ) {
              skippedCompletedCount += 1;
              return target;
            }

            updatedCount += 1;
            updatedIds.push(target.id);

            let nextBox: SourceBox | null = null;
            let scaledFlag = false;
            let pendingScaledRoi = target.pendingScaledRoi;

            if (sourceBox) {
              if (target.probe) {
                const sameRes =
                  target.probe.width === source.probe!.width &&
                  target.probe.height === source.probe!.height;
                if (sameRes) {
                  nextBox = { ...sourceBox };
                  scaledFlag = false;
                } else {
                  nextBox = scaleSourceBox(
                    sourceBox,
                    source.probe!.width,
                    source.probe!.height,
                    target.probe.width,
                    target.probe.height
                  );
                  scaledFlag = true;
                }
                pendingScaledRoi = null;
              } else {
                pendingScaledRoi = {
                  fromW: source.probe!.width,
                  fromH: source.probe!.height,
                  box: { ...sourceBox },
                };
              }
            }

            const shouldResetCompleted =
              includeCompleted &&
              (target.status === 'ready' || target.status === 'exported');

            return {
              ...target,
              engine: source.engine,
              providerId: source.providerId,
              script: source.script,
              sampleFps: source.sampleFps,
              roiMode: source.roiMode,
              regionBox: nextBox,
              roiScaledFromQueue: scaledFlag,
              pendingScaledRoi,
              autoRegionInfo: null,
              capabilityWarning: source.capabilityWarning,
              ...(shouldResetCompleted
                ? {
                    status: 'idle' as const,
                    cues: [],
                    streamingPreviewCues: [],
                    history: createHistory([]),
                    activeCueId: null,
                    selectedCueIds: [],
                    error: null,
                    metrics: null,
                    failedSlotCount: null,
                  }
                : {}),
            };
          }),
        }));

        for (const uid of updatedIds) {
          userTouchedTasks.add(uid);
          scheduleDraftSave(uid);
        }

        if (updatedCount > 0) {
          pushToast({
            severity: 'success',
            title: `已将当前配置套用至 ${updatedCount} 个队列视频`,
          });
        }

        return {
          updatedCount,
          skippedCompletedCount,
          skippedExtractingCount,
        };
      },

      seekPreview(timestampMs: number, taskId?: string): void {
        const id = taskId ?? get().activeTaskId;
        if (!id) {
          return;
        }
        const item = get().tasks.find((t) => t.id === id);
        if (!item) {
          return;
        }
        stopPlaybackIfPlaying(id);
        const clamped = clampMsToRange(timestampMs, item.probe?.duration_ms);
        updateTask(id, (t) => ({
          ...t,
          previewTimestampMs: clamped,
          playbackMode: 'still',
        }));
      },

      stepPreview(deltaMs: number, taskId?: string): void {
        const id = taskId ?? get().activeTaskId;
        if (!id) {
          return;
        }
        const item = get().tasks.find((t) => t.id === id);
        if (!item) {
          return;
        }
        stopPlaybackIfPlaying(id);
        const clamped = clampMsToRange(
          item.previewTimestampMs + deltaMs,
          item.probe?.duration_ms
        );
        updateTask(id, (t) => ({
          ...t,
          previewTimestampMs: clamped,
          playbackMode: 'still',
        }));
      },

      setPlaybackMode(mode: 'still' | 'playing', taskId?: string): void {
        const id = taskId ?? get().activeTaskId;
        if (!id) {
          return;
        }
        const item = get().tasks.find((t) => t.id === id);
        if (!item) {
          return;
        }
        if (mode === 'still') {
          stopPlaybackIfPlaying(id);
          return;
        }
        updateTask(id, (t) => ({
          ...t,
          playbackMode: 'playing',
          streamBaseMs: t.previewTimestampMs,
        }));
      },

      syncPlaybackTime(timestampMs: number, taskId?: string): void {
        const id = taskId ?? get().activeTaskId;
        if (!id || !Number.isFinite(timestampMs)) {
          return;
        }
        const item = get().tasks.find((t) => t.id === id);
        if (!item || item.playbackMode !== 'playing') {
          return;
        }
        const clamped = clampMsToRange(timestampMs, item.probe?.duration_ms);
        updateTask(id, (t) =>
          t.playbackMode === 'playing'
            ? { ...t, previewTimestampMs: clamped }
            : t
        );
      },

      async startTaskExtraction(taskId?: string): Promise<void> {
        const id = taskId ?? get().activeTaskId;
        if (!id) {
          return;
        }
        await runSingleTaskExtraction(id);
      },

      async startQueueExtraction(): Promise<void> {
        if (get().queueRunActive) {
          return;
        }
        const myGen = ++queueRunGen;
        let completedCount = 0;
        set({ queueRunActive: true });
        try {
          // 顺序执行队列中的 idle（以及未被人工干预的新增）项；失败不阻塞后续项
          const visited = new Set<string>();
          while (get().queueRunActive && queueRunGen === myGen) {
            const nextItem = get().tasks.find(
              (t) =>
                !visited.has(t.id) &&
                t.status === 'idle' &&
                t.probe !== null &&
                t.probe.display_transform_ok
            );
            if (!nextItem) {
              break;
            }
            visited.add(nextItem.id);
            await runSingleTaskExtraction(nextItem.id);
            const finished = get().tasks.find((t) => t.id === nextItem.id);
            if (finished?.status === 'ready') {
              completedCount += 1;
            }
          }
          if (queueRunGen === myGen && completedCount > 0) {
            pushToast({
              severity: 'success',
              title: `队列提取完成：已完成 ${completedCount} 个视频`,
              autoDismissMs: 5000,
            });
          }
        } finally {
          if (queueRunGen === myGen) {
            set({ queueRunActive: false });
          }
        }
      },

      async cancelTaskExtraction(taskId?: string): Promise<void> {
        const id = taskId ?? get().activeTaskId;
        if (!id) {
          return;
        }
        const item = get().tasks.find((t) => t.id === id);
        if (!item || item.status !== 'extracting') {
          return;
        }
        if (preDetectTaskIds.has(id)) {
          preDetectTaskIds.delete(id);
          runGenMap.set(id, (runGenMap.get(id) ?? 0) + 1);
          activeRunTaskIds.delete(id);
          updateTask(id, (t) => ({
            ...t,
            status: 'cancelled',
            subPhase: null,
            isAutoDetecting: false,
            isCancelling: false,
          }));
          void flushDraftSave(id);
          return;
        }
        const backendTaskId = activeRunTaskIds.get(id);
        if (!backendTaskId) {
          return;
        }
        updateTask(id, (t) => ({ ...t, isCancelling: true }));
        try {
          await get().bridge.cancelExtraction(backendTaskId);
        } catch {
          // 依赖 Channel Cancelled 或 Promise Aborted 收敛状态
        }
      },

      async cancelAllExtraction(): Promise<void> {
        queueRunGen += 1;
        set({ queueRunActive: false });
        const extractingIds = get()
          .tasks.filter((t) => t.status === 'extracting')
          .map((t) => t.id);
        await Promise.all(
          extractingIds.map((id) => get().cancelTaskExtraction(id))
        );
      },

      ...providerActions,
      ...proofreadActions,

      async exportActiveSrt(outPathOverride?: string): Promise<boolean> {
        const state = get();
        const active = state.tasks.find((t) => t.id === state.activeTaskId);
        const commands = selectActiveCommands(state);
        if (!active || !commands.exportActiveSrt.enabled) {
          return false;
        }

        let outPath = outPathOverride ?? null;
        if (!outPath) {
          try {
            outPath = await get().bridge.pickSaveSrtPath(
              deriveSrtFileName(active.fileName)
            );
          } catch (err) {
            const parsed = parseIpcError(err);
            pushToast({
              severity: 'error',
              title: '打开保存对话框失败',
              reason: parsed.message,
            });
            return false;
          }
        }

        if (!outPath) {
          return false;
        }

        const latestBeforeExport = get().tasks.find((t) => t.id === active.id);
        if (
          !latestBeforeExport ||
          (latestBeforeExport.status !== 'ready' &&
            latestBeforeExport.status !== 'exported') ||
          latestBeforeExport.cues.length === 0
        ) {
          return false;
        }
        const exportedCues = latestBeforeExport.cues;

        set({ lastExporting: true });
        try {
          // 契约硬要求：前端校对导出固定传 apply_merge: false，统一走 C++ finalize_srt
          const res = await get().bridge.exportSrt(
            outPath,
            toSubtitleEntries(exportedCues),
            false
          );
          updateTask(active.id, (t) =>
            (t.status === 'ready' || t.status === 'exported') &&
            t.cues === exportedCues
              ? { ...t, status: 'exported' }
              : t
          );
          set({ lastExporting: false });
          await flushDraftSave(active.id);
          pushToast({
            severity: 'success',
            title: `已导出 SRT（${res.cue_count} 条）`,
            reason: res.out_path,
          });
          return true;
        } catch (err) {
          const parsed = parseIpcError(err);
          set({ lastExporting: false });
          pushToast({
            severity: 'error',
            title: '导出 SRT 失败',
            reason: parsed.message,
            nextStep: '请确认目标目录有写入权限且文件未被占用',
          });
          return false;
        }
      },

      async batchExportSrt(
        conflictPolicy: BatchExportConflictPolicy,
        includeEmpty: boolean = false
      ): Promise<BatchExportReport | null> {
        const state = get();
        if (state.isDirtyInlineEdit) {
          return null;
        }
        const candidates = state.tasks.filter(
          (t) =>
            (t.status === 'ready' || t.status === 'exported') &&
            (includeEmpty || t.cues.length > 0)
        );
        if (candidates.length === 0) {
          return null;
        }

        const exportedCuesById = new Map(candidates.map((t) => [t.id, t.cues]));
        const items: BatchExportItemInput[] = candidates.map((t) => ({
          video_path: t.videoPath,
          out_path: deriveSiblingSrtPath(t.videoPath),
          entries: toSubtitleEntries(t.cues),
        }));

        set({ lastExporting: true });
        try {
          const report = await get().bridge.batchExportSrt(items, conflictPolicy);
          const exportedPaths = new Set(
            report.results
              .filter(
                (r) => r.status === 'exported' || r.status === 'renamed'
              )
              .map((r) => r.video_path)
          );
          const exportedIds: string[] = [];
          set((s) => ({
            lastExporting: false,
            lastBatchReport: report,
            tasks: s.tasks.map((t) => {
              const snapshotCues = exportedCuesById.get(t.id);
              if (
                exportedPaths.has(t.videoPath) &&
                snapshotCues !== undefined &&
                (t.status === 'ready' || t.status === 'exported') &&
                t.cues === snapshotCues
              ) {
                exportedIds.push(t.id);
                return { ...t, status: 'exported' };
              }
              return t;
            }),
          }));
          await Promise.all(exportedIds.map((id) => flushDraftSave(id)));
          return report;
        } catch (err) {
          const parsed = parseIpcError(err);
          set({ lastExporting: false });
          pushToast({
            severity: 'error',
            title: '批量导出 SRT 失败',
            reason: parsed.message,
            nextStep: '请检查原片目录写入权限后重试',
          });
          return null;
        }
      },

      clearBatchReport(): void {
        set({ lastBatchReport: null });
      },

      flushDraftSave,

      resetStore(nextBridge?: IpcBridge): void {
        for (const timer of draftTimers.values()) {
          clearTimeout(timer);
        }
        draftTimers.clear();
        runGenMap.clear();
        activeRunTaskIds.clear();
        preDetectTaskIds.clear();
        probeGenMap.clear();
        autoRegionGenMap.clear();
        userTouchedTasks.clear();
        queueRunGen += 1;
        set({
          bridge: nextBridge ?? get().bridge,
          initialized: false,
          capabilities: null,
          capabilitiesError: null,
          theme: 'dark',
          tasks: [],
          activeTaskId: null,
          queueCollapsed: true,
          queuePinned: false,
          queueRunActive: false,
          isDirtyInlineEdit: false,
          isInlineTextFocused: false,
          editingCueUid: null,
          findReplace: {
            isOpen: false,
            showReplace: false,
            query: '',
            replacement: '',
            scope: 'current',
            caseSensitive: false,
            activeMatchIndex: 0,
            focusRequestId: 0,
          },
          providers: [],
          providersLoading: false,
          providersImporting: false,
          providersError: null,
          vlmSettingsOpen: false,
          recentSessions: [],
          toasts: [],
          lastExporting: false,
          lastBatchReport: null,
        });
      },
    };
  });
}

export const useWorkbenchStore = createWorkbenchStore(createTauriBridge());
