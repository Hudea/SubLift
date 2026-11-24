import type {
  CapabilitiesDto,
  TaskStatus,
  VideoInfoDto,
  WorkbenchEngine,
} from './types';

export interface CommandAvailability {
  enabled: boolean;
  reason?: string;
}

export interface WorkbenchCommandMatrix {
  addVideos: CommandAvailability;
  detectAutoRegion: CommandAvailability;
  editRoi: CommandAvailability;
  startExtraction: CommandAvailability;
  cancelExtraction: CommandAvailability;
  applyConfigToQueue: CommandAvailability;
  editCue: CommandAvailability;
  splitCue: CommandAvailability;
  mergeCue: CommandAvailability;
  reRecognizeLowConf: CommandAvailability;
  exportActiveSrt: CommandAvailability;
  batchExportSrt: CommandAvailability;
  undo: CommandAvailability;
  redo: CommandAvailability;
}

export interface WorkbenchCommandsInput {
  hasActiveTask: boolean;
  activeStatus: TaskStatus | null;
  activeProbe: VideoInfoDto | null;
  activeEngine: WorkbenchEngine | null;
  capabilities: CapabilitiesDto | null;
  hasValidVlmProvider: boolean;
  isAutoDetecting: boolean;
  isReRecognizing: boolean;
  isDirtyInlineEdit: boolean;
  isInlineTextFocused: boolean;
  activeCueCount: number;
  queueLength: number;
  hasApplyTargets: boolean;
  exportableQueueCount: number;
  canUndo: boolean;
  canRedo: boolean;
}

function allow(): CommandAvailability {
  return { enabled: true };
}

function deny(reason: string): CommandAvailability {
  return { enabled: false, reason };
}

/**
 * 唯一命令可用性派生矩阵：工具栏、右键菜单、快捷键必须共享本选择器。
 */
