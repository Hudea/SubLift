import type { SubtitleEntry, OcrEngineName, NormalizedRegionBox } from './api';
import type { RoiPolicy, SamplingQuality, ExtractionConfig } from './config';
export type { RoiPolicy, SamplingQuality, ExtractionConfig } from './config';
export {
  SAMPLING_QUALITY_MAP,
  QUALITY_FPS_MAP,
  DEFAULT_BOTTOM_ROI,
  DEFAULT_CONFIDENCE_THRESHOLD,
  createDefaultExtractionConfig,
  qualityToFps,
  fpsToQuality,
} from './config';

/**
 * 08102 / 08308 对齐：9 状态批量任务状态机
 */
export type BatchTaskStatus =
  | 'waiting'
  | 'preparing'
  | 'extracting'
  | 'exporting'
  | 'completed'
  | 'failed'
  | 'cancelled'
  | 'interrupted'
  | 'skipped';

/** 状态投影过滤器 */
export type BatchTaskStatusFilter =
  | 'all'
  | 'waiting'
  | 'running'
  | 'completed'
  | 'failed'
  | 'cancelled'
  | 'skipped';

export interface BatchTaskConfig extends ExtractionConfig {}

export interface BatchTaskResult {
  entryCount: number;
  outputPath: string; // 规划/预期输出路径
  savedPath?: string; // 实际已落盘的最终文件绝对路径
  diskStatus?: 'unwritten' | 'saved' | 'skipped' | 'empty_result' | 'failed'; // 真实磁盘落盘状态
  emptyResult?: boolean; // 是否为空字幕（0 条目）
  savedAt?: number;
  runtimeIdentity?: string;
  roiSource?: string;
  effectiveRegion?: NormalizedRegionBox | null;
}

export interface BatchTaskItem {
  id: string;
  videoPath: string;
  name: string;
  importRootPath?: string; // 来源根目录，用于计算 locationDisplay
  outputPath?: string; // 预期输出 SRT 路径
  savedPath?: string; // 实际落盘文件路径
  diskStatus?: 'unwritten' | 'saved' | 'skipped' | 'empty_result' | 'failed';
  emptyResult?: boolean;
  outputExists?: boolean; // 输出目标是否已存在（预警）
  planningError?: string; // 规划错误提示
  status: BatchTaskStatus;
  progressPct: number;
  stage: string;
  jobId: string | null;
  entries: SubtitleEntry[];
  error: string | null;
  elapsedMs: number;
  config: BatchTaskConfig;
  result?: BatchTaskResult;
  createdAt: number;
}

export interface BatchQueueStats {
  total: number;
  waiting: number;
  active: number;
  completed: number;
  failed: number;
  cancelled: number;
  skipped: number;
}

export interface TaskInspectorModel {
  id: string;
  filename: string;
  statusName: string;
  status: BatchTaskStatus;
  locationDisplay: string;
  locationFullPath: string;
  outputFolderDisplay: string;
  outputFilename: string;
  outputFullPath?: string; // 规划路径
  savedFullPath?: string; // 实际落盘路径
  diskStatus: 'unwritten' | 'saved' | 'skipped' | 'empty_result' | 'failed';
  diskStatusDisplay: string;
  emptyResult: boolean;
  outputFileExists: boolean;
  planningError?: string;
  outputExistsWarning?: string;
  engine: OcrEngineName;
  quality: SamplingQuality;
  confidence_threshold: number;
  roi_policy: RoiPolicy;
  roi_source_display: string;
  region_box?: NormalizedRegionBox | null;
  script?: string;
  engineDisplay: string;
  qualityDisplay: string;
  roiPolicyDisplay: string;
  canEditConfiguration: boolean;
  isEngineAvailable: boolean;
  engineUnavailableReason?: string;
  failureMessage?: string;
  runtimeIdentity?: string;
  entryCount: number;
  canCancel: boolean;
  canRetry: boolean;
  canRemove: boolean;
  canReorder: boolean;
  canStartSingle: boolean;
  canSaveToDisk: boolean;
}

export type BatchInputRejectionReason =
  | { kind: 'unsupportedFormat'; extension?: string; detail?: string }
  | { kind: 'mkvRequiresFfmpeg'; detail?: string }
  | { kind: 'unreadable'; detail?: string }
  | { kind: 'duplicate'; detail?: string }
  | { kind: 'emptyDirectory'; detail?: string }
  | { kind: 'outOfWorkspace'; detail?: string }
  | { kind: 'securityViolation'; detail?: string };

export interface BatchScanRejection {
  path?: string;
  pathOrName: string;
  reason: BatchInputRejectionReason;
  message?: string;
}

export interface BatchAcceptedItem {
  id?: string;
  videoPath: string;
  name: string;
  sizeBytes?: number;
  importRootPath?: string;
  relativePath?: string;
  outputPath?: string;
  outputExists?: boolean;
  format?: string;
  location?: string;
}

export interface BatchScanSummary {
  accepted: BatchAcceptedItem[];
  skipped: number; // hidden / package / symlink 静默排除计数
  rejected: BatchScanRejection[];
}
