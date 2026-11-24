import { Channel, invoke, isTauri } from '@tauri-apps/api/core';
import type {
  AutoRegionDto,
  BatchExportConflictPolicy,
  BatchExportItemInput,
  BatchExportReport,
  CapabilitiesDto,
  ExportResultDto,
  ExtractionResultDto,
  FramePreviewDto,
  IpcBridge,
  MergeOptionsDto,
  OcrEngineId,
  ProviderConnectionTestDto,
  ProviderProfileMaskedDto,
  ProviderUpsertDto,
  ReRecognizeItemInput,
  ReRecognizeResult,
  SessionDraftDto,
  SessionSummaryDto,
  SourceBox,
  StartExtractionParams,
  SubtitleEntry,
  SubtitleScript,
  TaskEvent,
  VideoInfoDto,
} from '../core/types';

export type { IpcBridge } from '../core/types';

export function isTauriRuntime(): boolean {
  try {
    return isTauri();
  } catch {
    return false;
  }
}

export interface ParsedIpcError {
  code: string;
  message: string;
}

/**
 * 解析 Rust 宿主统一返回的 "[ErrorCode] message" 错误字符串。
 */
export function parseIpcError(err: unknown): ParsedIpcError {
  const raw =
    err instanceof Error
      ? err.message
      : typeof err === 'string'
        ? err
        : JSON.stringify(err) || '未知错误';
  const match = /^\[([A-Za-z0-9_]+)\]\s*(.*)$/s.exec(raw.trim());
  if (match) {
    return {
      code: match[1],
      message: match[2] || raw,
    };
  }
  return {
    code: 'IpcError',
    message: raw,
  };
}

/**
 * 真实 Tauri v2 IPC 桥接：全部 19 个命令参数键严格采用 snake_case，与 commands/mod.rs 对齐。
 */
export function createTauriBridge(): IpcBridge {
  return {
    getCapabilities(): Promise<CapabilitiesDto> {
      return invoke<CapabilitiesDto>('get_capabilities');
    },

    probeVideo(videoPath: string): Promise<VideoInfoDto> {
      return invoke<VideoInfoDto>('probe_video', { video_path: videoPath });
    },

    extractPreviewFrame(
      videoPath: string,
      timestampMs: number,
      crop?: SourceBox,
      maxWidth?: number
    ): Promise<FramePreviewDto> {
      return invoke<FramePreviewDto>('extract_preview_frame', {
        video_path: videoPath,
        timestamp_ms: timestampMs,
        crop: crop ?? null,
        max_width: maxWidth ?? null,
      });
    },

    detectAutoRegion(
      videoPath: string,
      engine: OcrEngineId,
      script: SubtitleScript,
      bottomRatio: number
    ): Promise<AutoRegionDto> {
      return invoke<AutoRegionDto>('detect_auto_region', {
        video_path: videoPath,
        engine,
        script,
        bottom_ratio: bottomRatio,
      });
    },

    startExtraction(
      taskId: string,
      params: StartExtractionParams,
      onEvent: (event: TaskEvent) => void
    ): Promise<ExtractionResultDto> {
      const channel = new Channel<TaskEvent>();
      channel.onmessage = onEvent;
      return invoke<ExtractionResultDto>('start_extraction', {
        task_id: taskId,
        params,
        on_event: channel,
      });
    },

    cancelExtraction(taskId: string): Promise<void> {
      return invoke<void>('cancel_extraction', { task_id: taskId });
    },

    reRecognizeEntries(
      videoPath: string,
      regionBox: SourceBox | undefined,
      items: ReRecognizeItemInput[],
      providerId: string | undefined,
      script: SubtitleScript
    ): Promise<ReRecognizeResult[]> {
      return invoke<ReRecognizeResult[]>('re_recognize_entries', {
        video_path: videoPath,
        region_box: regionBox ?? null,
        items,
        provider_id: providerId ?? null,
        script,
      });
    },

    exportSrt(
      outPath: string,
      entries: SubtitleEntry[],
      applyMerge: boolean,
      mergeOptions?: MergeOptionsDto
    ): Promise<ExportResultDto> {
      return invoke<ExportResultDto>('export_srt', {
        out_path: outPath,
        entries,
        apply_merge: applyMerge,
        merge_options: mergeOptions ?? null,
      });
    },

    batchExportSrt(
      items: BatchExportItemInput[],
      conflictPolicy: BatchExportConflictPolicy
    ): Promise<BatchExportReport> {
      return invoke<BatchExportReport>('batch_export_srt', {
        items,
        conflict_policy: conflictPolicy,
      });
    },

    pickVideoFiles(): Promise<string[]> {
      return invoke<string[]>('pick_video_files');
    },

    pickSaveSrtPath(defaultName: string): Promise<string | null> {
      return invoke<string | null>('pick_save_srt_path', {
        default_name: defaultName,
      });
    },

    listProviders(): Promise<ProviderProfileMaskedDto[]> {
      return invoke<ProviderProfileMaskedDto[]>('list_providers');
    },

    saveProvider(profile: ProviderUpsertDto): Promise<ProviderProfileMaskedDto> {
      return invoke<ProviderProfileMaskedDto>('save_provider', { profile });
    },

    testProviderConnection(
      profile: ProviderUpsertDto
    ): Promise<ProviderConnectionTestDto> {
      return invoke<ProviderConnectionTestDto>('test_provider_connection', {
        profile,
      });
    },

    deleteProvider(id: string): Promise<void> {
      return invoke<void>('delete_provider', { id });
    },

    importProvidersFromEnv(envPath?: string): Promise<ProviderProfileMaskedDto[]> {
      return invoke<ProviderProfileMaskedDto[]>('import_providers_from_env', {
        env_path: envPath ?? null,
      });
    },

    loadSessionDraft(videoPath: string): Promise<SessionDraftDto | null> {
      return invoke<SessionDraftDto | null>('load_session_draft', {
        video_path: videoPath,
      });
    },

    saveSessionDraft(draft: SessionDraftDto): Promise<void> {
      return invoke<void>('save_session_draft', { draft });
    },

    listRecentSessions(): Promise<SessionSummaryDto[]> {
      return invoke<SessionSummaryDto[]>('list_recent_sessions');
    },

    stopMediaStream(): Promise<void> {
      return invoke<void>('stop_media_stream');
    },
  };
}
