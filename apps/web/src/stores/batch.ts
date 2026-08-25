import { defineStore } from 'pinia';
import { ref, computed } from 'vue';
import { SubLiftApiClient } from '../api/client';
import { useSystemStore } from './system';
import { exportSrtFile, exportBatchZip } from '../utils/srt_formatter';
import type { NormalizedRegionBox, JobConflictPolicy } from '../types/api';
import {
  type BatchTaskItem,
  type BatchQueueStats,
  type BatchTaskConfig,
  type BatchTaskStatus,
  type BatchTaskStatusFilter,
  type BatchScanSummary,
  type TaskInspectorModel,
  type RoiPolicy,
  SAMPLING_QUALITY_MAP,
  DEFAULT_BOTTOM_ROI,
  DEFAULT_CONFIDENCE_THRESHOLD,
  qualityToFps,
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
    confidence_threshold: DEFAULT_CONFIDENCE_THRESHOLD,
    roi_policy: 'auto',
    region_box: null,
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
  const canClearCompleted = computed(
    () => stats.value.completed > 0 || stats.value.cancelled > 0 || stats.value.skipped > 0
  );
  const canExportAll = computed(() => stats.value.completed > 0);
  const canBatchSave = computed(() => tasks.value.some((t) => t.status === 'completed' && !!t.jobId));

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

    const roiPolicyMap: Record<RoiPolicy, string> = {
      auto: '智能识别 (逐视频独立分析)',
      fixed: '固定选区',
      default: '默认区域 (底边 30%)',
    };

    const outPath = task.outputPath || `${task.videoPath.replace(/\.[^/.]+$/, '')}.srt`;
    const outParts = outPath.replace(/\\/g, '/').split('/');
    const outFilename = outParts.pop() || '';
    const outFolder = outParts.pop() || '';

    const isEngineAvailable =
      task.config.engine === 'mock' ||
      (!systemStore.systemInfo ||
        systemStore.systemInfo.engines.some((e) => e.name === task.config.engine && e.available));

    let roiSourceDisplay = '默认底边 30%';
    if (task.result?.roiSource) {
      roiSourceDisplay = task.result.roiSource;
    } else if (task.config.roi_policy === 'auto') {
      roiSourceDisplay = '准备阶段独立检测';
    } else if (task.config.roi_policy === 'fixed') {
      roiSourceDisplay = '用户固定选区';
    }

    const diskStatus = task.diskStatus || task.result?.diskStatus || 'unwritten';
    const diskStatusMap: Record<string, string> = {
      unwritten: '未落盘 (建议路径)',
      saved: '已落盘',
      skipped: '跳过落盘 (已存在同名文件)',
      empty_result: '空字幕 (未落盘)',
      failed: '落盘失败',
    };
    const emptyResult = task.emptyResult ?? (task.status === 'completed' && task.entries.length === 0);
    const savedFullPath = task.savedPath || task.result?.savedPath;

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
      savedFullPath,
      diskStatus,
      diskStatusDisplay: diskStatusMap[diskStatus] || diskStatus,
      emptyResult,
      outputFileExists: !!task.outputExists,
      planningError: task.planningError,
      outputExistsWarning: task.outputExists ? '该字幕已存在，导出时将按策略处理' : undefined,
      engine: task.config.engine,
      quality: task.config.quality,
      confidence_threshold: task.config.confidence_threshold ?? 0.0,
      roi_policy: task.config.roi_policy ?? 'auto',
      roi_source_display: roiSourceDisplay,
      region_box: task.result?.effectiveRegion ?? task.config.region_box ?? null,
      script: task.config.script,
      engineDisplay: engineMap[task.config.engine] || task.config.engine,
      qualityDisplay: `${SAMPLING_QUALITY_MAP[task.config.quality]?.label || task.config.quality} (${qualityToFps(task.config.quality)} FPS)`,
      roiPolicyDisplay: roiPolicyMap[task.config.roi_policy || 'auto'] || task.config.roi_policy,
      canEditConfiguration: BatchTaskStatusGuard.canEditConfig(task.status),
      isEngineAvailable,
      engineUnavailableReason: !isEngineAvailable
        ? `当前环境未就绪 ${engineMap[task.config.engine] || task.config.engine}，严格禁止静默回退`
        : undefined,
      failureMessage: task.error || undefined,
      runtimeIdentity: task.result?.runtimeIdentity,
      entryCount: task.entries.length,
      canCancel: BatchTaskStatusGuard.canCancel(task.status),
      canRetry: BatchTaskStatusGuard.canRetry(task.status),
      canRemove: BatchTaskStatusGuard.canRemove(task.status),
      canReorder: BatchTaskStatusGuard.canReorder(task.status),
      canStartSingle: BatchTaskStatusGuard.canStartSingle(task.status),
      canSaveToDisk: task.status === 'completed' && !!task.jobId,
    };
  });

  // 5. Actions: Ingestion & Scan
  function addBatchItems(
    items: Array<{
      videoPath: string;
      name?: string;
      importRootPath?: string;
      outputPath?: string;
      outputExists?: boolean;
      planningError?: string;
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
        outputPath: item.outputPath || `${cleanPath.replace(/\.[^/.]+$/, '')}.srt`,
        outputExists: item.outputExists,
        planningError: item.planningError,
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

    // 1. 冻结不可变配置快照：任务开始后配置与策略完全锁定，杜绝隐式漂移
    const frozenConfig: Readonly<BatchTaskConfig> = Object.freeze({
      engine: task.config.engine,
      quality: task.config.quality,
      confidence_threshold: task.config.confidence_threshold ?? DEFAULT_CONFIDENCE_THRESHOLD,
      roi_policy: task.config.roi_policy ?? 'auto',
      region_box: task.config.region_box ? Object.freeze({ ...task.config.region_box }) : null,
      script: task.config.script,
    });
    task.config = frozenConfig as BatchTaskConfig;

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

    // 2. 严格 Fail-Closed 引擎可用性校验（杜绝静默回退/切换）
    const isEngineAvailable =
      frozenConfig.engine === 'mock' ||
      (!systemStore.systemInfo ||
        systemStore.systemInfo.engines.some((e) => e.name === frozenConfig.engine && e.available));

    if (!isEngineAvailable) {
      task.status = 'failed';
      task.stage = 'failed';
      task.error = `OCR 引擎不可用: ${frozenConfig.engine} (严格禁止静默回退)`;
      if (singleRunTaskId.value === task.id) {
        singleRunTaskId.value = null;
        isQueueRunning.value = false;
      }
      currentRunningId.value = null;
      isDispatching = false;
      if (autoAdvance && isQueueRunning.value && singleRunTaskId.value === null) {
        queueMicrotask(() => processNext());
      }
      return;
    }

    try {
      // 3. 逐任务显式 ROI 策略执行
      let targetRegionBox: NormalizedRegionBox | null = null;
      let roiSource = '默认底边 30%';

      if (frozenConfig.roi_policy === 'auto') {
        task.stage = 'detecting_region';
        try {
          const detRes = await SubLiftApiClient.detectSubtitleRegion(
            task.videoPath,
            undefined,
            frozenConfig.engine
          );
          if (detRes && detRes.detected && detRes.suggested_box) {
            targetRegionBox = detRes.suggested_box;
            roiSource = detRes.preview_text
              ? `自动检测：“${detRes.preview_text}”`
              : '自动检测推荐选区';
          } else {
            targetRegionBox = { ...DEFAULT_BOTTOM_ROI };
            roiSource = '自动检测未命中字幕，回退默认底边 30%';
          }
        } catch {
          targetRegionBox = { ...DEFAULT_BOTTOM_ROI };
          roiSource = '智能检测异常，已使用默认底边 30%';
        }
      } else if (frozenConfig.roi_policy === 'fixed') {
        targetRegionBox = frozenConfig.region_box
          ? { ...frozenConfig.region_box }
          : { ...DEFAULT_BOTTOM_ROI };
        roiSource = '用户固定选区';
      } else {
        // default 策略：明确使用底层流水线标准 30% 底部区域
        targetRegionBox = { ...DEFAULT_BOTTOM_ROI };
        roiSource = '默认底边 30%';
      }

      if ((task.status as string) === 'cancelled') {
        currentRunningId.value = null;
        cleanupSse();
        shouldAdvanceImmediately = autoAdvance && singleRunTaskId.value === null;
        return;
      }

      const targetFps = qualityToFps(frozenConfig.quality);
      const resp = await SubLiftApiClient.createJob({
        video_path: task.videoPath,
        engine: frozenConfig.engine,
        fps: targetFps,
        confidence_threshold: frozenConfig.confidence_threshold,
        region_box: targetRegionBox,
        script: frozenConfig.script,
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
          if (task.status === 'extracting' && data && data.entry) {
            const entry = data.entry;
            const exists = task.entries.some(
              (e) =>
                e.index === entry.index ||
                (e.start_ms === entry.start_ms && e.end_ms === entry.end_ms && e.text === entry.text)
            );
            if (!exists) {
              task.entries.push(entry);
            }
          }
        },
        onDone: (data) => {
          if (task.status === 'extracting' || task.status === 'preparing' || task.status === 'exporting') {
            // FSM 级联快进推进至 completed，防止跨阶段转移拒绝
            task.status = 'completed';
            task.stage = 'completed';
            task.progressPct = 100;
            task.elapsedMs = data.elapsed_ms;
            const entryCount = data.total_entries;
            const emptyResult = entryCount === 0 || task.entries.length === 0;
            task.emptyResult = emptyResult;
            task.diskStatus = 'unwritten';
            task.result = {
              entryCount,
              outputPath: task.outputPath || `${task.videoPath.replace(/\.[^/.]+$/, '')}.srt`,
              runtimeIdentity: `${frozenConfig.engine.toUpperCase()} Engine`,
              roiSource,
              effectiveRegion: targetRegionBox,
              diskStatus: 'unwritten',
              emptyResult,
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
      (t) =>
        t.status !== 'completed' &&
        t.status !== 'cancelled' &&
        t.status !== 'interrupted' &&
        t.status !== 'skipped'
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

  async function saveTaskToDisk(
    id: string,
    policy: JobConflictPolicy = 'deterministic_rename',
    allowEmpty: boolean = false
  ) {
    const task = tasks.value.find((t) => t.id === id);
    if (!task || !task.jobId || task.status !== 'completed') return null;

    try {
      const resp = await SubLiftApiClient.saveJobToDisk(task.jobId, {
        target_path: task.outputPath,
        conflict_policy: policy,
        allow_empty: allowEmpty,
      });

      task.diskStatus = resp.status;
      task.savedPath = resp.saved_path || undefined;
      task.emptyResult = resp.empty_result;

      if (task.result) {
        task.result.diskStatus = resp.status;
        task.result.savedPath = resp.saved_path || undefined;
        task.result.emptyResult = resp.empty_result;
        task.result.savedAt = Date.now();
      }
      return resp;
    } catch (err: unknown) {
      task.diskStatus = 'failed';
      if (task.result) {
        task.result.diskStatus = 'failed';
      }
      throw err;
    }
  }

  /**
   * 将全部已完成任务打包为单一 ZIP 归档一次性下载（杜绝 setTimeout 连续多个下载 hack）
   */
  function downloadBatchZip(zipFilename: string = 'subtitles_batch.zip'): boolean {
    const completed = tasks.value.filter((t) => t.status === 'completed' && t.entries.length > 0);
    if (completed.length === 0) return false;

    const items = completed.map((task) => ({
      name: task.name,
      entries: task.entries,
    }));
    return exportBatchZip(items, zipFilename);
  }

  /**
   * 服务端批量原子落盘保存
   */
  async function batchSaveToDisk(
    policy: JobConflictPolicy = 'deterministic_rename',
    allowEmpty: boolean = false
  ) {
    const completedTasks = tasks.value.filter((t) => t.status === 'completed' && t.jobId);
    if (completedTasks.length === 0) return null;

    const jobIds = completedTasks.map((t) => t.jobId as string);
    const resp = await SubLiftApiClient.batchSaveToDisk({
      job_ids: jobIds,
      conflict_policy: policy,
      allow_empty: allowEmpty,
    });

    for (const res of resp.results) {
      const targetTask = tasks.value.find((t) => t.jobId === res.job_id);
      if (targetTask) {
        targetTask.diskStatus = res.status;
        targetTask.savedPath = res.saved_path || undefined;
        targetTask.emptyResult = res.empty_result;
        if (targetTask.result) {
          targetTask.result.diskStatus = res.status;
          targetTask.result.savedPath = res.saved_path || undefined;
          targetTask.result.emptyResult = res.empty_result;
          targetTask.result.savedAt = Date.now();
        }
      }
    }
    return resp;
  }

  function exportAllCompleted(zipFilename: string = 'subtitles_batch.zip') {
    return downloadBatchZip(zipFilename);
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
    canBatchSave,
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
    saveTaskToDisk,
    downloadBatchZip,
    batchSaveToDisk,
    exportAllCompleted,
    setScanSummary,
  };
});
