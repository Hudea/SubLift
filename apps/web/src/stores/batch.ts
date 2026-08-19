import { defineStore } from 'pinia';
import { ref, computed } from 'vue';
import { SubLiftApiClient } from '../api/client';
import { useSystemStore } from './system';
import { exportSrtFile } from '../utils/srt_formatter';
import {
  type BatchTaskItem,
  type BatchQueueStats,
  type BatchTaskConfig,
  type BatchTaskStatus,
  type BatchTaskStatusFilter,
  type BatchScanSummary,
  type TaskInspectorModel,
  SAMPLING_QUALITY_MAP,
} from '../types/batch';
import { BatchTaskStatusGuard } from '../utils/batch_guards';

export const useBatchStore = defineStore('batch', () => {
  const systemStore = useSystemStore();

  // 1. Core State
  const tasks = ref<BatchTaskItem[]>([]);
  const isQueueRunning = ref<boolean>(false);
  const currentRunningId = ref<string | null>(null);
  const selectedTaskId = ref<string | null>(null);
  const singleRunTaskId = ref<string | null>(null);

  // 2. View Projections & Filter State
  const searchText = ref<string>('');
  const statusFilter = ref<BatchTaskStatusFilter>('all');
  const lastScanSummary = ref<BatchScanSummary | null>(null);

  let sseUnsubscribe: (() => void) | null = null;
  let isDispatching = false;

  // 3. Default Configuration (Dynamically matched with platform preferred engine)
  const defaultBatchConfig = computed<BatchTaskConfig>(() => ({
    engine: systemStore.preferredEngine,
    quality: 'fast', // 默认 5.0 FPS（快速），符合 SubLift GT 锚点
    confidence_threshold: 0.0,
  }));

  // 4. Statistics & Projection
  const stats = computed<BatchQueueStats>(() => {
    const res: BatchQueueStats = {
      total: tasks.value.length,
      waiting: 0,
      active: 0,
      completed: 0,
      failed: 0,
      cancelled: 0,
      skipped: 0,
    };
    for (const t of tasks.value) {
      if (t.status === 'waiting') res.waiting++;
      else if (BatchTaskStatusGuard.isActive(t.status)) res.active++;
      else if (t.status === 'completed') res.completed++;
      else if (t.status === 'failed') res.failed++;
      else if (t.status === 'cancelled' || t.status === 'interrupted') res.cancelled++;
      else if (t.status === 'skipped') res.skipped++;
    }
    return res;
  });

  const activeTaskCount = computed(() => stats.value.waiting + stats.value.active);
  const canStartAll = computed(() => stats.value.waiting > 0 && !isQueueRunning.value);
  const canPause = computed(() => isQueueRunning.value);
  const canClearCompleted = computed(() => stats.value.completed > 0 || stats.value.cancelled > 0);
  const canExportAll = computed(() => stats.value.completed > 0);

  // 4.1 Filtered Tasks Projection (Search + Status Filter)
  const filteredTasks = computed<BatchTaskItem[]>(() => {
    const q = searchText.value.trim().toLowerCase();
    return tasks.value.filter((task) => {
      // 状态筛选
      let statusMatches = true;
      if (statusFilter.value === 'waiting') statusMatches = task.status === 'waiting';
      else if (statusFilter.value === 'running') statusMatches = BatchTaskStatusGuard.isActive(task.status);
      else if (statusFilter.value === 'completed') statusMatches = task.status === 'completed';
      else if (statusFilter.value === 'failed') statusMatches = task.status === 'failed';
      else if (statusFilter.value === 'cancelled') statusMatches = task.status === 'cancelled' || task.status === 'interrupted';
      else if (statusFilter.value === 'skipped') statusMatches = task.status === 'skipped';

      if (!statusMatches) return false;
      if (!q) return true;

      // 搜索匹配：文件名、位置显示串、完整绝对路径
      const location = getLocationDisplay(task).toLowerCase();
      return (
        task.name.toLowerCase().includes(q) ||
        task.videoPath.toLowerCase().includes(q) ||
        location.includes(q)
      );
    });
  });

  // 4.2 Location Display Formatter
  function getLocationDisplay(task: BatchTaskItem): string {
    if (task.importRootPath) {
      return task.importRootPath;
    }
    const cleanPath = task.videoPath.replace(/\\/g, '/');
    const parts = cleanPath.split('/').filter((p) => p.length > 0);
    if (parts.length > 1) {
      return `${parts[parts.length - 2]}/`;
    }
    return '根目录';
  }

  // 4.3 Active Inspector Model
  const selectedTask = computed(() => tasks.value.find((t) => t.id === selectedTaskId.value) || null);

  const inspectorModel = computed<TaskInspectorModel | null>(() => {
    const task = selectedTask.value;
    if (!task) return null;

    const engineMap: Record<string, string> = {
      vision: 'Apple Vision',
      paddle: 'PaddleOCR',
      mock: 'Mock Engine',
    };
    const statusMap: Record<BatchTaskStatus, string> = {
      waiting: '等待中',
      preparing: '准备中',
      extracting: '提取中',
      exporting: '导出中',
      completed: '已完成',
      failed: '失败',
      cancelled: '已取消',
      interrupted: '已中断',
      skipped: '已跳过',
    };

    const outPath = task.outputPath || `${task.videoPath.replace(/\.[^/.]+$/, '')}.srt`;
    const outParts = outPath.replace(/\\/g, '/').split('/');
    const outFilename = outParts.pop() || '';
    const outFolder = outParts.pop() || '';

    return {
      id: task.id,
      filename: task.name,
      statusName: statusMap[task.status] || task.status,
      status: task.status,
      locationDisplay: getLocationDisplay(task),
      locationFullPath: task.videoPath,
      outputFolderDisplay: outFolder,
      outputFilename: outFilename,
      outputFullPath: outPath,
      outputFileExists: !!task.outputExists,
      planningError: task.planningError,
      outputExistsWarning: task.outputExists ? '该字幕已存在，导出时将原子覆盖' : undefined,
      engine: task.config.engine,
      quality: task.config.quality,
      engineDisplay: engineMap[task.config.engine] || task.config.engine,
      qualityDisplay: SAMPLING_QUALITY_MAP[task.config.quality]?.label || task.config.quality,
      canEditConfiguration: BatchTaskStatusGuard.canEditConfig(task.status),
      failureMessage: task.error || undefined,
      runtimeIdentity: task.result?.runtimeIdentity,
      entryCount: task.entries.length,
      canCancel: BatchTaskStatusGuard.canCancel(task.status),
      canRetry: BatchTaskStatusGuard.canRetry(task.status),
      canRemove: BatchTaskStatusGuard.canRemove(task.status),
      canReorder: BatchTaskStatusGuard.canReorder(task.status),
      canStartSingle: BatchTaskStatusGuard.canStartSingle(task.status),
    };
  });

  // 5. Actions: Ingestion & Scan
  function addBatchItems(
    items: Array<{
      videoPath: string;
      name?: string;
      importRootPath?: string;
      config?: Partial<BatchTaskConfig>;
    }>
  ) {
    for (const item of items) {
      const cleanPath = item.videoPath.trim();
      if (!cleanPath) continue;

      const exists = tasks.value.some(
        (t) =>
          t.videoPath === cleanPath &&
          t.status !== 'completed' &&
          t.status !== 'failed' &&
          t.status !== 'cancelled'
      );
      if (exists) continue;

      const name = item.name || cleanPath.split(/[/\\]/).pop() || 'video.mp4';
      const id = `task-${Date.now()}-${Math.random().toString(36).substring(2, 7)}`;

      const mergedConfig: BatchTaskConfig = {
        ...defaultBatchConfig.value,
        ...item.config,
      };

      tasks.value.push({
        id,
        videoPath: cleanPath,
        name,
        importRootPath: item.importRootPath,
        outputPath: `${cleanPath.replace(/\.[^/.]+$/, '')}.srt`,
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

    if (!selectedTaskId.value && tasks.value.length > 0) {
      selectedTaskId.value = tasks.value[0].id;
    }
  }

  // 6. Actions: Single Task Controls
  function selectTask(id: string | null) {
    selectedTaskId.value = id;
  }

  function updateTaskConfig(id: string, configUpdate: Partial<BatchTaskConfig>) {
    const task = tasks.value.find((t) => t.id === id);
    if (!task || !BatchTaskStatusGuard.canEditConfig(task.status)) return false;

    task.config = {
      ...task.config,
      ...configUpdate,
    };
    return true;
  }

  // 槽位保留重排序：严格限定仅在 waiting 任务之间进行槽位交换
  function moveTaskUp(id: string) {
    const task = tasks.value.find((t) => t.id === id);
    if (!task || !BatchTaskStatusGuard.canReorder(task.status)) return;

    const waitingIndices = tasks.value
      .map((t, idx) => (t.status === 'waiting' ? idx : -1))
      .filter((idx) => idx !== -1);

    const currentIndex = tasks.value.findIndex((t) => t.id === id);
    const waitingPos = waitingIndices.indexOf(currentIndex);
    if (waitingPos <= 0) return;

    const targetIndex = waitingIndices[waitingPos - 1];
    const temp = tasks.value[currentIndex];
    tasks.value[currentIndex] = tasks.value[targetIndex];
    tasks.value[targetIndex] = temp;
  }

  function moveTaskDown(id: string) {
    const task = tasks.value.find((t) => t.id === id);
    if (!task || !BatchTaskStatusGuard.canReorder(task.status)) return;

    const waitingIndices = tasks.value
      .map((t, idx) => (t.status === 'waiting' ? idx : -1))
      .filter((idx) => idx !== -1);

    const currentIndex = tasks.value.findIndex((t) => t.id === id);
    const waitingPos = waitingIndices.indexOf(currentIndex);
    if (waitingPos < 0 || waitingPos >= waitingIndices.length - 1) return;

    const targetIndex = waitingIndices[waitingPos + 1];
    const temp = tasks.value[currentIndex];
    tasks.value[currentIndex] = tasks.value[targetIndex];
    tasks.value[targetIndex] = temp;
  }

  // 7. Actions: Execution & Queue Machine
  async function startSingle(id: string) {
    if (isQueueRunning.value || currentRunningId.value !== null) return false;
    const task = tasks.value.find((t) => t.id === id);
    if (!task || !BatchTaskStatusGuard.canStartSingle(task.status)) return false;

    singleRunTaskId.value = id;
    await executeSingleTask(task, false);
    return true;
  }

  function startQueue() {
    if (isQueueRunning.value) return;
    singleRunTaskId.value = null;
    isQueueRunning.value = true;
    if (!currentRunningId.value) {
      processNext();
    }
  }

  function pauseQueue() {
    isQueueRunning.value = false;
    singleRunTaskId.value = null;
  }

  async function processNext() {
    if (isDispatching || !isQueueRunning.value || singleRunTaskId.value !== null) return;

    if (currentRunningId.value !== null) {
      const active = tasks.value.find((t) => t.id === currentRunningId.value);
      if (active && BatchTaskStatusGuard.isActive(active.status)) {
        return;
      }
    }

    const nextTask = tasks.value.find((t) => t.status === 'waiting');
    if (!nextTask) {
      if (!tasks.value.some((t) => t.status === 'waiting' || BatchTaskStatusGuard.isActive(t.status))) {
        isQueueRunning.value = false;
      }
      currentRunningId.value = null;
      return;
    }

    await executeSingleTask(nextTask, true);
  }

  async function executeSingleTask(task: BatchTaskItem, autoAdvance: boolean) {
    isDispatching = true;
    currentRunningId.value = task.id;
    task.status = 'preparing';
    task.stage = 'preparing';
    task.progressPct = 0;
    task.entries = [];
    task.error = null;

    let shouldAdvanceImmediately = false;

    const finalizeIfCurrent = () => {
      if (currentRunningId.value !== task.id) return;
      cleanupSse();

      if (singleRunTaskId.value === task.id) {
        singleRunTaskId.value = null;
        isQueueRunning.value = false;
        currentRunningId.value = null;
        return;
      }

      currentRunningId.value = null;
      if (autoAdvance && isQueueRunning.value) {
        queueMicrotask(() => processNext());
      }
    };

    try {
      const targetFps = SAMPLING_QUALITY_MAP[task.config.quality]?.fps || 5.0;
      const resp = await SubLiftApiClient.createJob({
        video_path: task.videoPath,
        engine: task.config.engine,
        fps: targetFps,
        confidence_threshold: task.config.confidence_threshold || 0.0,
        region_box: task.config.region_box,
      });

      if ((task.status as string) === 'cancelled') {
        await SubLiftApiClient.cancelJob(resp.job_id).catch(() => {});
        currentRunningId.value = null;
        cleanupSse();
        shouldAdvanceImmediately = autoAdvance && singleRunTaskId.value === null;
        return;
      }

      task.jobId = resp.job_id;
      task.status = 'extracting';
      task.stage = 'extracting';

      sseUnsubscribe = SubLiftApiClient.subscribeJobEvents(resp.job_id, {
        onProgress: (data) => {
          if (task.status === 'extracting' || task.status === 'preparing') {
            task.stage = data.stage;
            task.progressPct = Math.round(data.pct * 100);
          }
        },
        onPushEntry: (data) => {
          if (task.status === 'extracting') {
            task.entries.push(data.entry);
          }
        },
        onDone: (data) => {
          if (task.status === 'extracting' || task.status === 'preparing' || task.status === 'exporting') {
            // FSM 级联快进推进至 completed，防止跨阶段转移拒绝
            task.status = 'completed';
            task.stage = 'completed';
            task.progressPct = 100;
            task.elapsedMs = data.elapsed_ms;
            task.result = {
              entryCount: data.total_entries,
              outputPath: task.outputPath || `${task.videoPath.replace(/\.[^/.]+$/, '')}.srt`,
              runtimeIdentity: `${task.config.engine.toUpperCase()} Engine`,
            };
          }
          finalizeIfCurrent();
        },
        onError: (err) => {
          if (BatchTaskStatusGuard.isActive(task.status)) {
            task.status = 'failed';
            task.stage = 'failed';
            task.error = String(err);
          }
          finalizeIfCurrent();
        },
        onCancelled: (reason) => {
          if (BatchTaskStatusGuard.isActive(task.status)) {
            task.status = 'cancelled';
            task.stage = 'cancelled';
            task.error = reason || null;
          }
          finalizeIfCurrent();
        },
      });
    } catch (err: unknown) {
      if ((task.status as string) !== 'cancelled') {
        task.status = 'failed';
        task.stage = 'failed';
        task.error = err instanceof Error ? err.message : String(err);
      }
      if (singleRunTaskId.value === task.id) {
        singleRunTaskId.value = null;
        isQueueRunning.value = false;
      }
      currentRunningId.value = null;
      cleanupSse();
      shouldAdvanceImmediately = autoAdvance && singleRunTaskId.value === null;
    } finally {
      isDispatching = false;
      if (shouldAdvanceImmediately && isQueueRunning.value) {
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
    if (!task || !BatchTaskStatusGuard.canCancel(task.status)) return;

    const wasRunning = BatchTaskStatusGuard.isActive(task.status);
    task.status = 'cancelled';
    task.stage = 'cancelled';

    if (wasRunning) {
      const jobId = task.jobId;
      cleanupSse();
      if (singleRunTaskId.value === id) {
        singleRunTaskId.value = null;
        isQueueRunning.value = false;
      }
      currentRunningId.value = null;
      if (jobId) {
        try {
          await SubLiftApiClient.cancelJob(jobId);
        } catch {
          // ignore
        }
      }
      if (isQueueRunning.value) {
        queueMicrotask(() => processNext());
      }
    }
  }

  function retryTask(id: string) {
    const task = tasks.value.find((t) => t.id === id);
    if (!task || !BatchTaskStatusGuard.canRetry(task.status)) return;

    // 深度重置旧错误与残留进度
    task.status = 'waiting';
    task.stage = 'waiting';
    task.progressPct = 0;
    task.elapsedMs = 0;
    task.entries = [];
    task.error = null;
    task.jobId = null;
    task.result = undefined;

    if (isQueueRunning.value && !currentRunningId.value) {
      processNext();
    }
  }

  async function removeTask(id: string) {
    const idx = tasks.value.findIndex((t) => t.id === id);
    if (idx >= 0) {
      const task = tasks.value[idx];
      if (BatchTaskStatusGuard.isActive(task.status)) {
        await cancelTask(id);
      }
      tasks.value.splice(idx, 1);
      // 选中态自愈
      if (selectedTaskId.value === id) {
        selectedTaskId.value = tasks.value[0]?.id || null;
      }
    }
  }

  function clearCompleted() {
    tasks.value = tasks.value.filter(
      (t) => t.status !== 'completed' && t.status !== 'cancelled' && t.status !== 'skipped'
    );
    if (selectedTaskId.value && !tasks.value.some((t) => t.id === selectedTaskId.value)) {
      selectedTaskId.value = tasks.value[0]?.id || null;
    }
  }

  function clearAll() {
    tasks.value = [];
    isQueueRunning.value = false;
    currentRunningId.value = null;
    selectedTaskId.value = null;
    singleRunTaskId.value = null;
    cleanupSse();
  }

  function exportTaskSrt(id: string) {
    const task = tasks.value.find((t) => t.id === id);
    if (!task || task.status !== 'completed' || task.entries.length === 0) return;
    const cleanName = task.name.replace(/\.[^/.]+$/, '');
    exportSrtFile(task.entries, cleanName);
  }

  function exportAllCompleted() {
    const completed = tasks.value.filter((t) => t.status === 'completed' && t.entries.length > 0);
    completed.forEach((task, idx) => {
      window.setTimeout(() => {
        const cleanName = task.name.replace(/\.[^/.]+$/, '');
        exportSrtFile(task.entries, cleanName);
      }, idx * 150);
    });
  }

  function setScanSummary(summary: BatchScanSummary | null) {
    lastScanSummary.value = summary;
  }

  return {
    tasks,
    isQueueRunning,
    currentRunningId,
    selectedTaskId,
    singleRunTaskId,
    searchText,
    statusFilter,
    lastScanSummary,
    defaultBatchConfig,
    stats,
    activeTaskCount,
    filteredTasks,
    selectedTask,
    inspectorModel,
    canStartAll,
    canPause,
    canClearCompleted,
    canExportAll,
    addBatchItems,
    selectTask,
    updateTaskConfig,
    moveTaskUp,
    moveTaskDown,
    startSingle,
    startQueue,
    pauseQueue,
    cancelTask,
    retryTask,
    removeTask,
    clearCompleted,
    clearAll,
    exportTaskSrt,
    exportAllCompleted,
    setScanSummary,
  };
});
