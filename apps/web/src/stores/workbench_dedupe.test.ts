import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import { useWorkbenchStore } from './workbench';
import { useSystemStore } from './system';
import { SubLiftApiClient } from '../api/client';
import type { SseJobCallbacks } from '../types/api';

describe('Workbench final entries replace live stream (dedupe path)', () => {
  beforeEach(() => {
    setActivePinia(createPinia());
    vi.restoreAllMocks();
    useSystemStore().systemInfo = {
      version: 'test',
      runtime: 'cpp',
      capabilities: [],
      engines: [{ name: 'mock', available: true, detail: '' }],
      ffmpeg: { available: true, path: '/usr/bin/ffmpeg' },
    };
  });

  afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
  });

  it('replaces push_entry list with GET /export after done', async () => {
    const system = useSystemStore();
    system.systemInfo = {
      version: 'test',
      runtime: 'cpp',
      capabilities: [],
      engines: [{ name: 'mock', available: true, detail: '' }],
      ffmpeg: { available: true, path: '/usr/bin/ffmpeg' },
    };
    const store = useWorkbenchStore();
    store.selectedEngine = 'mock';
    const sse: { cb: SseJobCallbacks | null } = { cb: null };
    const drafts: Record<string, string> = {};
    vi.stubGlobal('localStorage', {
      getItem: (key: string) => drafts[key] ?? null,
      setItem: (key: string, value: string) => { drafts[key] = value; },
      removeItem: (key: string) => { delete drafts[key]; },
    });

    vi.spyOn(SubLiftApiClient, 'detectSubtitleRegion').mockResolvedValue({
      detected: false,
      sample_time_s: 0,
      suggested_box: { x: 0, y: 0.8, width: 1, height: 0.2 },
      preview_text: '',
      confidence: 0,
      total_candidates: 0,
    });
    vi.spyOn(SubLiftApiClient, 'createJob').mockResolvedValue({ job_id: 'job-1', status: 'running' });
    vi.spyOn(SubLiftApiClient, 'subscribeJobEvents').mockImplementation((_id, callbacks) => {
      sse.cb = callbacks;
      return () => {
        sse.cb = null;
      };
    });
    vi.spyOn(SubLiftApiClient, 'exportSrt').mockResolvedValue(
      '1\n00:00:00,000 --> 00:00:02,000\n合并后的一句\n'
    );

    store.loadVideo('/tmp/clip.mp4');
    await store.startExtraction();
    expect(sse.cb).not.toBeNull();

    sse.cb?.onPushEntry?.({
      entry: { index: 1, start_ms: 0, end_ms: 600, text: '更', confidence: 0.9 },
    });
    sse.cb?.onPushEntry?.({
      entry: { index: 2, start_ms: 600, end_ms: 1400, text: '更多资源', confidence: 0.9 },
    });
    expect(store.entries.length).toBe(2);

    sse.cb?.onDone?.({
      total_entries: 1,
      elapsed_ms: 100,
    });

    await vi.waitFor(() => expect(store.state).toBe('Review'));
    expect(store.entries.length).toBe(1);
    expect(store.entries[0]?.text).toBe('合并后的一句');
    expect(JSON.parse(drafts['sublift_draft:/tmp/clip.mp4']).entries).toEqual(store.entries);
  });

  it('ignores a final export that arrives after cancellation', async () => {
    const store = useWorkbenchStore();
    store.selectedEngine = 'mock';
    const sse: { cb: SseJobCallbacks | null } = { cb: null };
    let resolveExport!: (srt: string) => void;
    const exportResult = new Promise<string>((resolve) => { resolveExport = resolve; });
    vi.spyOn(SubLiftApiClient, 'createJob').mockResolvedValue({ job_id: 'job-cancel', status: 'running' });
    vi.spyOn(SubLiftApiClient, 'subscribeJobEvents').mockImplementation((_id, callbacks) => {
      sse.cb = callbacks;
      return () => {};
    });
    vi.spyOn(SubLiftApiClient, 'exportSrt').mockReturnValue(exportResult);
    vi.spyOn(SubLiftApiClient, 'cancelJob').mockResolvedValue();

    store.loadVideo('/tmp/cancel.mp4');
    await store.startExtraction();
    sse.cb?.onPushEntry?.({
      entry: { index: 1, start_ms: 0, end_ms: 600, text: '原字幕', confidence: 0.9 },
    });
    sse.cb?.onDone?.({ total_entries: 1, elapsed_ms: 100 });
    await store.cancelExtraction();
    resolveExport('1\n00:00:00,000 --> 00:00:02,000\n迟到字幕\n');
    await exportResult;
    await Promise.resolve();

    expect(store.state).toBe('Cancelled');
    expect(store.entries[0]?.text).toBe('原字幕');
  });
});
