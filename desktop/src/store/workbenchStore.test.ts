import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const { tauriInvokeMock, tauriIsTauriMock, createdChannels } = vi.hoisted(() => ({
  tauriInvokeMock: vi.fn<(...args: unknown[]) => Promise<unknown>>(async () => undefined),
  tauriIsTauriMock: vi.fn(() => true),
  createdChannels: [] as Array<{ onmessage: ((ev: unknown) => void) | null }>,
}));

vi.mock('@tauri-apps/api/core', () => ({
  invoke: tauriInvokeMock,
  isTauri: tauriIsTauriMock,
  Channel: class MockChannel<T> {
    onmessage: ((ev: T) => void) | null = null;
    constructor() {
      createdChannels.push(this as unknown as { onmessage: ((ev: unknown) => void) | null });
    }
  },
}));

import type {
  AutoRegionDto,
  BatchExportReport,
  CapabilitiesDto,
  ExtractionMetricsDto,
  ExtractionResultDto,
  IpcBridge,
  OcrEngineId,
  ProviderProfileMaskedDto,
  ReRecognizeResult,
  SessionDraftDto,
  TaskEvent,
  VideoInfoDto,
} from '../core/types';
import { createTauriBridge, isTauriRuntime, parseIpcError } from './bridge';
import {
  selectActiveCommands,
  selectActiveTask,
  selectEffectiveProvider,
  selectLowConfCount,
  selectLowConfCues,
} from './selectors';
import { createWorkbenchStore } from './workbenchStore';

const DEFAULT_METRICS: ExtractionMetricsDto = {
  total_frames_processed: 100,
  total_segments_detected: 2,
  total_ocr_calls: 2,
  total_entries_produced: 2,
  wall_time_ms: 420,
  effective_fps: 238.1,
};

function makeVideoInfo(w: number, h: number, durationMs: number = 60_000): VideoInfoDto {
  return {
    width: w,
    height: h,
    duration_ms: durationMs,
    fps: 24,
    display_transform_ok: true,
    transform_note: null,
  };
}

