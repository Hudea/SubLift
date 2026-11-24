// ==================== 1. 基础枚举（与 src-tauri/src/dto.rs 1:1 对齐） ====================

export type OcrEngineId = 'paddle' | 'apple_vision';
export type WorkbenchEngine = OcrEngineId | 'vlm';
export type SubtitleScript = 'auto' | 'cjk' | 'latin';
export type RoiMode = 'auto' | 'band' | 'rect';
export type ExtractionMode = 'local_ocr' | 'pure_timing_strips';
export type ExtractionSubPhase =
  | 'probing'
  | 'timing'
  | 'local_ocr'
  | 'vlm_sheets'
  | 'finalizing';

export type ProviderType = 'minimax' | 'nvidia' | 'openai';
export type ThinkingMode = 'auto' | 'none' | 'low' | 'minimal' | 'default';
export type ReRecognizeCategory = 'dialogue' | 'bracket_note' | 'placeholder' | 'empty';
export type BatchExportConflictPolicy = 'overwrite' | 'rename' | 'skip';
export type BatchExportItemStatus = 'exported' | 'renamed' | 'skipped' | 'failed';

export type TaskStatus =
  | 'idle'
  | 'extracting'
  | 'ready'
  | 'exported'
  | 'cancelled'
  | 'failed';

// ==================== 2. 几何、媒体与能力 DTO ====================

/** 对应 C++ sublift::SourceBox（基于 probe_video 旋转后真实解码像素坐标系） */
export interface SourceBox {
  x: number; // >= 0
  y: number; // >= 0
  width: number; // >= 1, x + width <= srcW
  height: number; // >= 1, y + height <= srcH
}

export interface ContainRect {
  scale: number;
  drawW: number;
  drawH: number;
  offsetX: number;
  offsetY: number;
}

export interface Point2D {
  x: number;
  y: number;
}

export interface DisplayRect {
  left: number;
  top: number;
  width: number;
  height: number;
}

export interface CapabilitiesDto {
  engines: OcrEngineId[];
  ffmpeg_available: boolean;
  hwaccel_modes: string[];
}

/** 对应 C++ sublift::VideoInfo */
export interface VideoInfoDto {
  width: number;
  height: number;
  duration_ms: number;
  fps: number;
  display_transform_ok: boolean;
  transform_note?: string | null;
}

export interface FramePreviewDto {
  image_url: string;
  width: number; // 实际返回预览图宽（严禁用于 SourceBox 投影）
  height: number;
}

export interface SubtitleProfileDto {
  script: SubtitleScript;
  center_x: number;
  center_y: number;
  height: number;
  y_min: number;
  y_max: number;
}

/** 对应 C++ sublift::AutoRegionResult */
export interface AutoRegionDto {
  detected: boolean;
  region_box: SourceBox;
  profile?: SubtitleProfileDto | null;
  sample_timestamp_ms: number;
  preview_text: string;
  confidence: number;
  error_message: string;
}

// ==================== 3. 字幕条目、提取配置与流式事件 ====================

/** 对应 C++ sublift::SubtitleItem (SubtitleEntry) */
export interface SubtitleEntry {
  id: number;
  start_ms: number;
  end_ms: number;
  text: string;
  confidence: number; // 0.0..1.0；VLM 占位符 [待补录] 为 0.0
}

/** 前端校对扩展元数据（提交 finalize_srt 时映射为纯 SubtitleEntry[]） */
export interface ProofreadCue extends SubtitleEntry {
  local_uid: string;
  category?: ReRecognizeCategory;
  re_recognized?: boolean;
}

export interface ExtractionConfigDto {
  sample_fps: number;
  region_bottom_ratio: number;
  subtitle_script: SubtitleScript;
  confidence_threshold?: number | null;
  min_duration_ms?: number | null;
  merge_gap_ms?: number | null;
  predetected_profile?: SubtitleProfileDto | null;
}

export interface StartExtractionParams {
  video_path: string;
  mode: ExtractionMode;
  engine?: OcrEngineId | null;
  provider_id?: string | null;
  region_box?: SourceBox | null;
  config: ExtractionConfigDto;
  script: SubtitleScript;
}

/** 对应 C++ sublift::ExtractionMetrics */
export interface ExtractionMetricsDto {
  total_frames_processed: number;
  total_segments_detected: number;
  total_ocr_calls: number;
  total_entries_produced: number;
  wall_time_ms: number;
  effective_fps: number;
}

export interface ExtractionResultDto {
  entries: SubtitleEntry[];
  metrics: ExtractionMetricsDto;
  failed_slot_count?: number;
}

/** Tauri Channel<TaskEvent> 判别联合事件流 */
export type TaskEvent =
  | {
      type: 'phase';
      task_id: string;
      sub_phase: ExtractionSubPhase;
    }
  | {
      type: 'progress';
      task_id: string;
      current_ms: number;
      total_ms: number;
      fps: number;
    }
  | {
      type: 'subtitle_entry';
      task_id: string;
      entry: SubtitleEntry;
    }
  | {
      type: 'timing_slot';
      task_id: string;
      index: number;
      start_ms: number;
      end_ms: number;
      strip_path: string;
    }
  | {
      type: 'vlm_sheet_progress';
      task_id: string;
      completed_sheets: number;
      total_sheets: number;
      retried_splits: number;
    }
  | {
      type: 'completed';
      task_id: string;
      result: ExtractionResultDto;
    }
  | {
      type: 'cancelled';
      task_id: string;
    }
  | {
      type: 'failed';
      task_id: string;
      code: string;
      message: string;
      retryable: boolean;
    };

