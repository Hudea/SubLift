import type { SubtitleEntry, OcrEngineName, NormalizedRegionBox } from './api';

export type BatchTaskStatus = 'waiting' | 'running' | 'completed' | 'failed' | 'cancelled';

export interface BatchTaskConfig {
  engine: OcrEngineName;
  fps: number;
  confidence_threshold: number;
  region_box?: NormalizedRegionBox;
}

export interface BatchTaskItem {
  id: string;
  videoPath: string;
  name: string;
  sizeBytes?: number;
  status: BatchTaskStatus;
  progressPct: number;
  stage: string;
  jobId: string | null;
  entries: SubtitleEntry[];
  error: string | null;
  elapsedMs: number;
  config: BatchTaskConfig;
  createdAt: number;
}

export interface BatchQueueStats {
  total: number;
  waiting: number;
  running: number;
  completed: number;
  failed: number;
  cancelled: number;
}
