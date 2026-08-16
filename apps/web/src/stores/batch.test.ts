import { describe, it, expect, beforeEach } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import { useBatchStore } from './batch';
import { SubLiftApiClient } from '../api/client';
import type { SseJobCallbacks } from '../types/api';

describe('useBatchStore - Batch Queue & Concurrency (Features 12301 & 12503)', () => {
  beforeEach(() => {
    setActivePinia(createPinia());
  });

  it('initializes with empty tasks and idle state', () => {
    const batchStore = useBatchStore();
    expect(batchStore.tasks.length).toBe(0);
    expect(batchStore.stats.total).toBe(0);
    expect(batchStore.isQueueRunning).toBe(false);
    expect(batchStore.canStartAll).toBe(false);
  });

  it('adds multiple files and deduplicates paths', () => {
    const batchStore = useBatchStore();
    batchStore.addFiles(['/path/to/video1.mp4', '/path/to/video2.mov', '/path/to/video3.webm']);
    expect(batchStore.tasks.length).toBe(3);
    expect(batchStore.stats.waiting).toBe(3);
    expect(batchStore.canStartAll).toBe(true);
    expect(batchStore.tasks[0].name).toBe('video1.mp4');

    // Deduplication
    batchStore.addFiles(['/path/to/video1.mp4']);
    expect(batchStore.tasks.length).toBe(3);
  });

  it('cancels and retries waiting tasks', async () => {
    const batchStore = useBatchStore();
    batchStore.addFiles(['/path/to/v1.mp4', '/path/to/v2.mp4']);
    const task2Id = batchStore.tasks[1].id;

    await batchStore.cancelTask(task2Id);
    expect(batchStore.tasks[1].status).toBe('cancelled');
    expect(batchStore.stats.waiting).toBe(1);
    expect(batchStore.stats.cancelled).toBe(1);

    batchStore.retryTask(task2Id);
    expect(batchStore.tasks[1].status).toBe('waiting');
    expect(batchStore.stats.waiting).toBe(2);
    expect(batchStore.stats.cancelled).toBe(0);
  });

  it('executes serial queue with single concurrency invariant and auto-advancement', async () => {
    const batchStore = useBatchStore();
    batchStore.addFiles(['/path/to/video1.mp4', '/path/to/video2.mov', '/path/to/video3.webm']);

    let activeSseCallback: SseJobCallbacks | null = null;
    const createdJobIds: string[] = [];

    SubLiftApiClient.createJob = async () => {
      const jobId = `mock-job-${createdJobIds.length + 1}`;
      createdJobIds.push(jobId);
      return { job_id: jobId, status: 'running' };
    };

    SubLiftApiClient.subscribeJobEvents = (_jobId, callbacks) => {
      activeSseCallback = callbacks;
      return () => {
        activeSseCallback = null;
      };
    };

    // Start Queue
    batchStore.startQueue();
    await new Promise((r) => setTimeout(r, 10));

    expect(batchStore.isQueueRunning).toBe(true);
    expect(batchStore.currentRunningId).toBe(batchStore.tasks[0].id);
    expect(batchStore.tasks[0].status).toBe('running');
    expect(batchStore.tasks[1].status).toBe('waiting');
    expect(batchStore.tasks[2].status).toBe('waiting');
    expect(batchStore.stats.running).toBe(1);

    // Double startQueue ignored
    batchStore.startQueue();
    await new Promise((r) => setTimeout(r, 10));
    expect(batchStore.stats.running).toBe(1);

    // Progress update
    activeSseCallback?.onProgress?.({ stage: 'ocr', pct: 0.65, eta_ms: 1200 });
    expect(batchStore.tasks[0].progressPct).toBe(65);
    expect(batchStore.tasks[0].stage).toBe('ocr');

    // Complete Task 1 -> Task 2 auto-starts
    activeSseCallback?.onDone?.({
      job_id: createdJobIds[0],
      status: 'completed',
      total_entries: 5,
      elapsed_ms: 3200,
      video_path: '/path/to/video1.mp4',
    });
    await new Promise((r) => setTimeout(r, 10));

    expect(batchStore.tasks[0].status).toBe('completed');
    expect(batchStore.tasks[0].progressPct).toBe(100);
    expect(batchStore.tasks[1].status).toBe('running');
    expect(batchStore.currentRunningId).toBe(batchStore.tasks[1].id);

    // Error on Task 2 -> Task 3 auto-starts (Fail-closed isolation)
    activeSseCallback?.onError?.('Corrupted video header');
    await new Promise((r) => setTimeout(r, 10));

    expect(batchStore.tasks[1].status).toBe('failed');
    expect(batchStore.tasks[1].error).toBe('Corrupted video header');
    expect(batchStore.tasks[2].status).toBe('running');

    // Cancel in-flight Task 3
    let cancelledJobId: string | null = null;
    SubLiftApiClient.cancelJob = async (jobId) => {
      cancelledJobId = jobId;
    };

    await batchStore.cancelTask(batchStore.tasks[2].id);
    await new Promise((r) => setTimeout(r, 10));

    expect(batchStore.tasks[2].status).toBe('cancelled');
    expect(cancelledJobId).toBe(createdJobIds[2]);
    expect(batchStore.currentRunningId).toBeNull();
    expect(batchStore.isQueueRunning).toBe(false);
  });

  it('eliminates deadlock and auto-advances when createJob throws immediately (Feature 12503)', async () => {
    const batchStore = useBatchStore();
    batchStore.addFiles(['/path/to/bad_video.mp4', '/path/to/good_video.mp4']);
    expect(batchStore.tasks.length).toBe(2);

    let callCount = 0;
    SubLiftApiClient.createJob = async () => {
      callCount++;
      if (callCount === 1) {
        throw new Error('400 Bad Request: Invalid media file');
      }
      return { job_id: 'good-job-id', status: 'running' };
    };

    batchStore.startQueue();
    await new Promise((r) => setTimeout(r, 20));

    expect(batchStore.tasks[0].status).toBe('failed');
    expect(batchStore.tasks[0].error).toBe('400 Bad Request: Invalid media file');
    expect(batchStore.tasks[1].status).toBe('running');
    expect(batchStore.currentRunningId).toBe(batchStore.tasks[1].id);
  });
});
