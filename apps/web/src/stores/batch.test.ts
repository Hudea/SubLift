import { describe, it, expect, beforeEach } from 'vitest';
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
});
