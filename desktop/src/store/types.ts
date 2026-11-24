import type { UndoRedoHistory } from '../core/history';
import type {
  BatchExportConflictPolicy,
  BatchExportReport,
  CapabilitiesDto,
  ExtractionMetricsDto,
  ExtractionSubPhase,
  IpcBridge,
  ProofreadCue,
  ProviderConnectionTestDto,
  ProviderProfileMaskedDto,
  ProviderUpsertDto,
  RoiMode,
  SessionSummaryDto,
  SourceBox,
  SubtitleEntry,
  SubtitleProfileDto,
  SubtitleScript,
  TaskStatus,
  VideoInfoDto,
  WorkbenchEngine,
} from '../core/types';

export * from '../core/types';

export const EXTRACTION_SUB_PHASES: readonly ExtractionSubPhase[] = [
  'probing',
  'timing',
  'local_ocr',
  'vlm_sheets',
  'finalizing',
];

export type ThemeMode = 'dark' | 'light';

export type ToastSeverity = 'info' | 'success' | 'warning' | 'error';

export interface ToastItem {
  id: string;
  severity: ToastSeverity;
  title: string;
  reason?: string;
  nextStep?: string;
  actionLabel?: string;
  actionType?: 'open_vlm_settings' | 'retry_active_task';
  autoDismissMs?: number;
}

export interface ConfigSnapshot {
  engine: WorkbenchEngine;
  providerId: string | null;
  script: SubtitleScript;
  sampleFps: number;
  roiMode: RoiMode;
  regionBox: SourceBox | null;
}

export interface TaskProgressState {
  currentMs: number;
  totalMs: number;
  fps: number;
  completedSheets: number;
  totalSheets: number;
  retriedSplits: number;
  startedAtMs?: number;
}

export interface TaskErrorState {
  code: string;
  message: string;
  retryable: boolean;
}

export interface AutoRegionInfoState {
  detected: boolean;
  confidence: number;
  previewText: string;
  sampleTimestampMs: number;
  errorMessage: string;
  profile?: SubtitleProfileDto | null;
  script?: SubtitleScript;
}

export interface PendingScaledRoi {
  fromW: number;
  fromH: number;
  box: SourceBox;
}

export interface QueueTaskItem {
  id: string;
  videoPath: string;
  fileName: string;
  probe: VideoInfoDto | null;
  probeLoading: boolean;
  probeError: string | null;
  capabilityWarning: string | null;

  engine: WorkbenchEngine;
  providerId: string | null;
  script: SubtitleScript;
  sampleFps: number;
  roiMode: RoiMode;
  regionBox: SourceBox | null;
  roiScaledFromQueue: boolean;
  pendingScaledRoi: PendingScaledRoi | null;
  autoRegionInfo: AutoRegionInfoState | null;

  status: TaskStatus;
  subPhase: ExtractionSubPhase | null;
  progress: TaskProgressState | null;
  activeRunConfig: ConfigSnapshot | null;
  error: TaskErrorState | null;
  metrics: ExtractionMetricsDto | null;
  failedSlotCount: number | null;

  cues: ProofreadCue[];
  streamingPreviewCues: SubtitleEntry[];
  history: UndoRedoHistory<ProofreadCue[]>;
  activeCueId: string | null;
  selectedCueIds: string[];

  previewTimestampMs: number;
  playbackMode: 'still' | 'playing';
  streamBaseMs: number;
  isAutoDetecting: boolean;
  isReRecognizing: boolean;
  isCancelling: boolean;
}

export interface FindReplaceState {
  isOpen: boolean;
  showReplace: boolean;
  query: string;
  replacement: string;
  scope: 'current' | 'queue';
  caseSensitive: boolean;
  activeMatchIndex: number;
  focusRequestId?: number;
}

export interface WorkbenchState {
  bridge: IpcBridge;
  initialized: boolean;
  capabilities: CapabilitiesDto | null;
  capabilitiesError: string | null;
  theme: ThemeMode;

  tasks: QueueTaskItem[];
  activeTaskId: string | null;
  queueCollapsed: boolean;
  queuePinned: boolean;
  queueRunActive: boolean;

  isDirtyInlineEdit: boolean;
  isInlineTextFocused: boolean;
  editingCueUid: string | null;
  findReplace: FindReplaceState;

  providers: ProviderProfileMaskedDto[];
  providersLoading: boolean;
  providersImporting: boolean;
  providersError: string | null;
  vlmSettingsOpen: boolean;

  recentSessions: SessionSummaryDto[];
  toasts: ToastItem[];
  lastExporting: boolean;
  lastBatchReport: BatchExportReport | null;
}

export interface ApplyConfigOptions {
  sourceTaskId?: string;
  includeCompleted?: boolean;
}