function createMockBridge(overrides?: Partial<IpcBridge>): IpcBridge {
  return {
    getCapabilities: vi.fn(async (): Promise<CapabilitiesDto> => ({
      engines: ['paddle'],
      ffmpeg_available: true,
      hwaccel_modes: [],
    })),
    probeVideo: vi.fn(async (path: string) =>
      path.includes('720p') ? makeVideoInfo(1280, 720) : makeVideoInfo(1920, 1080)
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
      sample_timestamp_ms: 12_340,
      preview_text: '自动探测样本',
      confidence: 0.94,
      error_message: '',
    })),
    startExtraction: vi.fn(async (taskId, _params, onEvent) => {
      onEvent({ type: 'phase', task_id: taskId, sub_phase: 'timing' });
      onEvent({
        type: 'progress',
        task_id: taskId,
        current_ms: 30_000,
        total_ms: 60_000,
        fps: 120,
      });
      const result = {
        entries: [
          {
            id: 1,
            start_ms: 1000,
            end_ms: 3000,
            text: '第一句',
            confidence: 0.95,
          },
          {
            id: 2,
            start_ms: 4000,
            end_ms: 6000,
            text: '模糊识别',
            confidence: 0.42,
          },
        ],
        metrics: DEFAULT_METRICS,
      };
      onEvent({ type: 'completed', task_id: taskId, result });
      return result;
    }),
    cancelExtraction: vi.fn(async () => {}),
    reRecognizeEntries: vi.fn<IpcBridge['reRecognizeEntries']>(async (_vp, _rb, items) =>
      items.map((item) => ({
        cue_id: item.cue_id,
        ok: true,
        text: 'VLM 重识结果',
        category: 'dialogue' as const,
        confidence: 0.98,
      }))
    ),
    exportSrt: vi.fn<IpcBridge['exportSrt']>(async (outPath, entries) => ({
      out_path: outPath,
      cue_count: entries.length,
    })),
    batchExportSrt: vi.fn<IpcBridge['batchExportSrt']>(async (items) => ({
      results: items.map((it) => ({
        video_path: it.video_path,
        out_path: it.out_path,
        status: 'exported' as const,
        cue_count: it.entries.length,
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
    saveProvider: vi.fn(async (profile) => ({
      id: profile.id ?? 'prov_new',
      name: profile.name,
      provider_type: profile.provider_type,
      base_url: profile.base_url,
      model: profile.model,
      thinking_mode: profile.thinking_mode,
      concurrency: profile.concurrency,
      has_api_key: profile.api_key_update.mode !== 'clear',
      masked_api_key:
        profile.api_key_update.mode === 'clear' ? '' : 'sk-****9f2a',
      is_default: profile.is_default,
      custom_headers: profile.custom_headers ?? [],
    })),
    testProviderConnection: vi.fn(async (profile) => ({
      latency_ms: 42,
      endpoint: profile.base_url,
      model: profile.model,
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

describe('store/workbenchStore', () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('auto-collapses queue for single video, expands for multiple, and deduplicates paths', async () => {
    const bridge = createMockBridge();
    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();

    await store.getState().addVideoPaths(['/videos/ep01.mkv']);
    expect(store.getState().tasks).toHaveLength(1);
    expect(store.getState().queueCollapsed).toBe(true);

    // Duplicate path ignored, 2nd distinct path expands sidebar
    await store.getState().addVideoPaths(['/videos/ep01.mkv', '/videos/ep02_720p.mkv']);
    expect(store.getState().tasks).toHaveLength(2);
    expect(store.getState().queueCollapsed).toBe(false);
  });

  it('preserves background TaskEvent progress via per-item runGen when switching activeTaskId', async () => {
    let emitTask1!: (ev: TaskEvent) => void;
    let resolveTask1!: (val: Awaited<ReturnType<IpcBridge['startExtraction']>>) => void;

    const bridge = createMockBridge({
      startExtraction: vi.fn<IpcBridge['startExtraction']>(
        (taskId, _params, onEvent) =>
          new Promise<ExtractionResultDto>((resolve) => {
            if (taskId.startsWith('task_')) {
              emitTask1 = onEvent;
              resolveTask1 = resolve;
            }
          })
      ),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv', '/v/ep02.mkv']);

    const [t1, t2] = store.getState().tasks;
    const extractionPromise = store.getState().startTaskExtraction(t1.id);

    // 用户立即切换活动任务至 ep02
    store.getState().selectActiveTask(t2.id);
    expect(store.getState().activeTaskId).toBe(t2.id);

    // 冲刷默认 auto 模式前置探测微任务，等待 bridge.startExtraction 挂接回调
    await Promise.resolve();
    await Promise.resolve();
    expect(emitTask1).toBeTypeOf('function');

    // 后台 ep01 仍在推送事件：runGen 隔离确保 ep01 正常接收并收敛为 ready
    emitTask1({ type: 'phase', task_id: `${t1.id}:1`, sub_phase: 'local_ocr' });
    emitTask1({
      type: 'subtitle_entry',
      task_id: `${t1.id}:1`,
      entry: { id: 1, start_ms: 0, end_ms: 1000, text: '流式字幕', confidence: 0.9 },
    });
    expect(store.getState().tasks[0].subPhase).toBe('local_ocr');
    expect(store.getState().tasks[0].streamingPreviewCues).toHaveLength(1);

    const doneResult = {
      entries: [{ id: 1, start_ms: 0, end_ms: 1000, text: '最终合并字幕', confidence: 0.92 }],
      metrics: DEFAULT_METRICS,
    };
    emitTask1({ type: 'completed', task_id: `${t1.id}:1`, result: doneResult });
    resolveTask1(doneResult);
    await extractionPromise;

    expect(store.getState().tasks[0].status).toBe('ready');
    expect(store.getState().tasks[0].cues[0].text).toBe('最终合并字幕');
    expect(store.getState().tasks[1].status).toBe('idle');
  });

  it('mints distinct backend runTaskId on retry after cancel and freezes activeRunConfig', async () => {
    const startMock = vi.fn(
      async (taskId: string, params: Parameters<IpcBridge['startExtraction']>[1], onEvent: (e: TaskEvent) => void) => {
        if (taskId.endsWith(':1')) {
          onEvent({ type: 'cancelled', task_id: taskId });
          throw new Error('[Aborted] cancelled');
        }
        expect(params.config.subtitle_script).toBe(params.script);
        const res = { entries: [], metrics: DEFAULT_METRICS };
        onEvent({ type: 'completed', task_id: taskId, result: res });
        return res;
      }
    );

    const bridge = createMockBridge({ startExtraction: startMock });
    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv']);

    const t1 = store.getState().tasks[0];
    store.getState().setTaskScript('cjk', t1.id);
    await store.getState().startTaskExtraction(t1.id);
    expect(store.getState().tasks[0].status).toBe('cancelled');

    await store.getState().startTaskExtraction(t1.id);
    expect(store.getState().tasks[0].status).toBe('ready');
    expect(startMock.mock.calls[0][0]).not.toBe(startMock.mock.calls[1][0]);
  });

  it('falls back to local paddle engine for detectAutoRegion when vlm is active', async () => {
    const bridge = createMockBridge();
    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv']);

    store.getState().setTaskEngine('vlm');
    await store.getState().detectAutoRegionForTask();

    expect(bridge.detectAutoRegion).toHaveBeenCalledWith(
      '/v/ep01.mkv',
      'paddle',
      'auto',
      0.45
    );
    expect(store.getState().tasks[0].previewTimestampMs).toBe(12_340);
    expect(store.getState().tasks[0].regionBox).toEqual({
      x: 0,
      y: 800,
      width: 1920,
      height: 180,
    });
  });

  it('applies config to queue with cross-res scaling and protects completed items unless confirmed', async () => {
    const bridge = createMockBridge();
    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths([
      '/v/ep01.mkv',
      '/v/ep02.mkv',
      '/v/ep03_720p.mkv',
      '/v/ep04.mkv',
    ]);

    const [t1, t2, t3, t4] = store.getState().tasks;
    store.getState().setTaskRegionBox({ x: 0, y: 756, width: 1920, height: 324 }, t1.id);
    store.getState().setTaskEngine('vlm', t1.id);
    // Mark t4 ready
    store.setState((s) => ({
      tasks: s.tasks.map((t) => (t.id === t4.id ? { ...t, status: 'ready' } : t)),
    }));

    const summary = store.getState().applyConfigToQueue({
      sourceTaskId: t1.id,
      includeCompleted: false,
    });
    expect(summary.updatedCount).toBe(2);
    expect(summary.skippedCompletedCount).toBe(1);

    const updatedT2 = store.getState().tasks.find((t) => t.id === t2.id)!;
    const updatedT3 = store.getState().tasks.find((t) => t.id === t3.id)!;
    const preservedT4 = store.getState().tasks.find((t) => t.id === t4.id)!;

    expect(updatedT2.regionBox).toEqual({ x: 0, y: 756, width: 1920, height: 324 });
    expect(updatedT2.roiScaledFromQueue).toBe(false);

    expect(updatedT3.regionBox).toEqual({ x: 0, y: 504, width: 1280, height: 216 });
    expect(updatedT3.roiScaledFromQueue).toBe(true);

    expect(preservedT4.status).toBe('ready');

    // Now confirm includeCompleted: true
    store.getState().applyConfigToQueue({
      sourceTaskId: t1.id,
      includeCompleted: true,
    });
    const resetT4 = store.getState().tasks.find((t) => t.id === t4.id)!;
    expect(resetT4.status).toBe('idle');
    expect(resetT4.engine).toBe('vlm');
  });

  it('runs sequential queue extraction past failed items and debounces draft saves excluding extracting', async () => {
    const bridge = createMockBridge({
      startExtraction: vi.fn(async (taskId, params, onEvent) => {
        if (params.video_path.includes('fail')) {
          onEvent({
            type: 'failed',
            task_id: taskId,
            code: 'DecoderFailed',
            message: 'corrupt',
            retryable: true,
          });
          throw new Error('[DecoderFailed] corrupt');
        }
        const res = {
          entries: [{ id: 1, start_ms: 0, end_ms: 1000, text: 'OK', confidence: 0.9 }],
          metrics: DEFAULT_METRICS,
        };
        onEvent({ type: 'completed', task_id: taskId, result: res });
        return res;
      }),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01_fail.mkv', '/v/ep02_ok.mkv']);

    await store.getState().startQueueExtraction();
    expect(store.getState().tasks[0].status).toBe('failed');
    expect(store.getState().tasks[1].status).toBe('ready');

    // Debounced draft save check
    store.getState().setTaskSampleFps(8.0, store.getState().tasks[1].id);
    await vi.advanceTimersByTimeAsync(520);
    expect(bridge.saveSessionDraft).toHaveBeenCalled();
    const calls = vi.mocked(bridge.saveSessionDraft).mock.calls;
    for (const [draft] of calls) {
      expect((draft as SessionDraftDto).status).not.toBe('extracting');
    }
  });

  it('warns on unsupported apple_vision draft restore without silent downgrade', async () => {
    const bridge = createMockBridge({
      loadSessionDraft: vi.fn(async (videoPath) => ({
        video_path: videoPath,
        engine: 'apple_vision' as const,
        provider_id: null,
        script: 'cjk' as const,
        sample_fps: 6,
        roi_mode: 'band' as const,
        region_box: { x: 0, y: 700, width: 1920, height: 200 },
        status: 'idle' as const,
        entries: [],
        updated_at_ms: 1000,
      })),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv']);

    const item = store.getState().tasks[0];
    expect(item.engine).toBe('apple_vision');
    expect(item.capabilityWarning).toContain('Apple Vision');
    expect(selectActiveCommands(store.getState()).startExtraction.enabled).toBe(
      false
    );

    store.getState().setTaskEngine('paddle', item.id);
    expect(store.getState().tasks[0].capabilityWarning).toBeNull();
    expect(selectActiveCommands(store.getState()).startExtraction.enabled).toBe(
      true
    );
  });

  it('applies atomic single-undo patch on partial reRecognizeEntries and exports with apply_merge: false', async () => {
    const bridge = createMockBridge({
      reRecognizeEntries: vi.fn<IpcBridge['reRecognizeEntries']>(async (_vp, _rb, items) =>
        items.map((it, idx) =>
          idx === 0
            ? {
                cue_id: it.cue_id,
                ok: true,
                text: '重识修正',
                category: 'dialogue' as const,
                confidence: 0.99,
              }
            : {
                cue_id: it.cue_id,
                ok: false,
                error_message: 'HTTP 429',
              }
        )
      ),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv']);
    await store.getState().startTaskExtraction();

    // Mark exported first, then re-recognize -> transitions exported -> ready
    await store.getState().exportActiveSrt('/tmp/ep01.srt');
    expect(bridge.exportSrt).toHaveBeenCalledWith(
      '/tmp/ep01.srt',
      expect.any(Array),
      false
    );
    expect(store.getState().tasks[0].status).toBe('exported');

    const uids = store.getState().tasks[0].cues.map((c) => c.local_uid);
    const res = await store.getState().reRecognizeCues(uids);
    expect(res).toEqual({ ok: false, succeeded: 1, failed: 1 });
    expect(store.getState().tasks[0].status).toBe('ready');
    expect(store.getState().tasks[0].cues[0].text).toBe('重识修正');
    expect(store.getState().tasks[0].cues[1].text).toBe('模糊识别');
    expect(store.getState().toasts.at(-1)?.severity).toBe('warning');

    // Single undo reverts atomic batch patch
    store.getState().undo();
    expect(store.getState().tasks[0].cues[0].text).toBe('第一句');

    // Batch export also passes sibling .srt paths and invokes stopMediaStream on playback stop
    store.getState().setPlaybackMode('playing');
    expect(selectActiveTask(store.getState())?.playbackMode).toBe('playing');
    store.getState().stepPreview(100);
    expect(bridge.stopMediaStream).toHaveBeenCalled();

    const report = await store.getState().batchExportSrt('rename', false);
    expect(report?.exported_count).toBe(1);
    expect(bridge.batchExportSrt).toHaveBeenCalledWith(
      [
        expect.objectContaining({
          video_path: '/v/ep01.mkv',
          out_path: '/v/ep01.srt',
        }),
      ],
      'rename'
    );
  });

  it('manages provider keep/replace/clear and env import cleanly', async () => {
    const bridge = createMockBridge();
    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();

    await store.getState().saveProvider({
      id: 'prov_1',
      name: 'MiniMax 主力',
      provider_type: 'minimax',
      base_url: 'https://api.minimax.cn/v1',
      model: 'MiniMax-M3.1-Flash',
      thinking_mode: 'low',
      concurrency: 8,
      is_default: true,
      api_key_update: { mode: 'keep' },
    });
    expect(bridge.saveProvider).toHaveBeenCalledWith(
      expect.objectContaining({
        api_key_update: { mode: 'keep' },
      })
    );

    await store.getState().deleteProvider('prov_1');
    expect(bridge.deleteProvider).toHaveBeenCalledWith('prov_1');

    await store.getState().importProvidersFromEnv('/tmp/.env');
    expect(bridge.importProvidersFromEnv).toHaveBeenCalledWith('/tmp/.env');
  });

  it('recovers selectLowConfCues after activeTask becomes null and returns to the same cue list', async () => {
    const bridge = createMockBridge();
    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv']);
    await store.getState().startTaskExtraction();

    const t1Id = store.getState().tasks[0].id;
    expect(selectLowConfCues(store.getState())).toHaveLength(1);

    // activeTaskId -> null -> back to t1Id with identical cues reference
    store.setState({ activeTaskId: null });
    expect(selectLowConfCues(store.getState())).toEqual([]);

    store.setState({ activeTaskId: t1Id });
    expect(selectLowConfCues(store.getState())).toHaveLength(1);
  });

  it('prevents delayed loadSessionDraft from clobbering concurrent user edits or active extraction', async () => {
    let resolveDraft!: (draft: SessionDraftDto | null) => void;
    const bridge = createMockBridge({
      loadSessionDraft: vi.fn(
        () =>
          new Promise<SessionDraftDto | null>((resolve) => {
            resolveDraft = resolve;
          })
      ),
    });
    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();

    const addPromise = store.getState().addVideoPaths(['/v/ep01.mkv']);
    // Wait a microtask so probeVideo completes and loadSessionDraft is awaiting
    await Promise.resolve();
    await Promise.resolve();

    // User changes engine to 'vlm' before delayed draft returns
    store.getState().setTaskEngine('vlm');
    resolveDraft({
      video_path: '/v/ep01.mkv',
      engine: 'paddle',
      provider_id: null,
      script: 'latin',
      sample_fps: 2,
      roi_mode: 'band',
      region_box: { x: 0, y: 700, width: 1920, height: 200 },
      status: 'ready',
      entries: [{ id: 9, start_ms: 0, end_ms: 500, text: 'stale draft', confidence: 0.9 }],
      updated_at_ms: 500,
    });
    await addPromise;

    const item = store.getState().tasks[0];
    expect(item.engine).toBe('vlm');
    expect(item.status).toBe('idle');
    expect(item.cues).toHaveLength(0);
  });

  it('falls back to local paddle engine for detectAutoRegion when unsupported apple_vision draft is active and reconciles stale provider_id', async () => {
    const bridge = createMockBridge({
      loadSessionDraft: vi.fn(async (videoPath) => ({
        video_path: videoPath,
        engine: 'apple_vision' as const,
        provider_id: 'deleted_provider_id',
        script: 'auto' as const,
        sample_fps: 5,
        roi_mode: 'band' as const,
        region_box: { x: 0, y: 756, width: 1920, height: 324 },
        status: 'idle' as const,
        entries: [],
        updated_at_ms: 1000,
      })),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv']);

    // Unsupported apple_vision still warns and blocks startExtraction, but detectAutoRegion falls back to 'paddle'
    await store.getState().detectAutoRegionForTask();
    expect(bridge.detectAutoRegion).toHaveBeenCalledWith(
      '/v/ep01.mkv',
      'paddle',
      'auto',
      0.45
    );

    // Stale provider_id reconciled to prov_1, so switching to VLM enables startExtraction immediately
    expect(store.getState().tasks[0].providerId).toBe('prov_1');
    store.getState().setTaskEngine('vlm');
    expect(selectActiveCommands(store.getState()).startExtraction.enabled).toBe(true);
  });

  it('prevents exportActiveSrt from clobbering an overlapping extraction back to exported', async () => {
    let resolveExport!: (res: { out_path: string; cue_count: number }) => void;
    let resolveExtract!: (res: ExtractionResultDto) => void;
    let extractCall = 0;

    const bridge = createMockBridge({
      exportSrt: vi.fn(
        () =>
          new Promise<{ out_path: string; cue_count: number }>((resolve) => {
            resolveExport = resolve;
          })
      ),
      startExtraction: vi.fn((taskId, _params, onEvent) => {
        extractCall += 1;
        if (extractCall === 1) {
          const res = {
            entries: [{ id: 1, start_ms: 0, end_ms: 1000, text: '初始', confidence: 0.9 }],
            metrics: DEFAULT_METRICS,
          };
          onEvent({ type: 'completed', task_id: taskId, result: res });
          return Promise.resolve(res);
        }
        return new Promise<ExtractionResultDto>((resolve) => {
          resolveExtract = resolve;
        });
      }),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv']);
    await store.getState().startTaskExtraction();

    const exportPromise = store.getState().exportActiveSrt('/tmp/ep01.srt');
    const reExtractPromise = store.getState().startTaskExtraction();
    expect(store.getState().tasks[0].status).toBe('extracting');

    resolveExport({ out_path: '/tmp/ep01.srt', cue_count: 1 });
    await exportPromise;
    expect(store.getState().tasks[0].status).toBe('extracting');

    resolveExtract({ entries: [], metrics: DEFAULT_METRICS });
    await reExtractPromise;
    expect(store.getState().tasks[0].status).toBe('ready');
  });

  it('clamps activeMatchIndex on replaceCurrentMatch, scopes queueRunGen, clears dirty state on removeTask, and syncs streaming cue/playback', async () => {
    let resolveExtract1!: (res: ExtractionResultDto) => void;
    let extractCall = 0;
    const bridge = createMockBridge({
      startExtraction: vi.fn((taskId, _params, onEvent) => {
        extractCall += 1;
        if (extractCall === 1) {
          const res = {
            entries: [
              { id: 1, start_ms: 0, end_ms: 1000, text: 'foo one', confidence: 0.95 },
              { id: 2, start_ms: 2000, end_ms: 3000, text: 'foo two', confidence: 0.95 },
            ],
            metrics: DEFAULT_METRICS,
          };
          onEvent({ type: 'completed', task_id: taskId, result: res });
          return Promise.resolve(res);
        }
        if (extractCall === 2) {
          return new Promise<ExtractionResultDto>((resolve) => {
            resolveExtract1 = resolve;
          });
        }
        const res = {
          entries: [{ id: 1, start_ms: 0, end_ms: 1000, text: 'done', confidence: 0.95 }],
          metrics: DEFAULT_METRICS,
        };
        onEvent({ type: 'completed', task_id: taskId, result: res });
        return Promise.resolve(res);
      }),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv', '/v/ep02.mkv', '/v/ep03.mkv']);
    await store.getState().startTaskExtraction();

    // replaceCurrentMatch on last match clamps activeMatchIndex
    store.getState().openFind(true);
    store.getState().setFindState({ query: 'foo', replacement: 'bar' });
    store.getState().navigateFindMatch('next'); // index 0 -> 1
    expect(store.getState().findReplace.activeMatchIndex).toBe(1);
    store.getState().replaceCurrentMatch();
    expect(store.getState().findReplace.activeMatchIndex).toBe(0);

    // removeTask resets inline edit lock when removing active task
    store.getState().setDirtyInlineEdit(true, true, 'cue_1_0');
    store.getState().removeTask(store.getState().tasks[0].id);
    expect(store.getState().isDirtyInlineEdit).toBe(false);
    expect(store.getState().isInlineTextFocused).toBe(false);
    expect(store.getState().editingCueUid).toBeNull();

    // syncPlaybackTime updates previewTimestampMs without stopping Tier 2 or resetting streamBaseMs
    store.getState().seekPreview(10_000);
    store.getState().setPlaybackMode('playing');
    store.getState().syncPlaybackTime(15_250);
    expect(selectActiveTask(store.getState())?.playbackMode).toBe('playing');
    expect(selectActiveTask(store.getState())?.streamBaseMs).toBe(10_000);
    expect(selectActiveTask(store.getState())?.previewTimestampMs).toBe(15_250);

    // queueRunGen protects new queue loop from old loop's finally after cancelAllExtraction
    const loop1 = store.getState().startQueueExtraction();
    await Promise.resolve();
    await store.getState().cancelAllExtraction();
    const loop2 = store.getState().startQueueExtraction();
    expect(store.getState().queueRunActive).toBe(true);
    resolveExtract1({ entries: [], metrics: DEFAULT_METRICS });
    await Promise.all([loop1, loop2]);
    expect(store.getState().queueRunActive).toBe(false);
  });

  it('keeps status at ready when commitCueText mutates cues while exportActiveSrt or batchExportSrt is in flight', async () => {
    let resolveExport!: (res: { out_path: string; cue_count: number }) => void;
    let resolveBatch!: (res: BatchExportReport) => void;

    const bridge = createMockBridge({
      exportSrt: vi.fn(
        () =>
          new Promise<{ out_path: string; cue_count: number }>((resolve) => {
            resolveExport = resolve;
          })
      ),
      batchExportSrt: vi.fn(
        () =>
          new Promise<BatchExportReport>((resolve) => {
            resolveBatch = resolve;
          })
      ),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv', '/v/ep02.mkv']);
    await store.getState().startTaskExtraction(store.getState().tasks[0].id);
    await store.getState().startTaskExtraction(store.getState().tasks[1].id);

    const t1 = store.getState().tasks[0];
    const t2 = store.getState().tasks[1];
    store.getState().selectActiveTask(t1.id);

    // 1) exportActiveSrt overlapping commitCueText
    const exportPromise = store.getState().exportActiveSrt('/tmp/ep01.srt');
    store.getState().commitCueText(t1.cues[0].local_uid, '导出期间修改');
    resolveExport({ out_path: '/tmp/ep01.srt', cue_count: 2 });
    await exportPromise;
    expect(store.getState().tasks[0].status).toBe('ready');

    // 2) batchExportSrt overlapping commitCueText on ep01 while ep02 remains unchanged
    const batchPromise = store.getState().batchExportSrt('overwrite', false);
    store.getState().commitCueText(t1.cues[0].local_uid, '批量导出期间修改');
    resolveBatch({
      results: [
        {
          video_path: '/v/ep01.mkv',
          out_path: '/v/ep01.srt',
          status: 'exported',
          cue_count: 2,
        },
        {
          video_path: '/v/ep02.mkv',
          out_path: '/v/ep02.srt',
          status: 'exported',
          cue_count: 2,
        },
      ],
      exported_count: 2,
      skipped_count: 0,
      failed_count: 0,
    });
    await batchPromise;
    expect(store.getState().tasks.find((t) => t.id === t1.id)?.status).toBe('ready');
    expect(store.getState().tasks.find((t) => t.id === t2.id)?.status).toBe('exported');
  });

  it('prevents reRecognizeCues from racing with startTaskExtraction/applyConfigToQueue or overwriting extracting/new cues', async () => {
    let resolveReRec!: (res: ReRecognizeResult[]) => void;
    let extractCall = 0;

    const bridge = createMockBridge({
      startExtraction: vi.fn((taskId, _params, onEvent) => {
        extractCall += 1;
        const text = extractCall === 1 ? 'old low' : 'brand new cue';
        const res = {
          entries: [{ id: 1, start_ms: 0, end_ms: 1000, text, confidence: 0.4 }],
          metrics: DEFAULT_METRICS,
        };
        onEvent({ type: 'completed', task_id: taskId, result: res });
        return Promise.resolve(res);
      }),
      reRecognizeEntries: vi.fn(
        () =>
          new Promise<ReRecognizeResult[]>((resolve) => {
            resolveReRec = resolve;
          })
      ),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv', '/v/ep02.mkv']);
    const [t1, t2] = store.getState().tasks;
    await store.getState().startTaskExtraction(t1.id);

    const uid = store.getState().tasks[0].cues[0].local_uid;
    const reRecPromise1 = store.getState().reRecognizeCues([uid]);
    expect(store.getState().tasks[0].isReRecognizing).toBe(true);

    // While isReRecognizing is active, startTaskExtraction & applyConfigToQueue are gated
    await store.getState().startTaskExtraction(t1.id);
    expect(extractCall).toBe(1);
    const applySummary = store.getState().applyConfigToQueue({
      sourceTaskId: t2.id,
      includeCompleted: true,
    });
    expect(applySummary.updatedCount).toBe(0);
    expect(applySummary.skippedExtractingCount).toBe(1);

    // Even if task state transitions to 'extracting' externally while reRecognizeEntries is in flight,
    // stale VLM result refuses to overwrite cues
    store.setState((s) => ({
      tasks: s.tasks.map((t) =>
        t.id === t1.id ? { ...t, status: 'extracting' } : t
      ),
    }));
    resolveReRec([
      {
        cue_id: 1,
        ok: true,
        text: 'STALE_VLM_PATCH',
        category: 'dialogue',
        confidence: 0.99,
      },
    ]);
    const res1 = await reRecPromise1;
    expect(res1.ok).toBe(false);
    expect(store.getState().tasks[0].cues[0].text).toBe('old low');
    expect(store.getState().tasks[0].isReRecognizing).toBe(false);

    // Also test stale VLM patch when a new extraction replaced cues before reRecognizeEntries resolved
    store.setState((s) => ({
      tasks: s.tasks.map((t) =>
        t.id === t1.id ? { ...t, status: 'ready' } : t
      ),
    }));
    const reRecPromise2 = store.getState().reRecognizeCues([uid]);
    store.setState((s) => ({
      tasks: s.tasks.map((t) =>
        t.id === t1.id
          ? {
              ...t,
              status: 'ready',
              cues: [
                {
                  id: 1,
                  local_uid: 'cue_1_0',
                  start_ms: 0,
                  end_ms: 1000,
                  text: 'brand new cue',
                  confidence: 0.96,
                },
              ],
            }
          : t
      ),
    }));
    resolveReRec([
      {
        cue_id: 1,
        ok: true,
        text: 'STALE_VLM_PATCH',
        category: 'dialogue',
        confidence: 0.99,
      },
    ]);
    const res2 = await reRecPromise2;
    expect(res2.ok).toBe(false);
    expect(store.getState().tasks[0].cues[0].text).toBe('brand new cue');
    expect(store.getState().tasks[0].isReRecognizing).toBe(false);
  });

  it('parses multi-line IPC error strings and converges multi-line [Aborted] to cancelled [HIGH-4]', async () => {
    expect(parseIpcError(new Error('[Aborted] cancelled\nstderr line 1\nline 2'))).toEqual({
      code: 'Aborted',
      message: 'cancelled\nstderr line 1\nline 2',
    });
    expect(parseIpcError('[DecoderFailed] bad header\nffmpeg exit 1')).toEqual({
      code: 'DecoderFailed',
      message: 'bad header\nffmpeg exit 1',
    });
    expect(parseIpcError('plain error\nline 2')).toEqual({
      code: 'IpcError',
      message: 'plain error\nline 2',
    });
    expect(parseIpcError({ detail: 'obj' })).toEqual({
      code: 'IpcError',
      message: '{"detail":"obj"}',
    });

    const bridge = createMockBridge({
      startExtraction: vi.fn(async () => {
        throw new Error('[Aborted] task aborted\nworker stderr tail');
      }),
    });
    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv']);
    await store.getState().startTaskExtraction();
    expect(store.getState().tasks[0].status).toBe('cancelled');
  });

  it('invokes all 19 Tauri IPC commands with exact snake_case keys and Channel events [HIGH-4]', async () => {
    tauriInvokeMock.mockClear();
    createdChannels.length = 0;
    expect(isTauriRuntime()).toBe(true);
    tauriIsTauriMock.mockImplementationOnce(() => {
      throw new Error('no window');
    });
    expect(isTauriRuntime()).toBe(false);

    const tauriBridge = createTauriBridge();

    await tauriBridge.getCapabilities();
    await tauriBridge.probeVideo('/v/ep01.mkv');
    await tauriBridge.extractPreviewFrame('/v/ep01.mkv', 1200, { x: 0, y: 800, width: 1920, height: 200 }, 960);
    await tauriBridge.extractPreviewFrame('/v/ep01.mkv', 500);
    await tauriBridge.detectAutoRegion('/v/ep01.mkv', 'paddle', 'cjk', 0.45);
    const onEv = vi.fn();
    await tauriBridge.startExtraction(
      'task_1:1',
      {
        video_path: '/v/ep01.mkv',
        mode: 'local_ocr',
        engine: 'paddle',
        provider_id: null,
        region_box: null,
        config: {
          sample_fps: 5,
          region_bottom_ratio: 0.3,
          subtitle_script: 'auto',
        },
        script: 'auto',
      },
      onEv
    );
    expect(createdChannels).toHaveLength(1);
    const sampleEv: TaskEvent = { type: 'phase', task_id: 'task_1:1', sub_phase: 'timing' };
    createdChannels[0].onmessage?.(sampleEv);
    expect(onEv).toHaveBeenCalledWith(sampleEv);

    await tauriBridge.cancelExtraction('task_1:1');
    await tauriBridge.reRecognizeEntries(
      '/v/ep01.mkv',
      undefined,
      [{ cue_id: 1, start_ms: 0, end_ms: 1000 }],
      undefined,
      'auto'
    );
    await tauriBridge.exportSrt('/tmp/out.srt', [], false);
    await tauriBridge.batchExportSrt([], 'rename');
    await tauriBridge.pickVideoFiles();
    await tauriBridge.pickSaveSrtPath('ep01.srt');
    await tauriBridge.listProviders();
    await tauriBridge.saveProvider({
      id: null,
      name: 'P1',
      provider_type: 'minimax',
      base_url: 'https://api.minimax.cn/v1',
      model: 'MiniMax-M3.1-Flash',
      thinking_mode: 'auto',
      concurrency: 4,
      is_default: true,
      api_key_update: { mode: 'keep' },
    });
    await tauriBridge.testProviderConnection({
      id: null,
      name: '',
      provider_type: 'openai',
      base_url: 'https://api.example.com/v1',
      model: 'gpt-4o-mini',
      thinking_mode: 'auto',
      concurrency: 4,
      is_default: false,
      api_key_update: { mode: 'replace', value: 'sk-probe' },
    });
    await tauriBridge.deleteProvider('p1');
    await tauriBridge.importProvidersFromEnv();
    await tauriBridge.loadSessionDraft('/v/ep01.mkv');
    await tauriBridge.saveSessionDraft({
      video_path: '/v/ep01.mkv',
      engine: 'paddle',
      provider_id: null,
      script: 'auto',
      sample_fps: 5,
      roi_mode: 'band',
      region_box: null,
      status: 'ready',
      entries: [],
      updated_at_ms: 1234,
    });
    await tauriBridge.listRecentSessions();
    await tauriBridge.stopMediaStream();

    const invoked = tauriInvokeMock.mock.calls.map((c) => [c[0], c[1]]);
    expect(invoked).toEqual([
      ['get_capabilities', undefined],
      ['probe_video', { video_path: '/v/ep01.mkv' }],
      [
        'extract_preview_frame',
        {
          video_path: '/v/ep01.mkv',
          timestamp_ms: 1200,
          crop: { x: 0, y: 800, width: 1920, height: 200 },
          max_width: 960,
        },
      ],
      [
        'extract_preview_frame',
        { video_path: '/v/ep01.mkv', timestamp_ms: 500, crop: null, max_width: null },
      ],
      [
        'detect_auto_region',
        { video_path: '/v/ep01.mkv', engine: 'paddle', script: 'cjk', bottom_ratio: 0.45 },
      ],
      [
        'start_extraction',
        expect.objectContaining({
          task_id: 'task_1:1',
          params: expect.objectContaining({ video_path: '/v/ep01.mkv' }),
          on_event: createdChannels[0],
        }),
      ],
      ['cancel_extraction', { task_id: 'task_1:1' }],
      [
        're_recognize_entries',
        {
          video_path: '/v/ep01.mkv',
          region_box: null,
          items: [{ cue_id: 1, start_ms: 0, end_ms: 1000 }],
          provider_id: null,
          script: 'auto',
        },
      ],
      [
        'export_srt',
        { out_path: '/tmp/out.srt', entries: [], apply_merge: false, merge_options: null },
      ],
      ['batch_export_srt', { items: [], conflict_policy: 'rename' }],
      ['pick_video_files', undefined],
      ['pick_save_srt_path', { default_name: 'ep01.srt' }],
      ['list_providers', undefined],
      ['save_provider', { profile: expect.objectContaining({ name: 'P1' }) }],
      [
        'test_provider_connection',
        {
          profile: expect.objectContaining({
            base_url: 'https://api.example.com/v1',
          }),
        },
      ],
      ['delete_provider', { id: 'p1' }],
      ['import_providers_from_env', { env_path: null }],
      ['load_session_draft', { video_path: '/v/ep01.mkv' }],
      ['save_session_draft', { draft: expect.objectContaining({ video_path: '/v/ep01.mkv' }) }],
      ['list_recent_sessions', undefined],
      ['stop_media_stream', undefined],
    ]);
  });

  it('blocks undo, redo, replaceCurrentMatch, and replaceAllMatches (current & queue) while isReRecognizing is active [HIGH-2]', async () => {
    let resolveReRec!: (res: ReRecognizeResult[]) => void;
    const bridge = createMockBridge({
      reRecognizeEntries: vi.fn(
        () =>
          new Promise<ReRecognizeResult[]>((resolve) => {
            resolveReRec = resolve;
          })
      ),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv', '/v/ep02.mkv']);
    const [t1, t2] = store.getState().tasks;
    await store.getState().startTaskExtraction(t1.id);
    await store.getState().startTaskExtraction(t2.id);

    // Push one editable mutation onto t1 history so canUndo is true
    store.getState().selectActiveTask(t1.id);
    const uid1 = store.getState().tasks[0].cues[0].local_uid;
    const uid2 = store.getState().tasks[0].cues[1].local_uid;
    store.getState().commitCueText(uid1, '已改第一句');
    store.getState().undo(t1.id);
    expect(store.getState().tasks[0].cues[0].text).toBe('第一句');
    // Now both canUndo (after another edit) and canRedo are testable; push edit then undo/redo
    store.getState().redo(t1.id);
    expect(store.getState().tasks[0].cues[0].text).toBe('已改第一句');

    // Start re-recognition on cue 2 of t1
    const reRecPromise = store.getState().reRecognizeCues([uid2]);
    expect(store.getState().tasks[0].isReRecognizing).toBe(true);

    // Attempt undo, redo, replaceCurrentMatch, replaceAllMatches(current), and replaceAllMatches(queue)
    store.getState().undo(t1.id);
    expect(store.getState().tasks[0].cues[0].text).toBe('已改第一句');
    store.getState().redo(t1.id);

    store.getState().openFind(true);
    store.getState().setFindState({ query: '已改', replacement: 'X', scope: 'current' });
    store.getState().replaceCurrentMatch();
    expect(store.getState().tasks[0].cues[0].text).toBe('已改第一句');
    expect(store.getState().replaceAllMatches()).toBe(0);

    // Queue-wide replace must skip t1 (isReRecognizing) while still updating t2
    store.getState().setFindState({ query: '模糊识别', replacement: '队列替换', scope: 'queue' });
    const replacedCount = store.getState().replaceAllMatches();
    expect(replacedCount).toBe(1);
    expect(store.getState().tasks[0].cues[1].text).toBe('模糊识别');
    expect(store.getState().tasks[1].cues[1].text).toBe('队列替换');

    // Resolve VLM patch -> must still apply cleanly because t1.cues reference was protected!
    resolveReRec([
      {
        cue_id: 2,
        ok: true,
        text: 'VLM 修正成功',
        category: 'dialogue',
        confidence: 0.99,
      },
    ]);
    const reRecRes = await reRecPromise;
    expect(reRecRes).toEqual({ ok: true, succeeded: 1, failed: 0 });
    expect(store.getState().tasks[0].cues[1].text).toBe('VLM 修正成功');
    expect(store.getState().tasks[0].isReRecognizing).toBe(false);
  });

  it('prevents delayed detectAutoRegionForTask from overwriting roiMode/regionBox/previewTimestampMs during extracting [MEDIUM-1]', async () => {
    let resolveAuto!: (res: AutoRegionDto) => void;
    let resolveExtract!: (res: ExtractionResultDto) => void;

    const bridge = createMockBridge({
      detectAutoRegion: vi.fn(
        () =>
          new Promise<AutoRegionDto>((resolve) => {
            resolveAuto = resolve;
          })
      ),
      startExtraction: vi.fn(
        () =>
          new Promise<ExtractionResultDto>((resolve) => {
            resolveExtract = resolve;
          })
      ),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv']);

    const bandBox = { x: 0, y: 756, width: 1920, height: 324 };
    store.getState().setTaskRoiMode('band');
    store.getState().setTaskRegionBox(bandBox);

    const autoPromise = store.getState().detectAutoRegionForTask();
    expect(store.getState().tasks[0].isAutoDetecting).toBe(true);

    // Start extraction before auto-detect completes
    const extractPromise = store.getState().startTaskExtraction();
    expect(store.getState().tasks[0].status).toBe('extracting');
    expect(store.getState().tasks[0].activeRunConfig?.roiMode).toBe('band');

    // Calling detectAutoRegionForTask while extracting is also a no-op
    await store.getState().detectAutoRegionForTask();
    expect(bridge.detectAutoRegion).toHaveBeenCalledTimes(1);

    // Delayed auto-detect resolves mid-extraction: must NOT clobber extracting task
    resolveAuto({
      detected: true,
      region_box: { x: 10, y: 500, width: 500, height: 100 },
      profile: null,
      sample_timestamp_ms: 42_000,
      preview_text: '迟到自动探测',
      confidence: 0.95,
      error_message: '',
    });
    await autoPromise;

    const duringExtract = store.getState().tasks[0];
    expect(duringExtract.roiMode).toBe('band');
    expect(duringExtract.regionBox).toEqual(bandBox);
    expect(duringExtract.previewTimestampMs).toBe(0);
    expect(duringExtract.isAutoDetecting).toBe(false);

    resolveExtract({ entries: [], metrics: DEFAULT_METRICS });
    await extractPromise;
  });

  it('blocks navigateFindMatch cross-task switch when isDirtyInlineEdit is true and seeks streamingPreviewCues during extracting [MEDIUM-2]', async () => {
    let emitStream!: (ev: TaskEvent) => void;
    let resolveExtract!: (res: ExtractionResultDto) => void;
    let extractCall = 0;

    const bridge = createMockBridge({
      startExtraction: vi.fn((taskId, _params, onEvent) => {
        extractCall += 1;
        if (extractCall === 1) {
          const res = {
            entries: [{ id: 1, start_ms: 0, end_ms: 1000, text: '共享词一', confidence: 0.9 }],
            metrics: DEFAULT_METRICS,
          };
          onEvent({ type: 'completed', task_id: taskId, result: res });
          return Promise.resolve(res);
        }
        if (extractCall === 2) {
          const res = {
            entries: [{ id: 1, start_ms: 0, end_ms: 1000, text: '共享词二', confidence: 0.9 }],
            metrics: DEFAULT_METRICS,
          };
          onEvent({ type: 'completed', task_id: taskId, result: res });
          return Promise.resolve(res);
        }
        return new Promise<ExtractionResultDto>((resolve) => {
          emitStream = (ev) => onEvent({ ...ev, task_id: taskId });
          resolveExtract = resolve;
        });
      }),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv', '/v/ep02.mkv']);
    const [t1, t2] = store.getState().tasks;
    await store.getState().startTaskExtraction(t1.id);
    await store.getState().startTaskExtraction(t2.id);

    store.getState().selectActiveTask(t1.id);
    store.getState().openFind(false);
    store.getState().setFindState({ query: '共享词', scope: 'queue' });
    const t1CueUid = store.getState().tasks[0].cues[0].local_uid;
    store.getState().setDirtyInlineEdit(true, true, t1CueUid);

    // Next match is in t2 -> blocked by isDirtyInlineEdit
    store.getState().navigateFindMatch('next');
    expect(store.getState().activeTaskId).toBe(t1.id);
    expect(store.getState().findReplace.activeMatchIndex).toBe(0);
    expect(store.getState().toasts.at(-1)?.title).toContain('未提交的行内编辑');

    // Clear dirty edit -> cross-task jump succeeds
    store.getState().setDirtyInlineEdit(false, false, t1CueUid);
    store.getState().navigateFindMatch('next');
    expect(store.getState().activeTaskId).toBe(t2.id);

    // During extracting, current-scope Find seeks streamingPreviewCues midpoint
    store.getState().setFindState({ query: '流式命中', scope: 'current' });
    const streamRun = store.getState().startTaskExtraction(t2.id);
    await Promise.resolve();
    await Promise.resolve();
    emitStream({
      type: 'subtitle_entry',
      task_id: 'task_2:2',
      entry: { id: 7, start_ms: 8000, end_ms: 10_000, text: '包含流式命中的字幕', confidence: 0.92 },
    });
    store.getState().navigateFindMatch('next');
    expect(store.getState().tasks[1].activeCueId).toBe('stream_7_0');
    expect(store.getState().tasks[1].previewTimestampMs).toBe(9000);

    resolveExtract({ entries: [], metrics: DEFAULT_METRICS });
    await streamRun;
  });

  it('handles removeTask during pending probeVideo and applyConfigToQueue pendingScaledRoi hydration [LOW-1]', async () => {
    let resolvePendingProbe2!: (info: VideoInfoDto) => void;
    let resolvePendingProbe3!: (info: VideoInfoDto) => void;

    const bridge = createMockBridge({
      probeVideo: vi.fn((path: string) => {
        if (path.includes('ep02')) {
          return new Promise<VideoInfoDto>((resolve) => {
            resolvePendingProbe2 = resolve;
          });
        }
        if (path.includes('ep03_720p')) {
          return new Promise<VideoInfoDto>((resolve) => {
            resolvePendingProbe3 = resolve;
          });
        }
        return Promise.resolve(makeVideoInfo(1920, 1080));
      }),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();

    const addPromise = store.getState().addVideoPaths([
      '/v/ep01.mkv',
      '/v/ep02.mkv',
      '/v/ep03_720p.mkv',
    ]);
    await Promise.resolve();

    const [t1, t2, t3] = store.getState().tasks;
    store.getState().setTaskRegionBox({ x: 0, y: 756, width: 1920, height: 324 }, t1.id);

    // Apply config BEFORE ep03 probe resolves -> stores pendingScaledRoi
    const summary = store.getState().applyConfigToQueue({ sourceTaskId: t1.id });
    expect(summary.updatedCount).toBe(2);
    expect(store.getState().tasks[2].pendingScaledRoi).not.toBeNull();

    // Remove ep02 while its probeVideo is still pending
    store.getState().removeTask(t2.id);
    expect(store.getState().tasks.map((t) => t.id)).toEqual([t1.id, t3.id]);

    // Resolve pending probes: removed ep02 is ignored, ep03 scales pendingScaledRoi to 1280x720
    resolvePendingProbe2(makeVideoInfo(1920, 1080));
    resolvePendingProbe3(makeVideoInfo(1280, 720));
    await addPromise;

    const hydratedT3 = store.getState().tasks.find((t) => t.id === t3.id)!;
    expect(hydratedT3.pendingScaledRoi).toBeNull();
    expect(hydratedT3.roiScaledFromQueue).toBe(true);
    expect(hydratedT3.regionBox).toEqual({ x: 0, y: 504, width: 1280, height: 216 });
  });

  it('calibrates low-confidence threshold for apple_vision and filters blank noise cues on extraction and draft restore', async () => {
    const bridge = createMockBridge({
      getCapabilities: vi.fn(async () => ({
        engines: ['paddle', 'apple_vision'] as OcrEngineId[],
        ffmpeg_available: true,
        hwaccel_modes: [],
      })),
      loadSessionDraft: vi.fn(async (videoPath: string) =>
        videoPath.includes('draft')
          ? {
              video_path: videoPath,
              engine: 'apple_vision' as const,
              provider_id: null,
              script: 'cjk' as const,
              sample_fps: 5,
              roi_mode: 'band' as const,
              region_box: { x: 0, y: 756, width: 1920, height: 324 },
              status: 'ready' as const,
              entries: [
                { id: 1, start_ms: 0, end_ms: 800, text: '   ', confidence: 0.0 },
                { id: 2, start_ms: 800, end_ms: 1600, text: '中文准确句', confidence: 0.5 },
                { id: 3, start_ms: 1600, end_ms: 2400, text: '低置信句', confidence: 0.3 },
              ],
              updated_at_ms: 1000,
            }
          : null
      ),
      startExtraction: vi.fn(async (taskId, _params, onEvent) => {
        onEvent({
          type: 'subtitle_entry',
          task_id: taskId,
          entry: { id: 1, start_ms: 0, end_ms: 500, text: '', confidence: 0.0 },
        });
        onEvent({
          type: 'subtitle_entry',
          task_id: taskId,
          entry: { id: 2, start_ms: 500, end_ms: 1500, text: 'Apple Vision 中文', confidence: 0.5 },
        });
        const res = {
          entries: [
            { id: 1, start_ms: 0, end_ms: 500, text: '   ', confidence: 0.0 },
            { id: 2, start_ms: 500, end_ms: 1500, text: 'Apple Vision 中文', confidence: 0.5 },
            { id: 3, start_ms: 1500, end_ms: 2500, text: '模糊噪点句', confidence: 0.3 },
          ],
          metrics: DEFAULT_METRICS,
        };
        onEvent({ type: 'completed', task_id: taskId, result: res });
        return res;
      }),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/live.mkv', '/v/draft.mkv']);

    // Run live apple_vision extraction
    const [liveTask, draftTask] = store.getState().tasks;
    store.getState().setTaskEngine('apple_vision', liveTask.id);
    await store.getState().startTaskExtraction(liveTask.id);

    const completedLive = store.getState().tasks[0];
    expect(completedLive.cues.map((c) => c.id)).toEqual([2, 3]);
    expect(selectLowConfCount(store.getState())).toBe(1);
    expect(selectLowConfCues(store.getState()).map((c) => c.id)).toEqual([3]);

    // Switching engine button on an already-ready task does NOT alter activeRunConfig low-conf threshold
    store.getState().setTaskEngine('paddle', liveTask.id);
    expect(selectLowConfCount(store.getState())).toBe(1);

    // Restored ready apple_vision draft preserves activeRunConfig threshold & filters blank lines
    store.getState().selectActiveTask(draftTask.id);
    expect(store.getState().tasks[1].cues.map((c) => c.id)).toEqual([2, 3]);
    expect(selectLowConfCount(store.getState())).toBe(1);
    expect(selectLowConfCues(store.getState()).map((c) => c.id)).toEqual([3]);
  });

  it('defaults newly added videos to auto ROI, pre-detects with profile passthrough before extraction, reuses valid cache, and supports queue/fallback/cancel', async () => {
    const detectedBoxEp1 = { x: 0, y: 820, width: 1920, height: 140 };
    const detectedProfileEp1 = {
      script: 'cjk' as const,
      center_x: 960,
      center_y: 890,
      height: 54,
      y_min: 855,
      y_max: 925,
    };
    const detectedBoxEp2 = { x: 0, y: 540, width: 1280, height: 100 };

    let detectCalls = 0;
    const bridge = createMockBridge({
      detectAutoRegion: vi.fn(async (videoPath: string) => {
        detectCalls += 1;
        if (videoPath.includes('ep02_720p')) {
          return {
            detected: true,
            region_box: detectedBoxEp2,
            profile: null,
            sample_timestamp_ms: 4200,
            preview_text: '第二集样本',
            confidence: 0.9,
            error_message: '',
          };
        }
        return {
          detected: true,
          region_box: detectedBoxEp1,
          profile: detectedProfileEp1,
          sample_timestamp_ms: 8100,
          preview_text: '第一集样本',
          confidence: 0.95,
          error_message: '',
        };
      }),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv', '/v/ep02_720p.mkv']);

    const [t1, t2] = store.getState().tasks;
    // 1. Default ROI mode is 'auto'
    expect(t1.roiMode).toBe('auto');
    expect(t2.roiMode).toBe('auto');

    // 2. Starting extraction directly on un-probed auto task pre-triggers detectAutoRegion and passes region_box + predetected_profile
    await store.getState().startTaskExtraction(t1.id);
    expect(detectCalls).toBe(1);
    expect(bridge.startExtraction).toHaveBeenLastCalledWith(
      `${t1.id}:1`,
      expect.objectContaining({
        region_box: detectedBoxEp1,
        config: expect.objectContaining({
          region_bottom_ratio: 0.45,
          predetected_profile: detectedProfileEp1,
        }),
      }),
      expect.any(Function)
    );
    expect(store.getState().tasks[0].autoRegionInfo?.detected).toBe(true);
    expect(store.getState().tasks[0].autoRegionInfo?.profile).toEqual(detectedProfileEp1);

    // 3. Re-running with unchanged script reuses valid cache (no duplicate detectAutoRegion); changing script invalidates cache and triggers fresh detect on next run
    await store.getState().startTaskExtraction(t1.id);
    expect(detectCalls).toBe(1);
    store.getState().setTaskScript('cjk', t1.id);
    expect(store.getState().tasks[0].autoRegionInfo).toBeNull();
    await store.getState().startTaskExtraction(t1.id);
    expect(detectCalls).toBe(2);

    // 4. Queue mode runs per-episode pre-detection independently (applyConfigToQueue clears target autoRegionInfo even on ready->idle reset)
    store.getState().applyConfigToQueue({ sourceTaskId: t1.id, includeCompleted: true });
    expect(store.getState().tasks[1].script).toBe('cjk');
    expect(store.getState().tasks[1].autoRegionInfo).toBeNull();
    await store.getState().startQueueExtraction();
    expect(detectCalls).toBe(3); // +1 for ep02_720p in queue
    const ep2StartCall = vi.mocked(bridge.startExtraction).mock.calls.at(-1);
    expect(ep2StartCall?.[1].region_box).toEqual(detectedBoxEp2);

    // 5. Fallback when detectAutoRegion returns detected=false or throws error (does not block queue)
    const fallbackBridge = createMockBridge({
      detectAutoRegion: vi.fn(async (videoPath: string) => {
        if (videoPath.includes('miss')) {
          return {
            detected: false,
            region_box: { x: 0, y: 594, width: 1920, height: 486 },
            profile: null,
            sample_timestamp_ms: 0,
            preview_text: '',
            confidence: 0,
            error_message: '无字幕行',
          };
        }
        throw new Error('探测进程超时');
      }),
    });
    const fallbackStore = createWorkbenchStore(fallbackBridge);
    await fallbackStore.getState().initialize();
    await fallbackStore.getState().addVideoPaths(['/v/miss.mkv', '/v/err.mkv']);
    await fallbackStore.getState().startQueueExtraction();
    expect(fallbackStore.getState().tasks[0].status).toBe('ready');
    expect(fallbackStore.getState().tasks[1].status).toBe('ready');
    expect(vi.mocked(fallbackBridge.startExtraction)).toHaveBeenCalledTimes(2);
    // Expected default 0.45 band height on 1920x1080 = 486, y = 594
    expect(vi.mocked(fallbackBridge.startExtraction).mock.calls[0][1].region_box).toEqual({
      x: 0,
      y: 594,
      width: 1920,
      height: 486,
    });

    // 6. Cancelling during in-flight pre-detect immediately cancels and never invokes startExtraction
    let resolveHangingDetect!: (val: AutoRegionDto) => void;
    const cancelBridge = createMockBridge({
      detectAutoRegion: vi.fn(
        () =>
          new Promise<AutoRegionDto>((resolve) => {
            resolveHangingDetect = resolve;
          })
      ),
    });
    const cancelStore = createWorkbenchStore(cancelBridge);
    await cancelStore.getState().initialize();
    await cancelStore.getState().addVideoPaths(['/v/hang.mkv']);
    const pendingRun = cancelStore.getState().startTaskExtraction();
    expect(cancelStore.getState().tasks[0].status).toBe('extracting');
    expect(cancelStore.getState().tasks[0].isAutoDetecting).toBe(true);

    await cancelStore.getState().cancelTaskExtraction();
    expect(cancelStore.getState().tasks[0].status).toBe('cancelled');
    expect(cancelStore.getState().tasks[0].isAutoDetecting).toBe(false);
    expect(cancelBridge.cancelExtraction).not.toHaveBeenCalled();

    resolveHangingDetect({
      detected: true,
      region_box: detectedBoxEp1,
      profile: detectedProfileEp1,
      sample_timestamp_ms: 1000,
      preview_text: 'late',
      confidence: 0.9,
      error_message: '',
    });
    await pendingRun;
    expect(cancelBridge.startExtraction).not.toHaveBeenCalled();
    expect(cancelStore.getState().tasks[0].status).toBe('cancelled');
  });

  it('syncs existing tasks and restored drafts when a different default VLM provider is saved', async () => {
    const providerList: ProviderProfileMaskedDto[] = [
      {
        id: 'env-minimax',
        name: 'MiniMax (env)',
        provider_type: 'minimax',
        base_url: 'https://api.minimax.cn/v1',
        model: 'MiniMax-M3.1-Flash',
        thinking_mode: 'auto',
        concurrency: 6,
        has_api_key: true,
        masked_api_key: 'sk-****1111',
        is_default: true,
      },
      {
        id: 'env-nvidia',
        name: 'NVIDIA NIM (env)',
        provider_type: 'nvidia',
        base_url: 'https://integrate.api.nvidia.com/v1',
        model: 'nvidia/nemotron-3.5-super-vl-120b-a12b',
        thinking_mode: 'auto',
        concurrency: 4,
        has_api_key: true,
        masked_api_key: 'nva****q1xc',
        is_default: false,
      },
    ];

    let statefulProviders = providerList.map((p) => ({ ...p }));
    const bridge = createMockBridge({
      listProviders: vi.fn(async () => statefulProviders.map((p) => ({ ...p }))),
      saveProvider: vi.fn(async (profile) => {
        const savedId = profile.id ?? 'prov_new';
        statefulProviders = statefulProviders.map((p) => ({
          ...p,
          is_default: profile.is_default ? p.id === savedId : p.is_default,
          ...(p.id === savedId && profile.custom_headers !== undefined
            ? { custom_headers: profile.custom_headers }
            : {}),
        }));
        const matched = statefulProviders.find((p) => p.id === savedId)!;
        return { ...matched };
      }),
      loadSessionDraft: vi.fn(async (videoPath: string) =>
        videoPath.includes('with_old_draft')
          ? {
              video_path: videoPath,
              engine: 'vlm' as const,
              provider_id: 'env-minimax',
              script: 'auto' as const,
              sample_fps: 5,
              roi_mode: 'auto' as const,
              region_box: { x: 0, y: 756, width: 1920, height: 324 },
              status: 'idle' as const,
              entries: [],
              updated_at_ms: 1000,
            }
          : null
      ),
    });

    const store = createWorkbenchStore(bridge);
    await store.getState().initialize();
    await store.getState().addVideoPaths(['/v/ep01.mkv']);

    expect(selectEffectiveProvider(store.getState())?.id).toBe('env-minimax');

    // Switch default VLM provider to env-nvidia
    await store.getState().saveProvider({
      id: 'env-nvidia',
      name: 'NVIDIA NIM (env)',
      provider_type: 'nvidia',
      base_url: 'https://integrate.api.nvidia.com/v1',
      model: 'nvidia/nemotron-3.5-super-vl-120b-a12b',
      thinking_mode: 'auto',
      concurrency: 4,
      is_default: true,
      api_key_update: { mode: 'keep' },
    });

    // Loaded task and selectEffectiveProvider immediately follow the new default
    expect(store.getState().tasks[0].providerId).toBe('env-nvidia');
    expect(selectEffectiveProvider(store.getState())?.id).toBe('env-nvidia');
    expect(selectEffectiveProvider(store.getState())?.name).toBe('NVIDIA NIM (env)');

    // Hydrating a legacy draft with stale provider_id ('env-minimax') aligns to active global default ('env-nvidia')
    await store.getState().addVideoPaths(['/v/with_old_draft.mkv']);
    expect(store.getState().tasks[1].providerId).toBe('env-nvidia');
  });
});
