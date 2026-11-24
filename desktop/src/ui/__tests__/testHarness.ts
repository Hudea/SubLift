import { vi } from 'vitest';
import type {
  CapabilitiesDto,
  ExtractionMetricsDto,
  IpcBridge,
  ProviderProfileMaskedDto,
  VideoInfoDto,
} from '../../core/types';
import { useWorkbenchStore } from '../../store';

export const DEFAULT_TEST_METRICS: ExtractionMetricsDto = {
  total_frames_processed: 50,
  total_segments_detected: 2,
  total_ocr_calls: 2,
  total_entries_produced: 2,
  wall_time_ms: 200,
  effective_fps: 250,
};

export function makeTestVideoInfo(
  width: number = 1920,
  height: number = 1080,
  duration_ms: number = 60_000,
  display_transform_ok: boolean = true,
  transform_note: string | null = null
): VideoInfoDto {
  return {
    width,
    height,
    duration_ms,
    fps: 24,
    display_transform_ok,
    transform_note,
  };
}

export function createTestBridge(overrides?: Partial<IpcBridge>): IpcBridge {
  return {
    getCapabilities: vi.fn(async (): Promise<CapabilitiesDto> => ({
      engines: ['paddle'],
      ffmpeg_available: true,
      hwaccel_modes: [],
    })),
    probeVideo: vi.fn(async (path: string) =>
      path.includes('720p')
        ? makeTestVideoInfo(1280, 720)
        : makeTestVideoInfo(1920, 1080)
    ),
    extractPreviewFrame: vi.fn(async () => ({
      image_url: 'data:image/png;base64,AA==',
      width: 640,
      height: 360,
    })),
    detectAutoRegion: vi.fn(async () => ({
      detected: true,
      region_box: { x: 0, y: 800, width: 1920, height: 180 },
      profile: null,
      sample_timestamp_ms: 9500,
      preview_text: '测试样本',
      confidence: 0.92,
      error_message: '',
    })),
    startExtraction: vi.fn(async (taskId, _params, onEvent) => {
      const res = {
        entries: [
          { id: 1, start_ms: 1000, end_ms: 3000, text: '你好世界', confidence: 0.96 },
          { id: 2, start_ms: 4000, end_ms: 6000, text: '[待补录]', confidence: 0.0 },
        ],
        metrics: DEFAULT_TEST_METRICS,
      };
      onEvent({ type: 'completed', task_id: taskId, result: res });
      return res;
    }),
    cancelExtraction: vi.fn(async () => {}),
    reRecognizeEntries: vi.fn<IpcBridge['reRecognizeEntries']>(async (_vp, _rb, items) =>
      items.map((it) => ({
        cue_id: it.cue_id,
        ok: true,
        text: 'VLM 修正',
        category: 'dialogue' as const,
        confidence: 0.97,
      }))
    ),
    exportSrt: vi.fn<IpcBridge['exportSrt']>(async (outPath, entries) => ({
      out_path: outPath,
      cue_count: entries.length,
    })),
    batchExportSrt: vi.fn<IpcBridge['batchExportSrt']>(async (items) => ({
      results: items.map((i) => ({
        video_path: i.video_path,
        out_path: i.out_path,
        status: 'exported' as const,
        cue_count: i.entries.length,
      })),
      exported_count: items.length,
      skipped_count: 0,
      failed_count: 0,
    })),
    pickVideoFiles: vi.fn(async () => []),
    pickSaveSrtPath: vi.fn(async (name) => `/tmp/${name}`),
    listProviders: vi.fn(async (): Promise<ProviderProfileMaskedDto[]> => [
      {
        id: 'prov_1',
        name: 'MiniMax 主力',
        provider_type: 'minimax',
        base_url: 'https://api.minimax.cn/v1',
        model: 'MiniMax-M3.1-Flash',
        thinking_mode: 'auto',
        concurrency: 6,
        has_api_key: true,
        masked_api_key: 'sk-****9f2a',
        is_default: true,
      },
    ]),
    saveProvider: vi.fn(async (p) => ({
      id: p.id ?? 'prov_new',
      name: p.name,
      provider_type: p.provider_type,
      base_url: p.base_url,
      model: p.model,
      thinking_mode: p.thinking_mode,
      concurrency: p.concurrency,
      has_api_key: p.api_key_update.mode !== 'clear',
      masked_api_key: p.api_key_update.mode === 'clear' ? '' : 'sk-****9f2a',
      is_default: p.is_default,
      custom_headers: p.custom_headers ?? [],
    })),
    testProviderConnection: vi.fn(async (p) => ({
      latency_ms: 42,
      endpoint: p.base_url,
      model: p.model,
      response_preview: 'OK',
    })),
    deleteProvider: vi.fn(async () => {}),
    importProvidersFromEnv: vi.fn(async () => []),
    loadSessionDraft: vi.fn(async () => null),
    saveSessionDraft: vi.fn(async () => {}),
    listRecentSessions: vi.fn(async () => []),
    stopMediaStream: vi.fn(async () => {}),
    ...overrides,
  };
}

