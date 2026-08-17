export type OcrEngineName = 'vision' | 'paddle' | 'mock';

export interface SystemEngineInfo {
  name: OcrEngineName;
  available: boolean;
  detail: string;
  model_type?: string;
  model_root?: string;
}

export interface FfmpegInfo {
  available: boolean;
  path?: string;
}

export interface SystemInfoDTO {
  version: string;
  runtime: 'cpp' | 'python';
  capabilities: string[];
  engines: SystemEngineInfo[];
  ffmpeg: FfmpegInfo;
}

export interface NormalizedRegionBox {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface JobConfig {
  video_path: string;
  engine: OcrEngineName;
  fps: number;
  confidence_threshold: number;
  region_box?: NormalizedRegionBox;
}

export interface CreateJobResponse {
  job_id: string;
  status: string;
}

export interface SubtitleEntry {
  index: number;
  start_ms: number;
  end_ms: number;
  text: string;
  confidence: number;
}

export interface SseProgressData {
  job_id?: string;
  stage: string;
  pct: number;
  eta_ms?: number;
}

export interface SsePushEntryData {
  job_id?: string;
  entry: SubtitleEntry;
}

export interface SseDoneData {
  job_id?: string;
  ok?: boolean;
  status?: string;
  total_entries: number;
  elapsed_ms: number;
  video_path?: string;
}

export interface SseJobCallbacks {
  onProgress?: (data: SseProgressData) => void;
  onPushEntry?: (data: SsePushEntryData) => void;
  onDone?: (data: SseDoneData) => void;
  onError?: (error: string) => void;
  /** 服务端主动取消/终止：语义为 cancelled 而非 failed */
  onCancelled?: (reason: string) => void;
}

/** 本机指纹反查（Feature 12507）：文件名 + 大小 + 首尾 4KB 原始字节 hex */
export interface FileFingerprintDTO {
  name: string;
  size: number;
  head_hex: string;
  tail_hex: string;
}

/** 媒体处理工作区配置（Feature 12508） */
export interface WorkspaceConfigDTO {
  configured: boolean;
  media_dir: string;
  cache_dir: string;
  video_count: number;
}

/** 工作区视频文件项（Feature 12508） */
export interface WorkspaceVideoFileDTO {
  name: string;
  path: string;
  relative_path: string;
  size_bytes: number;
}

