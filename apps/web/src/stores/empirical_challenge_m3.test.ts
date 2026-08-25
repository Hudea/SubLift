import { describe, it, expect, beforeEach } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import { useWorkbenchStore } from './workbench';
import { useBatchStore } from './batch';
import { useSystemStore } from './system';
import { SubLiftApiClient } from '../api/client';
import type { SseJobCallbacks, SubtitleEntry, JobConfig, NormalizedRegionBox } from '../types/api';
import { DEFAULT_BOTTOM_ROI } from '../types/config';

describe('Empirical Challenge - Phase 12 Milestone 3 (Feature 12513)', () => {
  beforeEach(() => {
    setActivePinia(createPinia());
    const systemStore = useSystemStore();
    systemStore.systemInfo = {
      version: 'v0.1.0-challenge-m3',
      runtime: 'cpp',
      capabilities: ['mock', 'vision', 'paddle'],
      engines: [
        { name: 'vision', available: true, detail: 'Apple Vision' },
        { name: 'paddle', available: true, detail: 'PaddleOCR' },
        { name: 'mock', available: true, detail: 'Mock Engine' },
      ],
      ffmpeg: { available: true, path: '/opt/homebrew/bin/ffmpeg' },
    };
  });

  describe('1. ROI Policy Execution Under Edge Cases', () => {
    it('Policy "auto": calls /api/video/detect-region per video and uses suggested_box on success', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/ws/video_auto_success.mp4',
          config: { roi_policy: 'auto', engine: 'vision', quality: 'fast' },
        },
      ]);

      let detectCallCount = 0;
      let detectedPath = '';
      let detectedEngine = '';
      SubLiftApiClient.detectSubtitleRegion = async (path, _ts, engine) => {
        detectCallCount++;
        detectedPath = path;
        detectedEngine = engine || '';
        return {
          detected: true,
          sample_time_s: 2.0,
          suggested_box: { x: 0.08, y: 0.75, width: 0.84, height: 0.2 },
          preview_text: 'Detected Subtitle Sample',
          confidence: 0.96,
          total_candidates: 4,
        };
      };

      let capturedJobConfig: JobConfig | null = null;
      const sse: { cb: SseJobCallbacks | null } = { cb: null };
      SubLiftApiClient.createJob = async (cfg) => {
        capturedJobConfig = cfg;
        return { job_id: 'job-auto-1', status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        sse.cb = callbacks;
        return () => {};
      };

      const task = batchStore.tasks[0];
      const runPromise = batchStore.startSingle(task.id);
      await new Promise((r) => setTimeout(r, 10));

      expect(detectCallCount).toBe(1);
      expect(detectedPath).toBe('/ws/video_auto_success.mp4');
      expect(detectedEngine).toBe('vision');
      expect(capturedJobConfig).not.toBeNull();
      expect(capturedJobConfig!.region_box).toEqual({ x: 0.08, y: 0.75, width: 0.84, height: 0.2 });

      sse.cb?.onDone?.({
        job_id: 'job-auto-1',
        total_entries: 1,
        elapsed_ms: 500,
      });
      await runPromise;

      expect(task.status).toBe('completed');
      expect(task.result?.roiSource).toContain('Detected Subtitle Sample');
      expect(task.result?.effectiveRegion).toEqual({ x: 0.08, y: 0.75, width: 0.84, height: 0.2 });
    });

    it('Policy "auto": handles detection failure (detected: false) by falling back to DEFAULT_BOTTOM_ROI with explicit diagnostic', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/ws/video_auto_miss.mp4',
          config: { roi_policy: 'auto', engine: 'vision', quality: 'fast' },
        },
      ]);

      SubLiftApiClient.detectSubtitleRegion = async () => ({
        detected: false,
        sample_time_s: 1.0,
        suggested_box: null as any,
        preview_text: '',
        confidence: 0,
        total_candidates: 0,
      });

      let capturedJobConfig: JobConfig | null = null;
      SubLiftApiClient.createJob = async (cfg) => {
        capturedJobConfig = cfg;
        return { job_id: 'job-auto-miss', status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        callbacks.onDone?.({ job_id: 'job-auto-miss', total_entries: 0, elapsed_ms: 300 });
        return () => {};
      };

      const task = batchStore.tasks[0];
      await batchStore.startSingle(task.id);

      expect(task.status).toBe('completed');
      expect(capturedJobConfig!.region_box).toEqual(DEFAULT_BOTTOM_ROI);
      expect(task.result?.roiSource).toContain('未命中字幕');
      expect(task.result?.roiSource).toContain('回退默认底边 30%');
      expect(task.result?.effectiveRegion).toEqual(DEFAULT_BOTTOM_ROI);
    });

    it('Policy "auto": handles detect-region API throwing an exception without crashing, falling back cleanly with diagnostics', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/ws/video_auto_exception.mp4',
          config: { roi_policy: 'auto', engine: 'vision', quality: 'fast' },
        },
      ]);

      SubLiftApiClient.detectSubtitleRegion = async () => {
        throw new Error('500 Internal Server Error: detector model timeout');
      };

      let capturedJobConfig: JobConfig | null = null;
      SubLiftApiClient.createJob = async (cfg) => {
        capturedJobConfig = cfg;
        return { job_id: 'job-auto-err', status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        callbacks.onDone?.({ job_id: 'job-auto-err', total_entries: 0, elapsed_ms: 400 });
        return () => {};
      };

      const task = batchStore.tasks[0];
      await batchStore.startSingle(task.id);

      // Successfully recovers and completes job with default ROI fallback
      expect(task.status).toBe('completed');
      expect(capturedJobConfig!.region_box).toEqual(DEFAULT_BOTTOM_ROI);
      expect(task.result?.roiSource).toContain('智能检测异常，已使用默认底边 30%');
      expect(task.result?.effectiveRegion).toEqual(DEFAULT_BOTTOM_ROI);
    });

    it('Policy "auto": in multi-task batch, triggers per-video independent detection without cross-contamination', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/ws/batch_vid_1.mp4',
          config: { roi_policy: 'auto', engine: 'vision' },
        },
        {
          videoPath: '/ws/batch_vid_2.mp4',
          config: { roi_policy: 'auto', engine: 'vision' },
        },
      ]);

      const detectCalls: string[] = [];
      SubLiftApiClient.detectSubtitleRegion = async (path) => {
        detectCalls.push(path);
        if (path === '/ws/batch_vid_1.mp4') {
          return {
            detected: true,
            suggested_box: { x: 0.1, y: 0.8, width: 0.8, height: 0.15 },
            preview_text: 'Box 1',
            confidence: 0.9,
            sample_time_s: 1.0,
            total_candidates: 1,
          };
        } else {
          return {
            detected: true,
            suggested_box: { x: 0.05, y: 0.7, width: 0.9, height: 0.25 },
            preview_text: 'Box 2',
            confidence: 0.95,
            sample_time_s: 1.0,
            total_candidates: 1,
          };
        }
      };

      const createdJobBoxes: NormalizedRegionBox[] = [];
      let jobIndex = 0;
      SubLiftApiClient.createJob = async (cfg) => {
        jobIndex++;
        createdJobBoxes.push(cfg.region_box!);
        return { job_id: `job-batch-auto-${jobIndex}`, status: 'running' };
      };

      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        callbacks.onDone?.({ job_id: _id, total_entries: 0, elapsed_ms: 200 });
        return () => {};
      };

      batchStore.startQueue();
      await new Promise((r) => setTimeout(r, 50));

      expect(detectCalls).toEqual(['/ws/batch_vid_1.mp4', '/ws/batch_vid_2.mp4']);
      expect(createdJobBoxes.length).toBe(2);
      expect(createdJobBoxes[0]).toEqual({ x: 0.1, y: 0.8, width: 0.8, height: 0.15 });
      expect(createdJobBoxes[1]).toEqual({ x: 0.05, y: 0.7, width: 0.9, height: 0.25 });

      expect(batchStore.tasks[0].result?.effectiveRegion).toEqual({ x: 0.1, y: 0.8, width: 0.8, height: 0.15 });
      expect(batchStore.tasks[1].result?.effectiveRegion).toEqual({ x: 0.05, y: 0.7, width: 0.9, height: 0.25 });
    });

    it('Policy "fixed": applies user region box directly to all items in batch with ZERO detect-region calls', async () => {
      const batchStore = useBatchStore();
      const customBox1 = { x: 0.15, y: 0.72, width: 0.7, height: 0.2 };
      const customBox2 = { x: 0.05, y: 0.65, width: 0.9, height: 0.3 };

      batchStore.addBatchItems([
        {
          videoPath: '/ws/fixed_vid_1.mp4',
          config: { roi_policy: 'fixed', region_box: customBox1, engine: 'vision' },
        },
        {
          videoPath: '/ws/fixed_vid_2.mp4',
          config: { roi_policy: 'fixed', region_box: customBox2, engine: 'vision' },
        },
      ]);

      let detectCalled = false;
      SubLiftApiClient.detectSubtitleRegion = async () => {
        detectCalled = true;
        return { detected: false } as any;
      };

      const capturedBoxes: any[] = [];
      SubLiftApiClient.createJob = async (cfg) => {
        capturedBoxes.push(cfg.region_box);
        return { job_id: `job-fixed-${capturedBoxes.length}`, status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        callbacks.onDone?.({ job_id: _id, total_entries: 0, elapsed_ms: 200 });
        return () => {};
      };

      batchStore.startQueue();
      await new Promise((r) => setTimeout(r, 50));

      expect(detectCalled).toBe(false);
      expect(capturedBoxes.length).toBe(2);
      expect(capturedBoxes[0]).toEqual(customBox1);
      expect(capturedBoxes[1]).toEqual(customBox2);
      expect(batchStore.tasks[0].result?.roiSource).toBe('用户固定选区');
      expect(batchStore.tasks[1].result?.roiSource).toBe('用户固定选区');
    });

    it('Policy "fixed": falls back to DEFAULT_BOTTOM_ROI if region_box was omitted or null', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/ws/fixed_null_box.mp4',
          config: { roi_policy: 'fixed', region_box: null, engine: 'vision' },
        },
      ]);

      let capturedBox: any = null;
      SubLiftApiClient.createJob = async (cfg) => {
        capturedBox = cfg.region_box;
        return { job_id: 'job-fixed-null', status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        callbacks.onDone?.({ job_id: 'job-fixed-null', total_entries: 0, elapsed_ms: 200 });
        return () => {};
      };

      await batchStore.startSingle(batchStore.tasks[0].id);

      expect(capturedBox).toEqual(DEFAULT_BOTTOM_ROI);
    });

    it('Policy "default": explicitly passes bottom 30% area with ZERO detect-region calls', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/ws/default_policy.mp4',
          config: { roi_policy: 'default', engine: 'paddle', quality: 'fast' },
        },
      ]);

      let detectCalled = false;
      SubLiftApiClient.detectSubtitleRegion = async () => {
        detectCalled = true;
        return { detected: false } as any;
      };

      let capturedJobConfig: JobConfig | null = null;
      SubLiftApiClient.createJob = async (cfg) => {
        capturedJobConfig = cfg;
        return { job_id: 'job-default-1', status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        callbacks.onDone?.({ job_id: 'job-default-1', total_entries: 0, elapsed_ms: 150 });
        return () => {};
      };

      await batchStore.startSingle(batchStore.tasks[0].id);

      expect(detectCalled).toBe(false);
      expect(capturedJobConfig!.fps).toBe(5.0);
      expect(capturedJobConfig!.region_box).toEqual(DEFAULT_BOTTOM_ROI);
      expect(batchStore.tasks[0].result?.roiSource).toBe('默认底边 30%');
    });
  });

  describe('2. Configuration Immutability Under Active Execution', () => {
    it('freezes task configuration and rejects all mutation attempts during extraction', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/ws/immutable_test.mp4',
          config: {
            engine: 'vision',
            quality: 'fast',
            confidence_threshold: 0.2,
            roi_policy: 'fixed',
            region_box: { x: 0.1, y: 0.7, width: 0.8, height: 0.25 },
            script: 'Hans',
          },
        },
      ]);

      const task = batchStore.tasks[0];
      const sse: { cb: SseJobCallbacks | null } = { cb: null };
      let capturedJobConfig: JobConfig | null = null;

      SubLiftApiClient.createJob = async (cfg) => {
        capturedJobConfig = cfg;
        return { job_id: 'job-immutable-1', status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        sse.cb = callbacks;
        return () => {};
      };

      const startPromise = batchStore.startSingle(task.id);
      await new Promise((r) => setTimeout(r, 10));

      expect(task.status).toBe('extracting');
      expect(Object.isFrozen(task.config)).toBe(true);

      // Attempt 1: Direct property modification
      expect(() => {
        (task.config as any).quality = 'fine';
      }).toThrow();

      // Attempt 2: Store helper updateTaskConfig
      const updateResult = batchStore.updateTaskConfig(task.id, { quality: 'fine', confidence_threshold: 0.9 });
      expect(updateResult).toBe(false);
      expect(task.config.quality).toBe('fast');
      expect(task.config.confidence_threshold).toBe(0.2);

      // Attempt 3: Modify global store defaults while task is running
      const systemStore = useSystemStore();
      systemStore.systemInfo!.engines[0].available = false; // vision unavailable, fallback preferred engine to paddle
      expect(batchStore.defaultBatchConfig.engine).toBe('paddle');

      // The running task must retain its frozen vision configuration
      expect(task.config.engine).toBe('vision');
      expect(capturedJobConfig!.engine).toBe('vision');
      expect(capturedJobConfig!.fps).toBe(5.0);
      expect(capturedJobConfig!.confidence_threshold).toBe(0.2);
      expect(capturedJobConfig!.script).toBe('Hans');

      sse.cb?.onDone?.({ job_id: 'job-immutable-1', total_entries: 0, elapsed_ms: 300 });
      await startPromise;
      expect(task.config.engine).toBe('vision');
    });

    it('retains pre-configured task snapshots in queued tasks when store defaults change mid-queue', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/ws/task1.mp4',
          config: { engine: 'vision', quality: 'fast', confidence_threshold: 0.1 },
        },
        {
          videoPath: '/ws/task2_custom.mp4',
          config: { engine: 'paddle', quality: 'fine', confidence_threshold: 0.6, script: 'Hant' },
        },
      ]);

      const executedConfigs: JobConfig[] = [];
      const sseCallbacks: Record<string, SseJobCallbacks> = {};
      let jobSeq = 0;

      SubLiftApiClient.createJob = async (cfg) => {
        jobSeq++;
        executedConfigs.push(cfg);
        return { job_id: `job-queue-imm-${jobSeq}`, status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = (id, callbacks) => {
        sseCallbacks[id] = callbacks;
        return () => {};
      };

      // Start queue processing Task 1
      batchStore.startQueue();
      await new Promise((r) => setTimeout(r, 10));

      expect(batchStore.currentRunningId).toBe(batchStore.tasks[0].id);

      // While Task 1 is running, simulate global defaults change by shifting preferred engine to paddle
      const systemStore = useSystemStore();
      systemStore.systemInfo!.engines = [
        { name: 'vision', available: false, detail: 'Vision unavailable' },
        { name: 'paddle', available: true, detail: 'Paddle' },
        { name: 'mock', available: true, detail: 'Mock' },
      ];
      expect(batchStore.defaultBatchConfig.engine).toBe('paddle');

      // Finish Task 1
      sseCallbacks['job-queue-imm-1']?.onDone?.({
        job_id: 'job-queue-imm-1',
        total_entries: 0,
        elapsed_ms: 200,
      });
      await new Promise((r) => setTimeout(r, 20));

      // Task 2 should now be executing with its OWN configured parameters (paddle, fine/12 FPS, 0.6, Hant)
      expect(executedConfigs.length).toBe(2);
      expect(executedConfigs[0].engine).toBe('vision');
      expect(executedConfigs[0].fps).toBe(5.0);

      expect(executedConfigs[1].engine).toBe('paddle');
      expect(executedConfigs[1].fps).toBe(12.0);
      expect(executedConfigs[1].confidence_threshold).toBe(0.6);
      expect(executedConfigs[1].script).toBe('Hant');
    });

    it('workbenchStore: freezes activeConfigSnapshot on startExtraction, immune to subsequent store property mutations', async () => {
      const workbenchStore = useWorkbenchStore();
      workbenchStore.loadVideo('/ws/workbench_imm.mp4');
      workbenchStore.selectedEngine = 'vision';
      workbenchStore.quality = 'balanced'; // 8 FPS
      workbenchStore.confidenceThreshold = 0.35;
      workbenchStore.updateRegionBox({ x: 0.1, y: 0.6, width: 0.8, height: 0.3 });
      workbenchStore.script = 'Latn';

      let capturedConfig: JobConfig | null = null;
      SubLiftApiClient.createJob = async (cfg) => {
        capturedConfig = cfg;
        return { job_id: 'wb-job-imm', status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = () => () => {};

      await workbenchStore.startExtraction();

      expect(workbenchStore.state).toBe('Processing');
      expect(workbenchStore.activeConfigSnapshot).not.toBeNull();
      expect(Object.isFrozen(workbenchStore.activeConfigSnapshot)).toBe(true);

      // Mutate mutable properties on workbench store while extraction is active
      workbenchStore.quality = 'fine';
      workbenchStore.selectedEngine = 'paddle';
      workbenchStore.confidenceThreshold = 0.99;
      workbenchStore.updateRegionBox({ x: 0, y: 0, width: 1, height: 1 }); // Should be blocked by isLocked
      workbenchStore.script = 'Grek';

      // Snapshot must retain the values at time of startExtraction
      expect(workbenchStore.activeConfigSnapshot!.quality).toBe('balanced');
      expect(workbenchStore.activeConfigSnapshot!.engine).toBe('vision');
      expect(workbenchStore.activeConfigSnapshot!.confidence_threshold).toBe(0.35);
      expect(workbenchStore.activeConfigSnapshot!.region_box).toEqual({ x: 0.1, y: 0.6, width: 0.8, height: 0.3 });
      expect(workbenchStore.activeConfigSnapshot!.script).toBe('Latn');

      expect(capturedConfig!.fps).toBe(8.0);
      expect(capturedConfig!.engine).toBe('vision');
      expect(capturedConfig!.confidence_threshold).toBe(0.35);
      expect(capturedConfig!.region_box).toEqual({ x: 0.1, y: 0.6, width: 0.8, height: 0.3 });
      expect(capturedConfig!.script).toBe('Latn');
    });
  });

  describe('3. Single Video vs Batch Parity & Output Consistency', () => {
    it('produces identical createJob requests and identical subtitle entries between workbench and batch mode', async () => {
      const workbenchStore = useWorkbenchStore();
      const batchStore = useBatchStore();

      const testVideo = '/ws/shared_parity_video.mp4';
      const testRoi = { x: 0.05, y: 0.75, width: 0.9, height: 0.22 };
      const commonConfig = {
        engine: 'vision' as const,
        quality: 'fine' as const, // 12 FPS
        confidence_threshold: 0.45,
        region_box: testRoi,
        script: 'Hans',
      };

      const emittedEntries: SubtitleEntry[] = [
        { index: 1, start_ms: 1000, end_ms: 2500, text: '第一行字幕测试', confidence: 0.98 },
        { index: 2, start_ms: 3000, end_ms: 4800, text: '第二行字幕测试', confidence: 0.95 },
        { index: 3, start_ms: 5000, end_ms: 7200, text: '第三行字幕测试', confidence: 0.99 },
      ];

      const capturedRequests: JobConfig[] = [];
      SubLiftApiClient.createJob = async (cfg) => {
        capturedRequests.push(cfg);
        return { job_id: `job-parity-${capturedRequests.length}`, status: 'running' };
      };

      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        emittedEntries.forEach((entry) => callbacks.onPushEntry?.({ entry }));
        callbacks.onDone?.({ total_entries: emittedEntries.length, elapsed_ms: 750 });
        return () => {};
      };

      // 1. Run Single Workbench
      workbenchStore.loadVideo(testVideo);
      workbenchStore.selectedEngine = commonConfig.engine;
      workbenchStore.quality = commonConfig.quality;
      workbenchStore.confidenceThreshold = commonConfig.confidence_threshold;
      workbenchStore.updateRegionBox(commonConfig.region_box);
      workbenchStore.script = commonConfig.script;

      await workbenchStore.startExtraction();

      // 2. Run Batch Mode
      batchStore.addBatchItems([
        {
          videoPath: testVideo,
          config: {
            engine: commonConfig.engine,
            quality: commonConfig.quality,
            confidence_threshold: commonConfig.confidence_threshold,
            roi_policy: 'fixed',
            region_box: commonConfig.region_box,
            script: commonConfig.script,
          },
        },
      ]);

      await batchStore.startSingle(batchStore.tasks[0].id);

      // Verify request parity
      expect(capturedRequests.length).toBe(2);
      const [singleReq, batchReq] = capturedRequests;

      expect(singleReq.video_path).toBe(batchReq.video_path);
      expect(singleReq.engine).toBe(batchReq.engine);
      expect(singleReq.fps).toBe(batchReq.fps);
      expect(singleReq.fps).toBe(12.0);
      expect(singleReq.confidence_threshold).toBe(batchReq.confidence_threshold);
      expect(singleReq.confidence_threshold).toBe(0.45);
      expect(singleReq.region_box).toEqual(batchReq.region_box);
      expect(singleReq.region_box).toEqual(testRoi);
      expect(singleReq.script).toBe(batchReq.script);
      expect(singleReq.script).toBe('Hans');

      // Verify subtitle entries parity
      expect(workbenchStore.entries).toEqual(batchStore.tasks[0].entries);
      expect(workbenchStore.entries.length).toBe(3);
    });
  });

  describe('4. Fail-Closed Engine Availability & Batch Resilience', () => {
    it('fails closed immediately when engine is unavailable without silent fallback, allowing subsequent tasks to proceed', async () => {
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

      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/ws/task1_ok.mp4',
          config: { engine: 'vision' },
        },
        {
          videoPath: '/ws/task2_fail.mp4',
          config: { engine: 'paddle' }, // Unavailable!
        },
        {
          videoPath: '/ws/task3_ok.mp4',
          config: { engine: 'vision' },
        },
      ]);

      const executedPaths: string[] = [];
      const sseCallbacks: Record<string, SseJobCallbacks> = {};
      let jobSeq = 0;

      SubLiftApiClient.createJob = async (cfg) => {
        jobSeq++;
        executedPaths.push(cfg.video_path);
        return { job_id: `job-mixed-${jobSeq}`, status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = (id, callbacks) => {
        sseCallbacks[id] = callbacks;
        return () => {};
      };

      batchStore.startQueue();
      await new Promise((r) => setTimeout(r, 10));

      // Task 1 running
      expect(batchStore.currentRunningId).toBe(batchStore.tasks[0].id);
      sseCallbacks['job-mixed-1']?.onDone?.({ job_id: 'job-mixed-1', total_entries: 0, elapsed_ms: 200 });
      await new Promise((r) => setTimeout(r, 20));

      // Task 2 should have failed closed immediately without calling createJob
      expect(batchStore.tasks[1].status).toBe('failed');
      expect(batchStore.tasks[1].error).toContain('OCR 引擎不可用: paddle (严格禁止静默回退)');

      // Queue should have smoothly advanced to Task 3
      expect(batchStore.currentRunningId).toBe(batchStore.tasks[2].id);
      sseCallbacks['job-mixed-2']?.onDone?.({ job_id: 'job-mixed-2', total_entries: 0, elapsed_ms: 200 });
      await new Promise((r) => setTimeout(r, 20));

      expect(batchStore.tasks[0].status).toBe('completed');
      expect(batchStore.tasks[1].status).toBe('failed');
      expect(batchStore.tasks[2].status).toBe('completed');

      // createJob was called only for task 1 and task 3
      expect(executedPaths).toEqual(['/ws/task1_ok.mp4', '/ws/task3_ok.mp4']);
    });
  });
});
