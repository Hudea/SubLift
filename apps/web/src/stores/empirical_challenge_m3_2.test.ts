import { describe, it, expect, beforeEach } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import { useWorkbenchStore } from './workbench';
import { useBatchStore } from './batch';
import { useSystemStore } from './system';
import { SubLiftApiClient } from '../api/client';
import {
  DEFAULT_BOTTOM_ROI,
} from '../types/config';
import type {
  OcrEngineName,
  SseJobCallbacks,
} from '../types/api';

describe('Empirical Challenge 2 - Phase 12 Milestone 3 (Feature 12513)', () => {
  beforeEach(() => {
    setActivePinia(createPinia());
    const systemStore = useSystemStore();
    systemStore.systemInfo = {
      version: 'v0.1.0-challenge',
      runtime: 'cpp',
      capabilities: ['mock', 'vision', 'paddle'],
      engines: [
        { name: 'mock', available: true, detail: 'Mock Engine for tests' },
        { name: 'vision', available: true, detail: 'Apple Vision (macOS)' },
        { name: 'paddle', available: false, detail: 'PaddleOCR weights missing' },
      ],
      ffmpeg: { available: true, path: '/opt/homebrew/bin/ffmpeg' },
    };
  });

  describe('1. Fail-Closed Behavior for Unsupported & Unavailable OCR Engines', () => {
    it('workbenchStore: strictly rejects unavailable or unsupported engine without calling createJob', async () => {
      const workbenchStore = useWorkbenchStore();
      workbenchStore.loadVideo('/test/video.mp4');

      let createJobCalled = false;
      SubLiftApiClient.createJob = async () => {
        createJobCalled = true;
        return { job_id: 'job-unsupported', status: 'running' };
      };

      // 1.1 Test unavailable engine ('paddle', available: false)
      workbenchStore.selectedEngine = 'paddle';
      await expect(workbenchStore.startExtraction()).rejects.toThrow(
        /所选 OCR 引擎不可用: paddle \(严格禁止静默回退\)/
      );
      expect(workbenchStore.state).toBe('Failed');
      expect(workbenchStore.errorMessage).toContain('严格禁止静默回退');
      expect(createJobCalled).toBe(false);

      // 1.2 Test unknown engine ('tesseract')
      workbenchStore.loadVideo('/test/video.mp4');
      workbenchStore.selectedEngine = 'tesseract' as unknown as OcrEngineName;
      await expect(workbenchStore.startExtraction()).rejects.toThrow(
        /所选 OCR 引擎不可用: tesseract \(严格禁止静默回退\)/
      );
      expect(workbenchStore.state).toBe('Failed');
      expect(workbenchStore.errorMessage).toContain('严格禁止静默回退');
      expect(createJobCalled).toBe(false);
    });

    it('batchStore: fails single task with unavailable engine immediately and auto-advances queue without deadlock', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/test/v1_vision.mp4',
          config: { engine: 'vision', quality: 'fast' },
        },
        {
          videoPath: '/test/v2_paddle_unavailable.mp4',
          config: { engine: 'paddle', quality: 'fast' },
        },
        {
          videoPath: '/test/v3_mock.mp4',
          config: { engine: 'mock', quality: 'fast' },
        },
      ]);

      const executedJobs: string[] = [];
      let sseCallbacks: SseJobCallbacks = {};

      SubLiftApiClient.detectSubtitleRegion = async () => ({
        detected: false,
        sample_time_s: 0,
        suggested_box: { ...DEFAULT_BOTTOM_ROI },
        preview_text: '',
        confidence: 0,
        total_candidates: 0,
      });

      SubLiftApiClient.createJob = async (dto) => {
        executedJobs.push(`${dto.video_path} [${dto.engine}]`);
        return { job_id: `job-${dto.engine}`, status: 'running' };
      };

      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        sseCallbacks = callbacks;
        // Auto-complete immediately for running jobs
        queueMicrotask(() => {
          sseCallbacks.onDone?.({
            job_id: _id,
            total_entries: 2,
            elapsed_ms: 100,
          });
        });
        return () => {
          sseCallbacks = {};
        };
      };

      batchStore.startQueue();

      // Give microtasks time to advance through all 3 tasks
      for (let i = 0; i < 20; ++i) {
        await new Promise((r) => setTimeout(r, 10));
        if (!batchStore.isQueueRunning && batchStore.stats.waiting === 0 && batchStore.stats.active === 0) {
          break;
        }
      }

      // Assertions
      expect(batchStore.tasks[0].status).toBe('completed');
      expect(batchStore.tasks[1].status).toBe('failed');
      expect(batchStore.tasks[1].error).toContain('OCR 引擎不可用: paddle (严格禁止静默回退)');
      expect(batchStore.tasks[2].status).toBe('completed');

      expect(batchStore.stats.total).toBe(3);
      expect(batchStore.stats.completed).toBe(2);
      expect(batchStore.stats.failed).toBe(1);
      expect(batchStore.stats.waiting).toBe(0);
      expect(batchStore.isQueueRunning).toBe(false);

      // Verify createJob was called only for valid engines (v1 and v3), never v2
      expect(executedJobs).toEqual([
        '/test/v1_vision.mp4 [vision]',
        '/test/v3_mock.mp4 [mock]',
      ]);
    });

    it('batchStore startSingle: fails cleanly on unavailable engine and resets running state', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/test/v_paddle.mp4',
          config: { engine: 'paddle' },
        },
      ]);

      const taskId = batchStore.tasks[0].id;
      const started = await batchStore.startSingle(taskId);

      expect(started).toBe(true);
      expect(batchStore.tasks[0].status).toBe('failed');
      expect(batchStore.tasks[0].error).toContain('OCR 引擎不可用: paddle (严格禁止静默回退)');
      expect(batchStore.currentRunningId).toBeNull();
      expect(batchStore.isQueueRunning).toBe(false);
    });
  });

  describe('2. UI Diagnostics & Visibility in Inspector and Task Row', () => {
    it('inspectorModel: accurately displays engine unavailable callouts and error messages', () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/test/v_paddle.mp4',
          config: { engine: 'paddle' },
        },
        {
          videoPath: '/test/v_mock.mp4',
          config: { engine: 'mock' },
        },
      ]);

      // Select Task 1 (Paddle - unavailable)
      batchStore.selectTask(batchStore.tasks[0].id);
      const inspector1 = batchStore.inspectorModel;
      expect(inspector1).not.toBeNull();
      expect(inspector1?.isEngineAvailable).toBe(false);
      expect(inspector1?.engineUnavailableReason).toContain('当前环境未就绪 PaddleOCR，严格禁止静默回退');

      // Select Task 2 (Mock - available)
      batchStore.selectTask(batchStore.tasks[1].id);
      const inspector2 = batchStore.inspectorModel;
      expect(inspector2).not.toBeNull();
      expect(inspector2?.isEngineAvailable).toBe(true);
      expect(inspector2?.engineUnavailableReason).toBeUndefined();
    });

    it('inspectorModel: displays task failure message with retry capability', () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([{ videoPath: '/test/v_fail.mp4' }]);
      const task = batchStore.tasks[0];
      task.status = 'failed';
      task.error = 'FFmpeg frame decode failed: corrupted MOOV atom';

      batchStore.selectTask(task.id);
      const inspector = batchStore.inspectorModel;
      expect(inspector).not.toBeNull();
      expect(inspector?.status).toBe('failed');
      expect(inspector?.statusName).toBe('失败');
      expect(inspector?.failureMessage).toBe('FFmpeg frame decode failed: corrupted MOOV atom');
      expect(inspector?.canRetry).toBe(true);
      expect(inspector?.canStartSingle).toBe(false);
      expect(inspector?.canCancel).toBe(false);
      expect(inspector?.canEditConfiguration).toBe(false);
    });
  });

  describe('3. ROI Policy Execution & Inspector State Consistency (No Stale State)', () => {
    it('roi_policy: auto with successful region detection reflects preview and suggested box', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/test/v_auto_hit.mp4',
          config: { engine: 'mock', roi_policy: 'auto' },
        },
      ]);

      const detectedBox = { x: 0.12, y: 0.82, width: 0.76, height: 0.14 };
      SubLiftApiClient.detectSubtitleRegion = async () => ({
        detected: true,
        sample_time_s: 5.0,
        suggested_box: detectedBox,
        preview_text: '欢迎收看 SubLift',
        confidence: 0.98,
        total_candidates: 3,
      });

      let submittedDto: any = null;
      let sseCallbacks: SseJobCallbacks = {};

      SubLiftApiClient.createJob = async (dto) => {
        submittedDto = dto;
        return { job_id: 'job-auto-hit', status: 'running' };
      };

      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        sseCallbacks = callbacks;
        queueMicrotask(() => {
          sseCallbacks.onDone?.({
            job_id: _id,
            total_entries: 5,
            elapsed_ms: 250,
          });
        });
        return () => {
          sseCallbacks = {};
        };
      };

      // In waiting state, check inspector
      batchStore.selectTask(batchStore.tasks[0].id);
      expect(batchStore.inspectorModel?.roi_source_display).toBe('准备阶段独立检测');

      await batchStore.startSingle(batchStore.tasks[0].id);
      await new Promise((r) => setTimeout(r, 20));

      const task = batchStore.tasks[0];
      expect(task.status).toBe('completed');
      expect(submittedDto.region_box).toEqual(detectedBox);
      expect(task.result?.roiSource).toBe('自动检测：“欢迎收看 SubLift”');
      expect(task.result?.effectiveRegion).toEqual(detectedBox);

      // Check Inspector after completion
      const inspector = batchStore.inspectorModel;
      expect(inspector?.roi_source_display).toBe('自动检测：“欢迎收看 SubLift”');
      expect(inspector?.region_box).toEqual(detectedBox);
    });

    it('roi_policy: auto with undetected fallback reflects default 30% bottom box', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/test/v_auto_miss.mp4',
          config: { engine: 'mock', roi_policy: 'auto' },
        },
      ]);

      SubLiftApiClient.detectSubtitleRegion = async () => ({
        detected: false,
        sample_time_s: 0,
        suggested_box: { ...DEFAULT_BOTTOM_ROI },
        preview_text: '',
        confidence: 0,
        total_candidates: 0,
      });

      let submittedDto: any = null;
      let sseCallbacks: SseJobCallbacks = {};

      SubLiftApiClient.createJob = async (dto) => {
        submittedDto = dto;
        return { job_id: 'job-auto-miss', status: 'running' };
      };

      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        sseCallbacks = callbacks;
        queueMicrotask(() => {
          sseCallbacks.onDone?.({
            job_id: _id,
            total_entries: 0,
            elapsed_ms: 150,
          });
        });
        return () => {
          sseCallbacks = {};
        };
      };

      await batchStore.startSingle(batchStore.tasks[0].id);
      await new Promise((r) => setTimeout(r, 20));

      const task = batchStore.tasks[0];
      expect(task.status).toBe('completed');
      expect(submittedDto.region_box).toEqual(DEFAULT_BOTTOM_ROI);
      expect(task.result?.roiSource).toBe('自动检测未命中字幕，回退默认底边 30%');
      expect(task.result?.effectiveRegion).toEqual(DEFAULT_BOTTOM_ROI);

      // Check Inspector
      batchStore.selectTask(task.id);
      const inspector = batchStore.inspectorModel;
      expect(inspector?.roi_source_display).toBe('自动检测未命中字幕，回退默认底边 30%');
      expect(inspector?.region_box).toEqual(DEFAULT_BOTTOM_ROI);
    });

    it('roi_policy: fixed and default policies bypass detectSubtitleRegion', async () => {
      const batchStore = useBatchStore();
      const customFixedBox = { x: 0.1, y: 0.5, width: 0.8, height: 0.4 };

      batchStore.addBatchItems([
        {
          videoPath: '/test/v_fixed.mp4',
          config: { engine: 'mock', roi_policy: 'fixed', region_box: customFixedBox },
        },
        {
          videoPath: '/test/v_default.mp4',
          config: { engine: 'mock', roi_policy: 'default' },
        },
      ]);

      let detectCalled = false;
      SubLiftApiClient.detectSubtitleRegion = async () => {
        detectCalled = true;
        return {
          detected: true,
          sample_time_s: 0,
          suggested_box: { ...DEFAULT_BOTTOM_ROI },
          preview_text: '',
          confidence: 1,
          total_candidates: 1,
        };
      };

      const submittedBoxes: any[] = [];
      SubLiftApiClient.createJob = async (dto) => {
        submittedBoxes.push(dto.region_box);
        return { job_id: 'job-pol', status: 'running' };
      };

      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        queueMicrotask(() => {
          callbacks.onDone?.({
            job_id: _id,
            total_entries: 1,
            elapsed_ms: 50,
          });
        });
        return () => {};
      };

      // Run fixed task
      await batchStore.startSingle(batchStore.tasks[0].id);
      await new Promise((r) => setTimeout(r, 20));

      // Run default task
      await batchStore.startSingle(batchStore.tasks[1].id);
      await new Promise((r) => setTimeout(r, 20));

      expect(detectCalled).toBe(false);
      expect(submittedBoxes[0]).toEqual(customFixedBox);
      expect(submittedBoxes[1]).toEqual(DEFAULT_BOTTOM_ROI);

      expect(batchStore.tasks[0].result?.roiSource).toBe('用户固定选区');
      expect(batchStore.tasks[1].result?.roiSource).toBe('默认底边 30%');
    });

    it('switching selected task in Inspector never leaks stale state across tasks', () => {
      const batchStore = useBatchStore();
      const fixedBox = { x: 0.2, y: 0.6, width: 0.6, height: 0.25 };

      batchStore.addBatchItems([
        {
          videoPath: '/path/A/movie.mp4',
          config: { engine: 'vision', quality: 'fine', roi_policy: 'auto' },
        },
        {
          videoPath: '/path/B/clip.mp4',
          config: { engine: 'paddle', quality: 'fast', roi_policy: 'fixed', region_box: fixedBox },
        },
        {
          videoPath: '/path/C/doc.mp4',
          config: { engine: 'mock', quality: 'balanced', roi_policy: 'default' },
        },
      ]);

      // Set different states and results
      batchStore.tasks[0].status = 'completed';
      batchStore.tasks[0].entries = Array.from({ length: 42 }, (_, i) => ({
        index: i + 1,
        start_ms: i * 1000,
        end_ms: (i + 1) * 1000,
        text: `Line ${i + 1}`,
        confidence: 0.99,
      }));
      batchStore.tasks[0].result = {
        entryCount: 42,
        outputPath: '/path/A/movie.srt',
        runtimeIdentity: 'VISION Engine',
        roiSource: '自动检测：“Movie Line”',
        effectiveRegion: { x: 0.05, y: 0.78, width: 0.9, height: 0.18 },
      };

      batchStore.tasks[1].status = 'failed';
      batchStore.tasks[1].error = 'OCR 引擎不可用: paddle (严格禁止静默回退)';

      batchStore.tasks[2].status = 'waiting';

      // 1. Select Task A
      batchStore.selectTask(batchStore.tasks[0].id);
      let ins = batchStore.inspectorModel!;
      expect(ins.id).toBe(batchStore.tasks[0].id);
      expect(ins.filename).toBe('movie.mp4');
      expect(ins.status).toBe('completed');
      expect(ins.engineDisplay).toBe('Apple Vision');
      expect(ins.qualityDisplay).toContain('精细 (12 FPS)');
      expect(ins.entryCount).toBe(42);
      expect(ins.roi_source_display).toBe('自动检测：“Movie Line”');
      expect(ins.region_box).toEqual({ x: 0.05, y: 0.78, width: 0.9, height: 0.18 });
      expect(ins.failureMessage).toBeUndefined();
      expect(ins.canEditConfiguration).toBe(false);

      // 2. Select Task B
      batchStore.selectTask(batchStore.tasks[1].id);
      ins = batchStore.inspectorModel!;
      expect(ins.id).toBe(batchStore.tasks[1].id);
      expect(ins.filename).toBe('clip.mp4');
      expect(ins.status).toBe('failed');
      expect(ins.engineDisplay).toBe('PaddleOCR');
      expect(ins.qualityDisplay).toContain('快速 (5 FPS)');
      expect(ins.entryCount).toBe(0);
      expect(ins.isEngineAvailable).toBe(false);
      expect(ins.failureMessage).toContain('OCR 引擎不可用: paddle');
      expect(ins.roi_source_display).toBe('用户固定选区');
      expect(ins.region_box).toEqual(fixedBox);
      expect(ins.canEditConfiguration).toBe(false);

      // 3. Select Task C
      batchStore.selectTask(batchStore.tasks[2].id);
      ins = batchStore.inspectorModel!;
      expect(ins.id).toBe(batchStore.tasks[2].id);
      expect(ins.filename).toBe('doc.mp4');
      expect(ins.status).toBe('waiting');
      expect(ins.engineDisplay).toBe('Mock Engine');
      expect(ins.qualityDisplay).toContain('平衡 (8 FPS)');
      expect(ins.entryCount).toBe(0);
      expect(ins.isEngineAvailable).toBe(true);
      expect(ins.failureMessage).toBeUndefined();
      expect(ins.roi_source_display).toBe('默认底边 30%');
      expect(ins.region_box).toBeNull();
      expect(ins.canEditConfiguration).toBe(true);

      // 4. Select null
      batchStore.selectTask(null);
      expect(batchStore.inspectorModel).toBeNull();

      // 5. Select Task A again
      batchStore.selectTask(batchStore.tasks[0].id);
      ins = batchStore.inspectorModel!;
      expect(ins.id).toBe(batchStore.tasks[0].id);
      expect(ins.entryCount).toBe(42);
      expect(ins.roi_source_display).toBe('自动检测：“Movie Line”');
      expect(ins.failureMessage).toBeUndefined();
    });

    it('freezes task configuration snapshot upon execution entry point', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/test/v_freeze.mp4',
          config: { engine: 'mock', quality: 'fast', roi_policy: 'auto' },
        },
      ]);

      const task = batchStore.tasks[0];
      let submittedFps = 0;

      SubLiftApiClient.detectSubtitleRegion = async () => ({
        detected: false,
        sample_time_s: 0,
        suggested_box: { ...DEFAULT_BOTTOM_ROI },
        preview_text: '',
        confidence: 0,
        total_candidates: 0,
      });

      SubLiftApiClient.createJob = async (dto) => {
        submittedFps = dto.fps;
        // Attempt to mutate task config while creating job
        try {
          (task.config as any).quality = 'fine';
        } catch {
          // Object.freeze throws in strict mode
        }
        return { job_id: 'job-freeze', status: 'running' };
      };

      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        queueMicrotask(() => {
          callbacks.onDone?.({
            job_id: _id,
            total_entries: 0,
            elapsed_ms: 10,
          });
        });
        return () => {};
      };

      await batchStore.startSingle(task.id);
      await new Promise((r) => setTimeout(r, 20));

      expect(submittedFps).toBe(5.0); // Fast quality = 5 FPS
      expect(task.config.quality).toBe('fast');
      expect(Object.isFrozen(task.config)).toBe(true);
    });
  });
});