// ==================== 4. VLM Provider 与局部重识 ====================

export interface CustomHeaderDto {
  name: string;
  value: string;
}

export interface ProviderProfileMaskedDto {
  id: string;
  name: string;
  provider_type: ProviderType;
  base_url: string;
  model: string;
  thinking_mode: ThinkingMode;
  concurrency: number; // 1..16
  has_api_key: boolean;
  masked_api_key: string; // e.g. 'sk-****9f2a' 或 ''
  is_default: boolean;
  custom_headers?: CustomHeaderDto[];
}

export type ApiKeyUpdate =
  | { mode: 'keep' }
  | { mode: 'replace'; value: string }
  | { mode: 'clear' };

export interface ProviderUpsertDto {
  id?: string | null;
  name: string;
  provider_type: ProviderType;
  base_url: string;
  model: string;
  thinking_mode: ThinkingMode;
  concurrency: number;
  is_default: boolean;
  api_key_update: ApiKeyUpdate;
  custom_headers?: CustomHeaderDto[];
}

export interface ProviderConnectionTestDto {
  latency_ms: number;
  endpoint: string;
  model: string;
  response_preview: string;
}

export interface ReRecognizeItemInput {
  cue_id: number;
  start_ms: number;
  end_ms: number;
}

export interface ReRecognizeResult {
  cue_id: number;
  ok: boolean;
  text?: string;
  category?: ReRecognizeCategory;
  confidence?: number;
  error_message?: string;
}

// ==================== 5. 导出、批量导出与会话草稿 ====================

export interface MergeOptionsDto {
  min_duration_ms: number;
  merge_gap_ms: number;
  drop_empty_text: boolean;
}

export interface ExportResultDto {
  out_path: string;
  cue_count: number;
}

export interface BatchExportItemInput {
  video_path: string;
  out_path: string;
  entries: SubtitleEntry[];
}

export interface BatchExportItemResult {
  video_path: string;
  out_path: string;
  status: BatchExportItemStatus;
  cue_count: number;
  error_message?: string;
}

export interface BatchExportReport {
  results: BatchExportItemResult[];
  exported_count: number;
  skipped_count: number;
  failed_count: number;
}

export interface SessionDraftDto {
  video_path: string;
  engine: WorkbenchEngine;
  provider_id?: string | null;
  script: SubtitleScript;
  sample_fps: number;
  roi_mode: RoiMode;
  region_box?: SourceBox | null;
  status: Exclude<TaskStatus, 'extracting'>;
  entries: SubtitleEntry[];
  updated_at_ms: number;
}

export interface SessionSummaryDto {
  video_path: string;
  cue_count: number;
  status: Exclude<TaskStatus, 'extracting'>;
  updated_at_ms: number;
}

// ==================== 6. Tauri v2 IPC 命令宿主接口（19 个强类型命令） ====================

export interface IpcBridge {
  getCapabilities(): Promise<CapabilitiesDto>;
  probeVideo(videoPath: string): Promise<VideoInfoDto>;
  extractPreviewFrame(
    videoPath: string,
    timestampMs: number,
    crop?: SourceBox,
    maxWidth?: number
  ): Promise<FramePreviewDto>;
  detectAutoRegion(
    videoPath: string,
    engine: OcrEngineId,
    script: SubtitleScript,
    bottomRatio: number
  ): Promise<AutoRegionDto>;
  startExtraction(
    taskId: string,
    params: StartExtractionParams,
    onEvent: (event: TaskEvent) => void
  ): Promise<ExtractionResultDto>;
  cancelExtraction(taskId: string): Promise<void>;
  reRecognizeEntries(
    videoPath: string,
    regionBox: SourceBox | undefined,
    items: ReRecognizeItemInput[],
    providerId: string | undefined,
    script: SubtitleScript
  ): Promise<ReRecognizeResult[]>;
  exportSrt(
    outPath: string,
    entries: SubtitleEntry[],
    applyMerge: boolean,
    mergeOptions?: MergeOptionsDto
  ): Promise<ExportResultDto>;
  batchExportSrt(
    items: BatchExportItemInput[],
    conflictPolicy: BatchExportConflictPolicy
  ): Promise<BatchExportReport>;
  pickVideoFiles(): Promise<string[]>;
  pickSaveSrtPath(defaultName: string): Promise<string | null>;
  listProviders(): Promise<ProviderProfileMaskedDto[]>;
  saveProvider(profile: ProviderUpsertDto): Promise<ProviderProfileMaskedDto>;
  testProviderConnection(
    profile: ProviderUpsertDto
  ): Promise<ProviderConnectionTestDto>;
  deleteProvider(id: string): Promise<void>;
  importProvidersFromEnv(envPath?: string): Promise<ProviderProfileMaskedDto[]>;
  loadSessionDraft(videoPath: string): Promise<SessionDraftDto | null>;
  saveSessionDraft(draft: SessionDraftDto): Promise<void>;
  listRecentSessions(): Promise<SessionSummaryDto[]>;
  stopMediaStream(): Promise<void>;
}