export function selectWorkbenchCommands(
  input: WorkbenchCommandsInput
): WorkbenchCommandMatrix {
  const {
    hasActiveTask,
    activeStatus,
    activeProbe,
    activeEngine,
    capabilities,
    hasValidVlmProvider,
    isAutoDetecting,
    isReRecognizing,
    isDirtyInlineEdit,
    isInlineTextFocused,
    activeCueCount,
    queueLength,
    hasApplyTargets,
    exportableQueueCount,
    canUndo,
    canRedo,
  } = input;

  const isExtracting = activeStatus === 'extracting';
  const isProofreadReady = activeStatus === 'ready' || activeStatus === 'exported';
  const displayTransformOk = activeProbe?.display_transform_ok === true;
  const unsupportedAppleVision =
    activeEngine === 'apple_vision' &&
    capabilities !== null &&
    !capabilities.engines.includes('apple_vision');

  // 1. addVideos
  const addVideos = allow();

  // 2. detectAutoRegion
  let detectAutoRegion: CommandAvailability;
  if (!hasActiveTask || !activeProbe) {
    detectAutoRegion = deny('请先添加并探测视频');
  } else if (!displayTransformOk) {
    detectAutoRegion = deny(
      activeProbe.transform_note || '视频显示变换异常，已禁用自动探测'
    );
  } else if (isExtracting) {
    detectAutoRegion = deny('正在提取中，无法修改选区');
  } else if (isAutoDetecting) {
    detectAutoRegion = deny('正在执行自动探测…');
  } else {
    detectAutoRegion = allow();
  }

  // 3. editRoi
  let editRoi: CommandAvailability;
  if (!hasActiveTask || !activeProbe) {
    editRoi = deny('请先添加并探测视频');
  } else if (!displayTransformOk) {
    editRoi = deny(
      activeProbe.transform_note || '视频显示变换异常，已禁用选区编辑'
    );
  } else if (isExtracting) {
    editRoi = deny('提取进行中，选区已锁定');
  } else {
    editRoi = allow();
  }

  // 4. startExtraction
  let startExtraction: CommandAvailability;
  if (!hasActiveTask || !activeProbe || activeStatus === null) {
    startExtraction = deny('请先添加视频');
  } else if (!displayTransformOk) {
    startExtraction = deny(
      activeProbe.transform_note || '视频显示变换异常，无法启动提取'
    );
  } else if (isExtracting) {
    startExtraction = deny('当前正在提取中');
  } else if (isAutoDetecting) {
    startExtraction = deny('正在执行自动探测，请稍候');
  } else if (isReRecognizing) {
    startExtraction = deny('VLM 局部重识进行中，请稍候');
  } else if (unsupportedAppleVision) {
    startExtraction = deny(
      '当前环境不支持 Apple Vision，请切换为 PaddleOCR 或 VLM 后再启动提取'
    );
  } else if (activeEngine === 'vlm' && !hasValidVlmProvider) {
    startExtraction = deny('请先配置 VLM 服务');
  } else {
    startExtraction = allow();
  }

  // 5. cancelExtraction
  const cancelExtraction = isExtracting
    ? allow()
    : deny('当前没有正在提取的任务');

  // 6. applyConfigToQueue
  let applyConfigToQueue: CommandAvailability;
  if (queueLength <= 1) {
    applyConfigToQueue = deny('队列仅单集，无需批量套用');
  } else if (!hasActiveTask || !activeProbe || !displayTransformOk) {
    applyConfigToQueue = deny('源视频尚未就绪或显示变换异常');
  } else if (!hasApplyTargets) {
    applyConfigToQueue = deny('队列中无可用目标视频');
  } else {
    applyConfigToQueue = allow();
  }

  // 7. editCue / splitCue / mergeCue
  let editCue: CommandAvailability;
  if (!isProofreadReady) {
    editCue = deny('提取完成前字幕列表为只读预览');
  } else if (isReRecognizing) {
    editCue = deny('VLM 局部重识进行中');
  } else {
    editCue = allow();
  }
  const splitCue = editCue;
  const mergeCue = editCue;

  // 8. reRecognizeLowConf
  let reRecognizeLowConf: CommandAvailability;
  if (!isProofreadReady) {
    reRecognizeLowConf = deny('请先完成字幕提取');
  } else if (isDirtyInlineEdit) {
    reRecognizeLowConf = deny('请先提交或取消当前行内编辑');
  } else if (isReRecognizing) {
    reRecognizeLowConf = deny('正在执行 VLM 局部重识…');
  } else if (!hasValidVlmProvider) {
    reRecognizeLowConf = deny('请先配置 VLM 服务');
  } else {
    reRecognizeLowConf = allow();
  }

  // 9. exportActiveSrt
  let exportActiveSrt: CommandAvailability;
  if (!isProofreadReady) {
    exportActiveSrt = deny('请先完成字幕提取');
  } else if (isDirtyInlineEdit) {
    exportActiveSrt = deny('存在未提交的行内编辑，请先按 Enter 提交或 Esc 放弃');
  } else if (activeCueCount === 0) {
    exportActiveSrt = deny('暂无可导出字幕');
  } else {
    exportActiveSrt = allow();
  }

  // 10. batchExportSrt
  let batchExportSrt: CommandAvailability;
  if (isDirtyInlineEdit) {
    batchExportSrt = deny('存在未提交的行内编辑');
  } else if (exportableQueueCount < 1) {
    batchExportSrt = deny('尚无已完成提取的剧集');
  } else {
    batchExportSrt = allow();
  }

  // 11. undo / redo
  let undo: CommandAvailability;
  let redo: CommandAvailability;
  if (isInlineTextFocused || isDirtyInlineEdit) {
    undo = deny('请先完成当前单元格编辑');
    redo = deny('请先完成当前单元格编辑');
  } else if (isExtracting) {
    undo = deny('正在提取中');
    redo = deny('正在提取中');
  } else if (isReRecognizing) {
    undo = deny('VLM 局部重识进行中');
    redo = deny('VLM 局部重识进行中');
  } else {
    undo = canUndo ? allow() : deny('暂无可撤销操作');
    redo = canRedo ? allow() : deny('暂无可重做操作');
  }

  return {
    addVideos,
    detectAutoRegion,
    editRoi,
    startExtraction,
    cancelExtraction,
    applyConfigToQueue,
    editCue,
    splitCue,
    mergeCue,
    reRecognizeLowConf,
    exportActiveSrt,
    batchExportSrt,
    undo,
    redo,
  };
}