export function resetWorkbenchStore(overrides?: Partial<IpcBridge>): IpcBridge {
  const bridge = createTestBridge(overrides);
  useWorkbenchStore.getState().resetStore(bridge);
  return bridge;
}

export function installJsdomPolyfills() {
  globalThis.ResizeObserver = class ResizeObserver {
    private cb: ResizeObserverCallback;
    constructor(cb: ResizeObserverCallback) {
      this.cb = cb;
    }
    observe(target: Element) {
      this.cb(
        [
          {
            target,
            contentRect: {
              x: 0,
              y: 0,
              width: 800,
              height: 600,
              top: 0,
              left: 0,
              bottom: 600,
              right: 800,
              toJSON: () => ({}),
            },
            borderBoxSize: [{ inlineSize: 800, blockSize: 600 }],
            contentBoxSize: [{ inlineSize: 800, blockSize: 600 }],
            devicePixelContentBoxSize: [{ inlineSize: 800, blockSize: 600 }],
          },
        ],
        this
      );
    }
    unobserve() {}
    disconnect() {}
  };

  Object.defineProperty(HTMLImageElement.prototype, 'decode', {
    configurable: true,
    value: vi.fn(async () => {}),
  });

  Object.defineProperty(HTMLElement.prototype, 'scrollTo', {
    configurable: true,
    value: vi.fn(),
  });

  Object.defineProperty(HTMLElement.prototype, 'getBoundingClientRect', {
    configurable: true,
    value: function getBoundingClientRect(this: HTMLElement) {
      if (this.dataset?.testid === 'preview-stage' || this.dataset?.testid === 'roi-stage-overlay') {
        return {
          x: 0,
          y: 0,
          left: 0,
          top: 0,
          right: 800,
          bottom: 600,
          width: 800,
          height: 600,
          toJSON: () => ({}),
        };
      }
      if (this.dataset?.testid === 'proofreader-virtual-list') {
        return {
          x: 0,
          y: 0,
          left: 0,
          top: 0,
          right: 420,
          bottom: 500,
          width: 420,
          height: 500,
          toJSON: () => ({}),
        };
      }
      return {
        x: 0,
        y: 0,
        left: 0,
        top: 0,
        right: 420,
        bottom: 56,
        width: 420,
        height: 56,
        toJSON: () => ({}),
      };
    },
  });

  Object.defineProperty(HTMLElement.prototype, 'offsetHeight', {
    configurable: true,
    get(this: HTMLElement) {
      if (this.dataset?.testid === 'proofreader-virtual-list') {
        return 500;
      }
      return 56;
    },
  });

  Object.defineProperty(HTMLElement.prototype, 'clientHeight', {
    configurable: true,
    get(this: HTMLElement) {
      if (this.dataset?.testid === 'proofreader-virtual-list') {
        return 500;
      }
      return 56;
    },
  });
}
