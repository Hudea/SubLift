import { defineStore } from 'pinia';
import { ref, computed } from 'vue';
import { SubLiftApiClient } from '../api/client';
import { exportSrtFile } from '../utils/srt_formatter';
import type {
  BatchTaskItem,
  BatchQueueStats,
  BatchTaskConfig,
} from '../types/batch';
import type { OcrEngineName, NormalizedRegionBox } from '../types/api';

export const useBatchStore = defineStore('batch', () => {
  // 1. State
  const tasks = ref<BatchTaskItem[]>([]);
  const isQueueRunning = ref<boolean>(false);
  const currentRunningId = ref<string | null>(null);
  let sseUnsubscribe: (() => void) | null = null;
  let isDispatching = false;

  // Default config for new tasks in batch
  const defaultBatchConfig = ref<BatchTaskConfig>({
    engine: 'paddle' as OcrEngineName,
    fps: 2.0,
    confidence_threshold: 0.0,
    region_box: {
      x: 0.0,
      y: 0.7,
      width: 1.0,
      height: 0.3,
    } as NormalizedRegionBox,
  });

  // 2. Computed statistics
  const stats = computed<BatchQueueStats>(() => {
    const res: BatchQueueStats = {
      total: tasks.value.length,
      waiting: 0,
      running: 0,
      completed: 0,
      failed: 0,
      cancelled: 0,
    };
    for (const t of tasks.value) {
      res[t.status]++;
    }
    return res;
  });

  const activeTaskCount = computed(() => stats.value.waiting + stats.value.running);
  const canStartAll = computed(() => stats.value.waiting > 0 && !isQueueRunning.value);
  const canPause = computed(() => isQueueRunning.value);
  const canClearCompleted = computed(() => stats.value.completed > 0 || stats.value.cancelled > 0);
  const canExportAll = computed(() => stats.value.completed > 0);

  // 3. Actions
  function addFiles(filePaths: string[], config?: Partial<BatchTaskConfig>) {
    for (const path of filePaths) {
      const cleanPath = path.trim();
      if (!cleanPath) continue;

      // 避免同路径重复添加未完成的任务
      const exists = tasks.value.some(
        (t) => t.videoPath === cleanPath && (t.status === 'waiting' || t.status === 'running')
      );
      if (exists) continue;

      const name = cleanPath.split(/[/\\]/).pop() || 'video.mp4';
      const id = `batch-${Date.now()}-${Math.random().toString(36).substring(2, 7)}`;

      const mergedConfig: BatchTaskConfig = {
        ...defaultBatchConfig.value,
        ...config,
      };

      tasks.value.push({
        id,
        videoPath: cleanPath,
        name,
        status: 'waiting',
        progressPct: 0,
        stage: 'waiting',
        jobId: null,
        entries: [],
        error: null,
        elapsedMs: 0,
        config: mergedConfig,
        createdAt: Date.now(),
      });
    }

    if (isQueueRunning.value && !currentRunningId.value) {
      processNext();
    }
  }

  function startQueue() {
    if (isQueueRunning.value) return;
    isQueueRunning.value = true;
    if (!currentRunningId.value) {
      processNext();
    }
  }

  function pauseQueue() {
    isQueueRunning.value = false;
  }

  async function processNext() {
    if (isDispatching || !isQueueRunning.value) return;

    // 严格确保当前没有其他正在活跃执行的任务
    if (currentRunningId.value !== null) {
      const active = tasks.value.find((t) => t.id === currentRunningId.value);
      if (active && active.status === 'running') {
        return;
      }
    }

    // 寻找下一个等待执行的任务
    const nextTask = tasks.value.find((t) => t.status === 'waiting');
    if (!nextTask) {
      if (tasks.value.length > 0 && !tasks.value.some((t) => t.status === 'running')) {
        isQueueRunning.value = false;
      }
      currentRunningId.value = null;
      return;
    }

    isDispatching = true;
    currentRunningId.value = nextTask.id;
    nextTask.status = 'running';
    nextTask.stage = 'starting';
    nextTask.progressPct = 0;
    nextTask.entries = [];
    nextTask.error = null;

    let shouldAdvanceImmediately = false;

    try {
      const resp = await SubLiftApiClient.createJob({
        video_path: nextTask.videoPath,
        engine: nextTask.config.engine,
        fps: nextTask.config.fps,
        confidence_threshold: nextTask.config.confidence_threshold,
        region_box: nextTask.config.region_box,
      });

      // 防御：若在网络创建期间用户取消了该任务
      if ((nextTask.status as string) === 'cancelled') {
        await SubLiftApiClient.cancelJob(resp.job_id).catch(() => {});
        currentRunningId.value = null;
        cleanupSse();
        shouldAdvanceImmediately = true;
        return;
      }

      nextTask.jobId = resp.job_id;

      // 订阅 SSE 实时进度
      sseUnsubscribe = SubLiftApiClient.subscribeJobEvents(resp.job_id, {
        onProgress: (data) => {
          if (nextTask.status === 'running') {
            nextTask.stage = data.stage;
            nextTask.progressPct = Math.round(data.pct * 100);
          }
        },
        onPushEntry: (data) => {
          if (nextTask.status === 'running') {
            nextTask.entries.push(data.entry);
          }
        },
        onDone: (data) => {
          if (nextTask.status === 'running') {
            nextTask.status = 'completed';
            nextTask.stage = 'completed';
            nextTask.progressPct = 100;
            nextTask.elapsedMs = data.elapsed_ms;
          }
          currentRunningId.value = null;
          cleanupSse();
          queueMicrotask(() => processNext());
        },
        onError: (err) => {
          if (nextTask.status === 'running') {
            nextTask.status = 'failed';
            nextTask.stage = 'failed';
            nextTask.error = String(err);
          }
          currentRunningId.value = null;
          cleanupSse();
          queueMicrotask(() => processNext());
        },
      });
    } catch (err: unknown) {
      nextTask.status = 'failed';
      nextTask.stage = 'failed';
      nextTask.error = err instanceof Error ? err.message : String(err);
      currentRunningId.value = null;
      cleanupSse();
      shouldAdvanceImmediately = true;
    } finally {
      isDispatching = false;
      if (shouldAdvanceImmediately) {
        queueMicrotask(() => processNext());
      }
    }
  }

  function cleanupSse() {
    if (sseUnsubscribe) {
      sseUnsubscribe();
      sseUnsubscribe = null;
    }
  }

  async function cancelTask(id: string) {
    const task = tasks.value.find((t) => t.id === id);
    if (!task) return;

    if (task.status === 'running') {
      task.status = 'cancelled';
      task.stage = 'cancelled';
      const jobId = task.jobId;
      currentRunningId.value = null;
      cleanupSse();

      if (jobId) {
        try {
          await SubLiftApiClient.cancelJob(jobId);
        } catch {
          // Ignore cancellation network error
        }
      }
      queueMicrotask(() => processNext());
    } else if (task.status === 'waiting') {
      task.status = 'cancelled';
      task.stage = 'cancelled';
    }
  }

  function retryTask(id: string) {
    const task = tasks.value.find((t) => t.id === id);
    if (!task) return;

    task.status = 'waiting';
    task.stage = 'waiting';
    task.progressPct = 0;
    task.entries = [];
    task.error = null;
    task.jobId = null;

    if (isQueueRunning.value && !currentRunningId.value) {
      processNext();
    }
  }

  async function removeTask(id: string) {
    const idx = tasks.value.findIndex((t) => t.id === id);
    if (idx >= 0) {
      const task = tasks.value[idx];
      if (task.status === 'running') {
        await cancelTask(id);
      }
      const newIdx = tasks.value.findIndex((t) => t.id === id);
      if (newIdx >= 0) {
        tasks.value.splice(newIdx, 1);
      }
    }
  }

  function clearCompleted() {
    tasks.value = tasks.value.filter(
      (t) => t.status !== 'completed' && t.status !== 'cancelled'
    );
  }

  function clearAll() {
    tasks.value = [];
    isQueueRunning.value = false;
    currentRunningId.value = null;
    cleanupSse();
  }

  function exportTaskSrt(id: string) {
    const task = tasks.value.find((t) => t.id === id);
    if (!task || task.status !== 'completed' || task.entries.length === 0) {
      return;
    }
    const cleanName = task.name.replace(/\.[^/.]+$/, '');
    exportSrtFile(task.entries, cleanName);
  }

  function exportAllCompleted() {
    const completed = tasks.value.filter(
      (t) => t.status === 'completed' && t.entries.length > 0
    );
    completed.forEach((task, idx) => {
      window.setTimeout(() => {
        const cleanName = task.name.replace(/\.[^/.]+$/, '');
        exportSrtFile(task.entries, cleanName);
      }, idx * 150);
    });
  }

  return {
    tasks,
    isQueueRunning,
    currentRunningId,
    defaultBatchConfig,
    stats,
    activeTaskCount,
    canStartAll,
    canPause,
    canClearCompleted,
    canExportAll,
    addFiles,
    startQueue,
    pauseQueue,
    cancelTask,
    retryTask,
    removeTask,
    clearCompleted,
    clearAll,
    exportTaskSrt,
    exportAllCompleted,
  };
});
