import { describe, it, expect, beforeEach } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import { useWorkbenchStore } from './workbench';
import { useSystemStore } from './system';
import { SubLiftApiClient } from '../api/client';
import type { SseJobCallbacks } from '../types/api';

describe('useWorkbenchStore - Milestone 2 (Feature 12512)', () => {
  beforeEach(() => {
    setActivePinia(createPinia());
    const systemStore = useSystemStore();
    systemStore.systemInfo = {
      version: 'v0.1.0',
      runtime: 'cpp',
      capabilities: [],
      engines: [{ name: 'vision', available: true, detail: 'Apple Vision' }],
      ffmpeg: { available: true, path: '/usr/bin/ffmpeg' },
    };
  });

  it('initializes in Empty state with progressPct at 0', () => {
    const store = useWorkbenchStore();
    expect(store.state).toBe('Empty');
    expect(store.progress.pct).toBe(0);
    expect(store.progressPct).toBe(0);
    expect(store.entries.length).toBe(0);
  });

  it('converts server 0.0–1.0 float progress scale to 0–100% integer scale', () => {
    const store = useWorkbenchStore();
    expect(store.progressPct).toBe(0);

    store.progress = { stage: 'extracting', pct: 0.125, eta_ms: 5000 };
    expect(store.progressPct).toBe(13);

    store.progress = { stage: 'extracting', pct: 0.5, eta_ms: 2500 };
    expect(store.progressPct).toBe(50);

    store.progress = { stage: 'extracting', pct: 0.999, eta_ms: 10 };
    expect(store.progressPct).toBe(100);

    store.progress = { stage: 'done', pct: 1.0 };
    expect(store.progressPct).toBe(100);
  });

  it('deduplicates incoming subtitle entries on replayed SSE events', async () => {
    const store = useWorkbenchStore();
    store.loadVideo('/path/to/sample.mp4');

    const sse: { cb: SseJobCallbacks | null } = { cb: null };
    SubLiftApiClient.createJob = async () => ({ job_id: 'wb-job-1', status: 'running' });
    SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
      sse.cb = callbacks;
      return () => {
        sse.cb = null;
      };
    };

    const extractPromise = store.startExtraction();
    await new Promise((r) => setTimeout(r, 10));

    expect(store.state).toBe('Processing');

    // First push entry
    sse.cb?.onPushEntry?.({
      job_id: 'wb-job-1',
      entry: { index: 1, start_ms: 500, end_ms: 1500, text: 'Hello subtitle', confidence: 0.96 },
    });
    expect(store.entries.length).toBe(1);

    // Replayed event with duplicate index
    sse.cb?.onPushEntry?.({
      job_id: 'wb-job-1',
      entry: { index: 1, start_ms: 500, end_ms: 1500, text: 'Hello subtitle', confidence: 0.96 },
    });
    expect(store.entries.length).toBe(1);

    // Next entry
    sse.cb?.onPushEntry?.({
      job_id: 'wb-job-1',
      entry: { index: 2, start_ms: 1600, end_ms: 3000, text: 'Next line', confidence: 0.92 },
    });
    expect(store.entries.length).toBe(2);

    sse.cb?.onDone?.({
      job_id: 'wb-job-1',
      total_entries: 2,
      elapsed_ms: 800,
    });

    await extractPromise;
    expect(store.state).toBe('Review');
    expect(store.entries.length).toBe(2);
  });

  it('handles cancellation and reset correctly', async () => {
    const store = useWorkbenchStore();
    store.loadVideo('/path/to/sample.mp4');

    let cancelCalled = false;
    SubLiftApiClient.createJob = async () => ({ job_id: 'wb-job-cancel', status: 'running' });
    SubLiftApiClient.cancelJob = async () => {
      cancelCalled = true;
    };
    SubLiftApiClient.subscribeJobEvents = () => () => {};

    await store.startExtraction();
    expect(store.state).toBe('Processing');

    await store.cancelExtraction();
    expect(cancelCalled).toBe(true);
    expect(store.state).toBe('Cancelled');

    store.reset();
    expect(store.state).toBe('Empty');
    expect(store.videoPath).toBe('');
    expect(store.entries.length).toBe(0);
  });

  describe('Feature 12513: Workbench Extraction Config & Quality Parity', () => {
    it('creates an immutable configuration snapshot upon startExtraction', async () => {
      const store = useWorkbenchStore();
      store.loadVideo('/path/to/video.mp4');
      store.selectedEngine = 'vision';
      store.quality = 'fine'; // 12 FPS
      store.confidenceThreshold = 0.3;
      store.updateRegionBox({ x: 0.1, y: 0.75, width: 0.8, height: 0.2 });

      let capturedJobConfig: any = null;
      SubLiftApiClient.createJob = async (cfg) => {
        capturedJobConfig = cfg;
        return { job_id: 'wb-snapshot-job', status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = () => () => {};

      await store.startExtraction();

      expect(store.activeConfigSnapshot).not.toBeNull();
      expect(Object.isFrozen(store.activeConfigSnapshot)).toBe(true);
      expect(store.activeConfigSnapshot?.quality).toBe('fine');
      expect(store.activeConfigSnapshot?.confidence_threshold).toBe(0.3);
      expect(store.activeConfigSnapshot?.region_box).toEqual({ x: 0.1, y: 0.75, width: 0.8, height: 0.2 });

      expect(capturedJobConfig.fps).toBe(12.0);
      expect(capturedJobConfig.confidence_threshold).toBe(0.3);
      expect(capturedJobConfig.region_box).toEqual({ x: 0.1, y: 0.75, width: 0.8, height: 0.2 });
    });

    it('fails closed when selected OCR engine is unavailable (no silent fallback)', async () => {
      const systemStore = useSystemStore();
      systemStore.systemInfo = {
        version: 'v0.1.0',
        runtime: 'cpp',
        capabilities: [],
        engines: [
          { name: 'vision', available: true, detail: 'Apple Vision' },
          { name: 'paddle', available: false, detail: 'PaddleOCR models missing' },
        ],
        ffmpeg: { available: true },
      };

      const store = useWorkbenchStore();
      store.loadVideo('/path/to/video.mp4');
      store.selectedEngine = 'paddle'; // Not available!

      let createJobCalled = false;
      SubLiftApiClient.createJob = async () => {
        createJobCalled = true;
        return { job_id: 'wb-should-not-run', status: 'running' };
      };

      await expect(store.startExtraction()).rejects.toThrow('OCR 引擎不可用: paddle (严格禁止静默回退)');
      expect(createJobCalled).toBe(false);
      expect(store.state).toBe('Failed');
      expect(store.errorMessage).toContain('严格禁止静默回退');
    });

    it('guarantees output and configuration parity between single workbench and batch mode for identical settings', async () => {
      const { useBatchStore } = await import('./batch');
      const workbenchStore = useWorkbenchStore();
      const batchStore = useBatchStore();

      const testVideo = '/workspace/parity_sample.mp4';
      const sharedBox = { x: 0.05, y: 0.72, width: 0.9, height: 0.22 };

      // Configure workbench
      workbenchStore.loadVideo(testVideo);
      workbenchStore.selectedEngine = 'vision';
      workbenchStore.quality = 'balanced'; // 8.0 FPS
      workbenchStore.confidenceThreshold = 0.15;
      workbenchStore.updateRegionBox(sharedBox);

      // Configure batch task
      batchStore.addBatchItems([
        {
          videoPath: testVideo,
          config: {
            engine: 'vision',
            quality: 'balanced',
            confidence_threshold: 0.15,
            roi_policy: 'fixed',
            region_box: sharedBox,
          },
        },
      ]);

      const capturedConfigs: any[] = [];
      const testEntries = [
        { index: 1, start_ms: 1000, end_ms: 3000, text: 'Parity Line 1', confidence: 0.98 },
        { index: 2, start_ms: 3500, end_ms: 5500, text: 'Parity Line 2', confidence: 0.94 },
      ];

      SubLiftApiClient.createJob = async (cfg) => {
        capturedConfigs.push(cfg);
        return { job_id: `job-parity-${capturedConfigs.length}`, status: 'running' };
      };

      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        testEntries.forEach((entry) => callbacks.onPushEntry?.({ entry }));
        callbacks.onDone?.({ total_entries: 2, elapsed_ms: 600 });
        return () => {};
      };

      // 1. Run single extraction
      await workbenchStore.startExtraction();
      expect(workbenchStore.entries.length).toBe(2);
      expect(workbenchStore.entries).toEqual(testEntries);

      // 2. Run batch extraction
      await batchStore.startSingle(batchStore.tasks[0].id);
      expect(batchStore.tasks[0].entries.length).toBe(2);
      expect(batchStore.tasks[0].entries).toEqual(testEntries);

      // 3. Compare createJob parameters
      expect(capturedConfigs.length).toBe(2);
      const [wbConfig, batchConfig] = capturedConfigs;

      expect(wbConfig.video_path).toBe(batchConfig.video_path);
      expect(wbConfig.engine).toBe(batchConfig.engine);
      expect(wbConfig.fps).toBe(batchConfig.fps);
      expect(wbConfig.fps).toBe(8.0);
      expect(wbConfig.confidence_threshold).toBe(batchConfig.confidence_threshold);
      expect(wbConfig.confidence_threshold).toBe(0.15);
      expect(wbConfig.region_box).toEqual(batchConfig.region_box);
      expect(wbConfig.region_box).toEqual(sharedBox);
    });
  });
});

