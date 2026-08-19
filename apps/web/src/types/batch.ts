import type { SubtitleEntry, OcrEngineName, NormalizedRegionBox } from './api';

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

/**
 * 采样质量 3 档分级（UI 统一展示中文名与基准 FPS）
 */
export type SamplingQuality = 'fast' | 'balanced' | 'fine';

export const SAMPLING_QUALITY_MAP: Record<
  SamplingQuality,
  { fps: number; label: string; desc: string }
> = {
  fast: { fps: 5.0, label: '快速', desc: '5 FPS 采样，更快完成，适合常规字幕' },
  balanced: { fps: 8.0, label: '平衡', desc: '8 FPS 采样，速度与短字幕召回更均衡' },
  fine: { fps: 12.0, label: '精细', desc: '12 FPS 采样，更高采样密度' },
};

/** 状态投影过滤器 */
export type BatchTaskStatusFilter =
  | 'all'
  | 'waiting'
  | 'running'
  | 'completed'
  | 'failed'
  | 'cancelled'
  | 'skipped';

export interface BatchTaskConfig {
  engine: OcrEngineName;
  quality: SamplingQuality;
  confidence_threshold?: number;
  region_box?: NormalizedRegionBox;
}

export interface BatchTaskResult {
  entryCount: number;
  outputPath: string;
  runtimeIdentity?: string;
}

export interface BatchTaskItem {
  id: string;
  videoPath: string;
  name: string;
  importRootPath?: string; // 来源根目录，用于计算 locationDisplay
  outputPath?: string; // 预期输出 SRT 路径
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
  outputFullPath?: string;
  outputFileExists: boolean;
  planningError?: string;
  outputExistsWarning?: string;
  engine: OcrEngineName;
  quality: SamplingQuality;
  engineDisplay: string;
  qualityDisplay: string;
  canEditConfiguration: boolean;
  failureMessage?: string;
  runtimeIdentity?: string;
  entryCount: number;
  canCancel: boolean;
  canRetry: boolean;
  canRemove: boolean;
  canReorder: boolean;
  canStartSingle: boolean;
}

export type BatchInputRejectionReason =
  | { kind: 'unsupportedFormat'; extension: string }
  | { kind: 'mkvRequiresFfmpeg' }
  | { kind: 'unreadable'; detail?: string }
  | { kind: 'duplicate' }
  | { kind: 'emptyDirectory' };

export interface BatchScanRejection {
  pathOrName: string;
  reason: BatchInputRejectionReason;
}

export interface BatchAcceptedItem {
  videoPath: string;
  name: string;
  sizeBytes?: number;
  importRootPath?: string;
}

export interface BatchScanSummary {
  accepted: BatchAcceptedItem[];
  skipped: number; // hidden / package / symlink 静默排除计数
  rejected: BatchScanRejection[];
}
