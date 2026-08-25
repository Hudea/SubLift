import { describe, it, expect, beforeEach, vi } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import { useBatchStore } from './batch';
import { SubLiftApiClient } from '../api/client';
import type { SseJobCallbacks } from '../types/api';

describe('useBatchStore - Batch Queue & Native Alignment (Features 12301, 12503 & Phase 8)', () => {
  beforeEach(() => {
    setActivePinia(createPinia());
  });

  it('initializes with empty tasks, preferred engine and fast 5 FPS default config', () => {
    const batchStore = useBatchStore();
    expect(batchStore.tasks.length).toBe(0);
    expect(batchStore.stats.total).toBe(0);
    expect(batchStore.isQueueRunning).toBe(false);
    expect(batchStore.canStartAll).toBe(false);
    expect(batchStore.defaultBatchConfig.quality).toBe('fast');
  });

  it('adds multiple items, generates expected srt paths and deduplicates', () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([
      { videoPath: '/path/to/video1.mp4' },
      { videoPath: '/path/to/video2.mov', importRootPath: 'Season 1/' },
    ]);
    expect(batchStore.tasks.length).toBe(2);
    expect(batchStore.stats.waiting).toBe(2);
    expect(batchStore.canStartAll).toBe(true);
    expect(batchStore.tasks[0].outputPath).toBe('/path/to/video1.mp4.srt'.replace(/\.mp4\.srt$/, '.srt'));
    expect(batchStore.tasks[1].importRootPath).toBe('Season 1/');
    expect(batchStore.selectedTaskId).toBe(batchStore.tasks[0].id);

    // Deduplication
    batchStore.addBatchItems([{ videoPath: '/path/to/video1.mp4' }]);
    expect(batchStore.tasks.length).toBe(2);
  });

  it('allows editing configuration only in waiting status', () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([{ videoPath: '/path/to/v1.mp4' }]);
    const task = batchStore.tasks[0];

    // Waiting -> can update
    const ok = batchStore.updateTaskConfig(task.id, { quality: 'fine', engine: 'paddle' });
    expect(ok).toBe(true);
    expect(task.config.quality).toBe('fine');
    expect(task.config.engine).toBe('paddle');

    // Manually transition to extracting -> cannot update
    task.status = 'extracting';
    const failedUpdate = batchStore.updateTaskConfig(task.id, { quality: 'fast' });
    expect(failedUpdate).toBe(false);
    expect(task.config.quality).toBe('fine');
  });

  it('preserves physical slots of non-waiting tasks during moveTaskUp / moveTaskDown', () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([
      { videoPath: '/path/to/v1.mp4' },
      { videoPath: '/path/to/v2.mp4' },
      { videoPath: '/path/to/v3.mp4' },
    ]);

    // Set v2 as completed (non-waiting)
    batchStore.tasks[1].status = 'completed';

    // Queue is: [v1 (waiting), v2 (completed), v3 (waiting)]
    // Moving v3 up should swap with v1, leaving v2 in the middle
    batchStore.moveTaskUp(batchStore.tasks[2].id);

    expect(batchStore.tasks[0].videoPath).toBe('/path/to/v3.mp4');
    expect(batchStore.tasks[1].videoPath).toBe('/path/to/v2.mp4');
    expect(batchStore.tasks[1].status).toBe('completed');
    expect(batchStore.tasks[2].videoPath).toBe('/path/to/v1.mp4');
  });

  it('executes startSingle independently and pauses queue upon completion', async () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([
      { videoPath: '/path/to/v1.mp4' },
      { videoPath: '/path/to/v2.mp4' },
    ]);

    const sse: { cb: SseJobCallbacks | null } = { cb: null };
    SubLiftApiClient.createJob = async () => ({ job_id: 'job-single-1', status: 'running' });
    SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
      sse.cb = callbacks;
      return () => {
        sse.cb = null;
      };
    };

    // startSingle on task 2
    const singlePromise = batchStore.startSingle(batchStore.tasks[1].id);
    await new Promise((r) => setTimeout(r, 10));

    expect(batchStore.currentRunningId).toBe(batchStore.tasks[1].id);
    expect(batchStore.tasks[1].status).toBe('extracting');
    expect(batchStore.tasks[0].status).toBe('waiting');

    // Finish single task
    sse.cb?.onDone?.({
      job_id: 'job-single-1',
      status: 'completed',
      total_entries: 3,
      elapsed_ms: 1500,
      video_path: '/path/to/v2.mp4',
    });
    await singlePromise;
    await new Promise((r) => setTimeout(r, 10));

    expect(batchStore.tasks[1].status).toBe('completed');
    expect(batchStore.tasks[0].status).toBe('waiting');
    expect(batchStore.currentRunningId).toBeNull();
    expect(batchStore.isQueueRunning).toBe(false); // Does not advance to task 1
  });

  it('filters tasks by search text and status tab', () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([
      { videoPath: '/path/to/movie_action.mp4', importRootPath: 'Action/' },
      { videoPath: '/path/to/drama_ep01.mov', importRootPath: 'Drama/' },
      { videoPath: '/path/to/comedy_ep01.mp4', importRootPath: 'Comedy/' },
    ]);

    batchStore.tasks[0].status = 'completed';
    batchStore.tasks[1].status = 'failed';

    // Status filter: completed
    batchStore.statusFilter = 'completed';
    expect(batchStore.filteredTasks.length).toBe(1);
    expect(batchStore.filteredTasks[0].name).toBe('movie_action.mp4');

    // Status filter: all + search 'drama'
    batchStore.statusFilter = 'all';
    batchStore.searchText = 'drama';
    expect(batchStore.filteredTasks.length).toBe(1);
    expect(batchStore.filteredTasks[0].name).toBe('drama_ep01.mov');

    // Search by directory 'comedy'
    batchStore.searchText = 'comedy';
    expect(batchStore.filteredTasks.length).toBe(1);
    expect(batchStore.filteredTasks[0].name).toBe('comedy_ep01.mp4');
  });

  it('heals selectedTaskId when the selected task is removed', async () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([
      { videoPath: '/path/to/v1.mp4' },
      { videoPath: '/path/to/v2.mp4' },
    ]);

    batchStore.selectTask(batchStore.tasks[0].id);
    expect(batchStore.selectedTaskId).toBe(batchStore.tasks[0].id);

    await batchStore.removeTask(batchStore.tasks[0].id);
    expect(batchStore.tasks.length).toBe(1);
    expect(batchStore.selectedTaskId).toBe(batchStore.tasks[0].id); // Healed to next task
  });

  it('re-queues failed tasks and completely clears residual error and entries', () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([{ videoPath: '/path/to/v1.mp4' }]);
    const task = batchStore.tasks[0];

    task.status = 'failed';
    task.error = 'OCR failed';
    task.progressPct = 50;
    task.entries = [{ index: 1, start_ms: 0, end_ms: 1000, text: 'hi', confidence: 0.95 }];

    batchStore.retryTask(task.id);
    expect(task.status).toBe('waiting');
    expect(task.error).toBeNull();
    expect(task.progressPct).toBe(0);
    expect(task.entries.length).toBe(0);
  });

  it('startQueue processes multiple waiting tasks sequentially with SSE events', async () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([
      { videoPath: '/path/to/seq1.mp4' },
      { videoPath: '/path/to/seq2.mp4' },
    ]);

    const sseCallbacks: Record<string, SseJobCallbacks> = {};
    let jobCounter = 0;
    SubLiftApiClient.createJob = async () => {
      jobCounter++;
      const jid = `job-seq-${jobCounter}`;
      return { job_id: jid, status: 'running' };
    };
    SubLiftApiClient.subscribeJobEvents = (id, callbacks) => {
      sseCallbacks[id] = callbacks;
      return () => {
        delete sseCallbacks[id];
      };
    };

    batchStore.startQueue();
    await new Promise((r) => setTimeout(r, 10));

    expect(batchStore.isQueueRunning).toBe(true);
    expect(batchStore.currentRunningId).toBe(batchStore.tasks[0].id);
    expect(batchStore.tasks[0].status).toBe('extracting');

    // Progress event
    sseCallbacks['job-seq-1']?.onProgress?.({
      job_id: 'job-seq-1',
      stage: 'extracting',
      pct: 0.5,
      eta_ms: 5000,
    });
    expect(batchStore.tasks[0].progressPct).toBe(50);

    // Push entry event
    sseCallbacks['job-seq-1']?.onPushEntry?.({
      job_id: 'job-seq-1',
      entry: { index: 1, start_ms: 100, end_ms: 500, text: 'Hello', confidence: 0.99 },
    });
    expect(batchStore.tasks[0].entries.length).toBe(1);

    // Done event for task 1
    sseCallbacks['job-seq-1']?.onDone?.({
      job_id: 'job-seq-1',
      status: 'completed',
      total_entries: 1,
      elapsed_ms: 1200,
      video_path: '/path/to/seq1.mp4',
    });
    await new Promise((r) => setTimeout(r, 15));

    expect(batchStore.tasks[0].status).toBe('completed');
    expect(batchStore.tasks[0].progressPct).toBe(100);

    // Queue advances to task 2
    expect(batchStore.currentRunningId).toBe(batchStore.tasks[1].id);
    expect(batchStore.tasks[1].status).toBe('extracting');

    // Done event for task 2
    sseCallbacks['job-seq-2']?.onDone?.({
      job_id: 'job-seq-2',
      status: 'completed',
      total_entries: 0,
      elapsed_ms: 800,
      video_path: '/path/to/seq2.mp4',
    });
    await new Promise((r) => setTimeout(r, 15));

    expect(batchStore.tasks[1].status).toBe('completed');
    expect(batchStore.currentRunningId).toBeNull();
    expect(batchStore.isQueueRunning).toBe(false);
  });

  it('handles SSE pipeline error and transitions task to failed with error message while continuing queue', async () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([
      { videoPath: '/path/to/err1.mp4' },
      { videoPath: '/path/to/ok2.mp4' },
    ]);

    const sseCallbacks: Record<string, SseJobCallbacks> = {};
    let jobCounter = 0;
    SubLiftApiClient.createJob = async () => {
      jobCounter++;
      return { job_id: `job-err-${jobCounter}`, status: 'running' };
    };
    SubLiftApiClient.subscribeJobEvents = (id, callbacks) => {
      sseCallbacks[id] = callbacks;
      return () => {
        delete sseCallbacks[id];
      };
    };

    batchStore.startQueue();
    await new Promise((r) => setTimeout(r, 10));

    expect(batchStore.currentRunningId).toBe(batchStore.tasks[0].id);

    // Trigger error on task 1
    sseCallbacks['job-err-1']?.onError?.('Decoder corrupted frame');
    await new Promise((r) => setTimeout(r, 15));

    expect(batchStore.tasks[0].status).toBe('failed');
    expect(batchStore.tasks[0].error).toBe('Decoder corrupted frame');

    // Queue advances to task 2
    expect(batchStore.currentRunningId).toBe(batchStore.tasks[1].id);
    expect(batchStore.tasks[1].status).toBe('extracting');

    sseCallbacks['job-err-2']?.onDone?.({
      job_id: 'job-err-2',
      status: 'completed',
      total_entries: 0,
      elapsed_ms: 500,
      video_path: '/path/to/ok2.mp4',
    });
    await new Promise((r) => setTimeout(r, 15));

    expect(batchStore.tasks[1].status).toBe('completed');
    expect(batchStore.isQueueRunning).toBe(false);
  });

  it('handles cancelTask while running, aborting backend job and moving to next task', async () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([
      { videoPath: '/path/to/cancel1.mp4' },
      { videoPath: '/path/to/next2.mp4' },
    ]);

    let cancelledJobId = '';
    SubLiftApiClient.createJob = async () => ({ job_id: 'job-cancel-test', status: 'running' });
    SubLiftApiClient.cancelJob = async (id: string) => {
      cancelledJobId = id;
    };
    SubLiftApiClient.subscribeJobEvents = () => () => {};

    batchStore.startQueue();
    await new Promise((r) => setTimeout(r, 10));

    expect(batchStore.currentRunningId).toBe(batchStore.tasks[0].id);

    await batchStore.cancelTask(batchStore.tasks[0].id);
    expect(batchStore.tasks[0].status).toBe('cancelled');
    expect(cancelledJobId).toBe('job-cancel-test');

    await new Promise((r) => setTimeout(r, 15));
    // Advanced to task 2
    expect(batchStore.currentRunningId).toBe(batchStore.tasks[1].id);
  });

  it('clearCompleted removes completed, cancelled, interrupted and skipped tasks while keeping waiting and failed tasks', () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([
      { videoPath: '/path/to/w.mp4' },
      { videoPath: '/path/to/c.mp4' },
      { videoPath: '/path/to/f.mp4' },
      { videoPath: '/path/to/cn.mp4' },
      { videoPath: '/path/to/s.mp4' },
      { videoPath: '/path/to/int.mp4' },
    ]);

    batchStore.tasks[1].status = 'completed';
    batchStore.tasks[2].status = 'failed';
    batchStore.tasks[3].status = 'cancelled';
    batchStore.tasks[4].status = 'skipped';
    batchStore.tasks[5].status = 'interrupted';

    expect(batchStore.canClearCompleted).toBe(true);

    batchStore.clearCompleted();
    expect(batchStore.tasks.length).toBe(2);
    expect(batchStore.tasks[0].videoPath).toBe('/path/to/w.mp4');
    expect(batchStore.tasks[1].videoPath).toBe('/path/to/f.mp4');
  });

  it('setScanSummary manages summary state with accepted, skipped, and rejected counts', () => {
    const batchStore = useBatchStore();
    expect(batchStore.lastScanSummary).toBeNull();

    batchStore.setScanSummary({
      accepted: [{ videoPath: '/path/to/v.mp4', name: 'v.mp4' }],
      skipped: 3,
      rejected: [{ pathOrName: 'bad.txt', reason: { kind: 'unsupportedFormat', extension: 'txt' } }],
    });

    expect(batchStore.lastScanSummary).not.toBeNull();
    expect(batchStore.lastScanSummary?.accepted.length).toBe(1);
    expect(batchStore.lastScanSummary?.skipped).toBe(3);
    expect(batchStore.lastScanSummary?.rejected.length).toBe(1);

    batchStore.setScanSummary(null);
    expect(batchStore.lastScanSummary).toBeNull();
  });

  it('preserves outputExists and planningError from addBatchItems in inspectorModel', () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([
      {
        videoPath: '/path/to/movie.mp4',
        outputPath: '/custom/path/movie.srt',
        outputExists: true,
        planningError: 'Warning: Companion subtitle already exists on disk',
      },
    ]);

    expect(batchStore.tasks[0].outputExists).toBe(true);
    expect(batchStore.tasks[0].outputPath).toBe('/custom/path/movie.srt');
    expect(batchStore.tasks[0].planningError).toBe('Warning: Companion subtitle already exists on disk');

    const model = batchStore.inspectorModel;
    expect(model).not.toBeNull();
    expect(model?.outputFileExists).toBe(true);
    expect(model?.outputExistsWarning).toContain('该字幕已存在');
    expect(model?.planningError).toContain('Companion subtitle already exists');
    expect(model?.canEditConfiguration).toBe(true);
  });

  it('clearAll resets all tasks and queue state', () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([{ videoPath: '/path/to/v1.mp4' }, { videoPath: '/path/to/v2.mp4' }]);
    batchStore.isQueueRunning = true;
    batchStore.currentRunningId = 'task-1';

    batchStore.clearAll();
    expect(batchStore.tasks.length).toBe(0);
    expect(batchStore.isQueueRunning).toBe(false);
    expect(batchStore.currentRunningId).toBeNull();
    expect(batchStore.selectedTaskId).toBeNull();
  });

  it('deduplicates incoming subtitle entries on replayed SSE push_entry events', async () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([{ videoPath: '/path/to/v1.mp4' }]);

    const sse: { cb: SseJobCallbacks | null } = { cb: null };
    SubLiftApiClient.createJob = async () => ({ job_id: 'job-dedupe-1', status: 'running' });
    SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
      sse.cb = callbacks;
      return () => {
        sse.cb = null;
      };
    };

    const runPromise = batchStore.startSingle(batchStore.tasks[0].id);
    await new Promise((r) => setTimeout(r, 10));

    // First push entry 1
    sse.cb?.onPushEntry?.({
      job_id: 'job-dedupe-1',
      entry: { index: 1, start_ms: 1000, end_ms: 2500, text: 'Hello', confidence: 0.95 },
    });

    expect(batchStore.tasks[0].entries.length).toBe(1);

    // Replay / duplicate push entry 1 with same index
    sse.cb?.onPushEntry?.({
      job_id: 'job-dedupe-1',
      entry: { index: 1, start_ms: 1000, end_ms: 2500, text: 'Hello', confidence: 0.95 },
    });

    expect(batchStore.tasks[0].entries.length).toBe(1);

    // Push entry 2
    sse.cb?.onPushEntry?.({
      job_id: 'job-dedupe-1',
      entry: { index: 2, start_ms: 2600, end_ms: 4000, text: 'World', confidence: 0.98 },
    });

    expect(batchStore.tasks[0].entries.length).toBe(2);

    sse.cb?.onDone?.({
      job_id: 'job-dedupe-1',
      status: 'completed',
      total_entries: 2,
      elapsed_ms: 1000,
    });
    await runPromise;
  });

  it('handles interrupted tasks properly in stats, retry, and clearCompleted', () => {
    const batchStore = useBatchStore();
    batchStore.addBatchItems([
      { videoPath: '/path/to/v1.mp4' },
      { videoPath: '/path/to/v2.mp4' },
      { videoPath: '/path/to/v3.mp4' },
    ]);

    batchStore.tasks[0].status = 'interrupted';
    batchStore.tasks[1].status = 'waiting';
    batchStore.tasks[2].status = 'completed';

    // Interrupted tasks count under cancelled in stats
    expect(batchStore.stats.cancelled).toBe(1);
    expect(batchStore.stats.waiting).toBe(1);
    expect(batchStore.stats.completed).toBe(1);

    // Retry interrupted task
    batchStore.retryTask(batchStore.tasks[0].id);
    expect(batchStore.tasks[0].status).toBe('waiting');
    expect(batchStore.stats.waiting).toBe(2);

    // Set task 0 back to interrupted
    batchStore.tasks[0].status = 'interrupted';
    batchStore.clearCompleted();

    // clearCompleted removes completed and interrupted tasks
    expect(batchStore.tasks.length).toBe(1);
    expect(batchStore.tasks[0].videoPath).toBe('/path/to/v2.mp4');
    expect(batchStore.tasks[0].status).toBe('waiting');
  });

  describe('Feature 12513: Batch ROI Policy & Execution Consistency', () => {
    it('executes ROI policy "auto", triggering per-video detection in preparing and passing suggested box', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/path/to/auto_video.mp4',
          config: { roi_policy: 'auto', engine: 'mock', quality: 'balanced' },
        },
      ]);

      let detectedPath = '';
      let detectedEngine = '';
      SubLiftApiClient.detectSubtitleRegion = async (path, _ts, engine) => {
        detectedPath = path;
        detectedEngine = engine || '';
        return {
          detected: true,
          sample_time_s: 1.5,
          suggested_box: { x: 0.05, y: 0.78, width: 0.9, height: 0.18 },
          preview_text: 'Auto Detected Subtitle',
          confidence: 0.95,
          total_candidates: 3,
        };
      };

      let passedJobConfig: any = null;
      const sse: { cb: SseJobCallbacks | null } = { cb: null };
      SubLiftApiClient.createJob = async (cfg) => {
        passedJobConfig = cfg;
        return { job_id: 'job-auto-roi-1', status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        sse.cb = callbacks;
        return () => {};
      };

      const task = batchStore.tasks[0];
      const runPromise = batchStore.startSingle(task.id);
      await new Promise((r) => setTimeout(r, 15));

      expect(detectedPath).toBe('/path/to/auto_video.mp4');
      expect(detectedEngine).toBe('mock');
      expect(passedJobConfig.fps).toBe(8.0); // balanced -> 8.0 FPS
      expect(passedJobConfig.region_box).toEqual({ x: 0.05, y: 0.78, width: 0.9, height: 0.18 });

      sse.cb?.onDone?.({
        job_id: 'job-auto-roi-1',
        status: 'completed',
        total_entries: 1,
        elapsed_ms: 1200,
      });
      await runPromise;

      expect(task.status).toBe('completed');
      expect(task.result?.roiSource).toContain('Auto Detected Subtitle');
      expect(task.result?.effectiveRegion).toEqual({ x: 0.05, y: 0.78, width: 0.9, height: 0.18 });
    });

    it('executes ROI policy "fixed", passing user-configured region_box directly without detect-region call', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/path/to/fixed_video.mp4',
          config: {
            roi_policy: 'fixed',
            region_box: { x: 0.12, y: 0.82, width: 0.76, height: 0.14 },
            engine: 'mock',
            quality: 'fine',
          },
        },
      ]);

      let detectCalled = false;
      SubLiftApiClient.detectSubtitleRegion = async () => {
        detectCalled = true;
        return { detected: false } as any;
      };

      let passedJobConfig: any = null;
      SubLiftApiClient.createJob = async (cfg) => {
        passedJobConfig = cfg;
        return { job_id: 'job-fixed-roi-1', status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        callbacks.onDone?.({ job_id: 'job-fixed-roi-1', total_entries: 0, elapsed_ms: 500 });
        return () => {};
      };

      await batchStore.startSingle(batchStore.tasks[0].id);
      expect(detectCalled).toBe(false);
      expect(passedJobConfig.fps).toBe(12.0); // fine -> 12.0 FPS
      expect(passedJobConfig.region_box).toEqual({ x: 0.12, y: 0.82, width: 0.76, height: 0.14 });
      expect(batchStore.tasks[0].result?.roiSource).toBe('用户固定选区');
    });

    it('executes ROI policy "default", explicitly using bottom 30% area without detect-region call', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/path/to/default_video.mp4',
          config: {
            roi_policy: 'default',
            engine: 'mock',
            quality: 'fast',
          },
        },
      ]);

      let detectCalled = false;
      SubLiftApiClient.detectSubtitleRegion = async () => {
        detectCalled = true;
        return { detected: false } as any;
      };

      let passedJobConfig: any = null;
      SubLiftApiClient.createJob = async (cfg) => {
        passedJobConfig = cfg;
        return { job_id: 'job-default-roi-1', status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        callbacks.onDone?.({ job_id: 'job-default-roi-1', total_entries: 0, elapsed_ms: 300 });
        return () => {};
      };

      await batchStore.startSingle(batchStore.tasks[0].id);
      expect(detectCalled).toBe(false);
      expect(passedJobConfig.fps).toBe(5.0); // fast -> 5.0 FPS
      expect(passedJobConfig.region_box).toEqual({ x: 0.0, y: 0.7, width: 1.0, height: 0.3 });
      expect(batchStore.tasks[0].result?.roiSource).toBe('默认底边 30%');
    });

    it('freezes task configuration into an immutable snapshot upon entering preparing', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/path/to/freeze.mp4',
          config: { engine: 'mock', quality: 'fast', confidence_threshold: 0.25, roi_policy: 'fixed', region_box: { x: 0.1, y: 0.7, width: 0.8, height: 0.25 } },
        },
      ]);

      const task = batchStore.tasks[0];
      const sse: { cb: SseJobCallbacks | null } = { cb: null };
      SubLiftApiClient.createJob = async () => ({ job_id: 'job-freeze-1', status: 'running' });
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        sse.cb = callbacks;
        return () => {};
      };

      const startPromise = batchStore.startSingle(task.id);
      await new Promise((r) => setTimeout(r, 10));

      expect(task.status).toBe('extracting');
      expect(Object.isFrozen(task.config)).toBe(true);

      // Attempting mutation during execution fails or throws in strict mode
      expect(() => {
        (task.config as any).quality = 'fine';
      }).toThrow();

      expect(batchStore.updateTaskConfig(task.id, { quality: 'fine' })).toBe(false);
      expect(task.config.quality).toBe('fast');

      sse.cb?.onDone?.({ job_id: 'job-freeze-1', total_entries: 0, elapsed_ms: 200 });
      await startPromise;
      expect(task.config.quality).toBe('fast');
    });

    it('fails closed with explicit error diagnostics when requested engine is unavailable (no silent fallback)', async () => {
      const { useSystemStore } = await import('./system');
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
          videoPath: '/path/to/fail_engine.mp4',
          config: { engine: 'paddle', quality: 'fast' },
        },
      ]);

      const task = batchStore.tasks[0];
      let createJobCalled = false;
      SubLiftApiClient.createJob = async () => {
        createJobCalled = true;
        return { job_id: 'job-should-not-run', status: 'running' };
      };

      await batchStore.startSingle(task.id);

      expect(createJobCalled).toBe(false); // NO silent fallback
      expect(task.status).toBe('failed');
      expect(task.error).toContain('OCR 引擎不可用: paddle (严格禁止静默回退)');

      // Inspector displays fail-closed diagnostic
      batchStore.selectTask(task.id);
      const inspector = batchStore.inspectorModel;
      expect(inspector?.isEngineAvailable).toBe(false);
      expect(inspector?.engineUnavailableReason).toContain('严格禁止静默回退');
      expect(inspector?.failureMessage).toContain('严格禁止静默回退');
    });
  });

  describe('Feature 12514: 真实输出计划与原子批量导出 (Milestone 4)', () => {
    beforeEach(() => {
      // Mock global document & URL for ZIP/download
      const mockCreateObjectURL = vi.fn().mockReturnValue('blob:mock-url');
      const mockRevokeObjectURL = vi.fn();
      (globalThis as any).URL = {
        createObjectURL: mockCreateObjectURL,
        revokeObjectURL: mockRevokeObjectURL,
      };
      (globalThis as any).document = {
        createElement: vi.fn(() => ({
          href: '',
          download: '',
          click: vi.fn(),
        })),
        body: {
          appendChild: vi.fn((n) => n),
          removeChild: vi.fn((n) => n),
        },
      };
    });

    it('packages all completed subtitle tasks into a single ZIP archive without sequential setTimeout hacks', () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        { videoPath: '/workspace/video1.mp4' },
        { videoPath: '/workspace/video2.mp4' },
      ]);

      const t1 = batchStore.tasks[0];
      const t2 = batchStore.tasks[1];

      t1.status = 'completed';
      t1.entries = [{ index: 1, start_ms: 0, end_ms: 1000, text: 'Sub 1', confidence: 1.0 }];

      t2.status = 'completed';
      t2.entries = [{ index: 1, start_ms: 500, end_ms: 1500, text: 'Sub 2', confidence: 1.0 }];

      const exported = batchStore.downloadBatchZip('test_bundle.zip');
      expect(exported).toBe(true);
      expect((globalThis as any).URL.createObjectURL).toHaveBeenCalledTimes(1);
    });

    it('saves single task to disk via server atomic endpoint and updates confirmed disk state', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([{ videoPath: '/workspace/movie.mp4' }]);
      const task = batchStore.tasks[0];
      task.status = 'completed';
      task.jobId = 'job-save-single-1';
      task.entries = [{ index: 1, start_ms: 100, end_ms: 200, text: 'Hello', confidence: 1.0 }];
      task.result = {
        entryCount: 1,
        outputPath: '/workspace/movie.srt',
        diskStatus: 'unwritten',
        emptyResult: false,
      };

      SubLiftApiClient.saveJobToDisk = async (_jobId, opts) => {
        expect(opts?.conflict_policy).toBe('deterministic_rename');
        return {
          job_id: 'job-save-single-1',
          status: 'saved',
          target_path: '/workspace/movie.srt',
          saved_path: '/workspace/movie_1.srt',
          empty_result: false,
          entry_count: 1,
        };
      };

      const resp = await batchStore.saveTaskToDisk(task.id, 'deterministic_rename');
      expect(resp?.status).toBe('saved');
      expect(task.diskStatus).toBe('saved');
      expect(task.savedPath).toBe('/workspace/movie_1.srt');
      expect(task.result?.savedPath).toBe('/workspace/movie_1.srt');

      // Inspector inspection
      batchStore.selectTask(task.id);
      const inspector = batchStore.inspectorModel;
      expect(inspector?.diskStatus).toBe('saved');
      expect(inspector?.diskStatusDisplay).toBe('已落盘');
      expect(inspector?.savedFullPath).toBe('/workspace/movie_1.srt');
    });

    it('performs batch save to disk with conflict policies and reflects skipped/saved statuses', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        { videoPath: '/workspace/ep1.mp4' },
        { videoPath: '/workspace/ep2.mp4' },
      ]);

      const t1 = batchStore.tasks[0];
      const t2 = batchStore.tasks[1];

      t1.status = 'completed';
      t1.jobId = 'job-ep1';
      t1.entries = [{ index: 1, start_ms: 0, end_ms: 100, text: 'Ep 1', confidence: 1.0 }];

      t2.status = 'completed';
      t2.jobId = 'job-ep2';
      t2.entries = [{ index: 1, start_ms: 0, end_ms: 100, text: 'Ep 2', confidence: 1.0 }];

      SubLiftApiClient.batchSaveToDisk = async (req) => {
        expect(req?.conflict_policy).toBe('skip');
        return {
          total: 2,
          saved: 1,
          skipped: 1,
          empty_results: 0,
          failed: 0,
          results: [
            {
              job_id: 'job-ep1',
              status: 'saved',
              target_path: '/workspace/ep1.srt',
              saved_path: '/workspace/ep1.srt',
              empty_result: false,
              entry_count: 1,
            },
            {
              job_id: 'job-ep2',
              status: 'skipped',
              target_path: '/workspace/ep2.srt',
              saved_path: '',
              empty_result: false,
              entry_count: 1,
              error: 'File already exists',
            },
          ],
        };
      };

      const res = await batchStore.batchSaveToDisk('skip');
      expect(res?.saved).toBe(1);
      expect(res?.skipped).toBe(1);

      expect(t1.diskStatus).toBe('saved');
      expect(t1.savedPath).toBe('/workspace/ep1.srt');

      expect(t2.diskStatus).toBe('skipped');
      expect(t2.savedPath).toBeUndefined();

      batchStore.selectTask(t2.id);
      const inspector2 = batchStore.inspectorModel;
      expect(inspector2?.diskStatus).toBe('skipped');
      expect(inspector2?.diskStatusDisplay).toBe('跳过落盘 (已存在同名文件)');
    });

    it('marks empty subtitle jobs as emptyResult: true and avoids empty disk writes', () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([{ videoPath: '/workspace/nosubs.mp4' }]);
      const task = batchStore.tasks[0];
      task.status = 'completed';
      task.entries = []; // 0 entries
      task.emptyResult = true;
      task.diskStatus = 'empty_result';

      batchStore.selectTask(task.id);
      const inspector = batchStore.inspectorModel;
      expect(inspector?.emptyResult).toBe(true);
      expect(inspector?.diskStatus).toBe('empty_result');
      expect(inspector?.diskStatusDisplay).toBe('空字幕 (未落盘)');
    });

    it('clearly distinguishes planned output path from confirmed written output file on disk', () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([{ videoPath: '/workspace/sample.mp4' }]);
      const task = batchStore.tasks[0];

      // Before execution
      batchStore.selectTask(task.id);
      let inspector = batchStore.inspectorModel;
      expect(inspector?.outputFullPath).toBe('/workspace/sample.srt');
      expect(inspector?.savedFullPath).toBeUndefined();
      expect(inspector?.diskStatus).toBe('unwritten');
      expect(inspector?.diskStatusDisplay).toBe('未落盘 (建议路径)');
      expect(inspector?.canSaveToDisk).toBe(false);

      // Completed but unwritten
      task.status = 'completed';
      task.jobId = 'job-sample-1';
      task.entries = [{ index: 1, start_ms: 10, end_ms: 20, text: 'Hi', confidence: 1.0 }];
      task.diskStatus = 'unwritten';

      inspector = batchStore.inspectorModel;
      expect(inspector?.outputFullPath).toBe('/workspace/sample.srt');
      expect(inspector?.savedFullPath).toBeUndefined();
      expect(inspector?.canSaveToDisk).toBe(true);

      // Written to disk
      task.diskStatus = 'saved';
      task.savedPath = '/workspace/sample_1.srt';

      inspector = batchStore.inspectorModel;
      expect(inspector?.outputFullPath).toBe('/workspace/sample.srt');
      expect(inspector?.savedFullPath).toBe('/workspace/sample_1.srt');
      expect(inspector?.diskStatus).toBe('saved');
      expect(inspector?.diskStatusDisplay).toBe('已落盘');
    });
  });
});