export interface WorkbenchActions {
  initialize(): Promise<void>;
  setTheme(theme: ThemeMode): void;
  dismissToast(id: string): void;
  pushToast(toast: Omit<ToastItem, 'id'>): string;
  setVlmSettingsOpen(open: boolean): void;

  // 队列与文件添加
  pickAndAddVideos(): Promise<void>;
  addVideoPaths(paths: string[]): Promise<void>;
  selectActiveTask(taskId: string): void;
  removeTask(taskId: string): void;
  toggleQueueSidebar(): void;
  setQueueCollapsed(collapsed: boolean, pin?: boolean): void;

  // 引擎、选区与播控配置
  setTaskEngine(engine: WorkbenchEngine, taskId?: string): void;
  setTaskProviderId(providerId: string | null, taskId?: string): void;
  setTaskScript(script: SubtitleScript, taskId?: string): void;
  setTaskSampleFps(fps: number, taskId?: string): void;
  setTaskRoiMode(mode: RoiMode, taskId?: string): void;
  setTaskRegionBox(box: SourceBox | null, taskId?: string): void;
  detectAutoRegionForTask(taskId?: string): Promise<void>;
  applyConfigToQueue(options?: ApplyConfigOptions): {
    updatedCount: number;
    skippedCompletedCount: number;
    skippedExtractingCount: number;
  };

  // 画布定位与播放控制
  seekPreview(timestampMs: number, taskId?: string): void;
  stepPreview(deltaMs: number, taskId?: string): void;
  setPlaybackMode(mode: 'still' | 'playing', taskId?: string): void;
  syncPlaybackTime(timestampMs: number, taskId?: string): void;

  // 提取执行与取消
  startTaskExtraction(taskId?: string): Promise<void>;
  startQueueExtraction(): Promise<void>;
  cancelTaskExtraction(taskId?: string): Promise<void>;
  cancelAllExtraction(): Promise<void>;

  // 校对、编辑与局部 VLM 重识
  setDirtyInlineEdit(
    dirty: boolean,
    inlineTextFocused?: boolean,
    cueUid?: string | null
  ): void;
  selectCue(localUid: string, opts?: { seek?: boolean }): void;
  toggleCueSelection(localUid: string): void;
  setSelectedCueIds(localUids: string[]): void;
  selectAllLowConfCues(): void;
  clearCueSelection(): void;
  commitCueText(localUid: string, text: string): void;
  commitCueTimes(
    localUid: string,
    startMs: number,
    endMs: number
  ): { ok: boolean; reason?: string };
  commitCueEdit(
    localUid: string,
    patch: { text?: string; startMs?: number; endMs?: number }
  ): { ok: boolean; reason?: string };
  nudgeCueTime(
    localUid: string,
    field: 'start_ms' | 'end_ms',
    deltaMs: number
  ): { ok: boolean; reason?: string };
  deleteCue(localUid: string): void;
  mergeCueWithNext(localUid: string): { ok: boolean; reason?: string };
  splitCue(
    localUid: string,
    cursorOffset: number,
    pendingText?: string
  ): { ok: boolean; reason?: string; splitLocalUid?: string };
  undo(taskId?: string): void;
  redo(taskId?: string): void;

  // 查找替换
  openFind(showReplace?: boolean): void;
  closeFind(): void;
  setFindState(partial: Partial<FindReplaceState>): void;
  navigateFindMatch(direction: 'next' | 'prev'): void;
  replaceCurrentMatch(): void;
  replaceAllMatches(): number;

  // 低置信度 VLM 局部重识
  reRecognizeCues(
    localUids: string[]
  ): Promise<{ ok: boolean; succeeded: number; failed: number; needsProvider?: boolean }>;
  reRecognizeAllLowConf(): Promise<{
    ok: boolean;
    succeeded: number;
    failed: number;
    needsProvider?: boolean;
  }>;

  // Provider 档案管理
  loadProviders(): Promise<void>;
  saveProvider(profile: ProviderUpsertDto): Promise<ProviderProfileMaskedDto | null>;
  testProviderConnection(
    profile: ProviderUpsertDto
  ): Promise<
    | { ok: true; result: ProviderConnectionTestDto }
    | { ok: false; error: string }
  >;
  deleteProvider(id: string): Promise<boolean>;
  importProvidersFromEnv(envPath?: string): Promise<ProviderProfileMaskedDto[]>;

  // 导出与草稿
  exportActiveSrt(outPathOverride?: string): Promise<boolean>;
  batchExportSrt(
    conflictPolicy: BatchExportConflictPolicy,
    includeEmpty?: boolean
  ): Promise<BatchExportReport | null>;
  clearBatchReport(): void;
  flushDraftSave(taskId: string): Promise<void>;
  resetStore(nextBridge?: IpcBridge): void;
}

export type WorkbenchStore = WorkbenchState & WorkbenchActions;
