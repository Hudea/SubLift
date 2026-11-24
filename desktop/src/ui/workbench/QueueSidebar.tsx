import { useState } from 'react';
import {
  AlertTriangle,
  CopyCheck,
  Download,
  FolderPlus,
  Play,
  RotateCcw,
  Square,
  X,
} from 'lucide-react';
import {
  countLowConfidenceCues,
  resolveLowConfidenceThreshold,
} from '../../core/subtitle';
import { formatCompactDuration } from '../../core/timecode';
import { selectActiveCommands, useWorkbenchStore } from '../../store';
import type { QueueTaskItem } from '../../store';
import { StatusBadge } from '../components/ui/badge';
import { Button } from '../components/ui/button';
import { cn } from '../lib/utils';
import { ApplyConfigConfirmModal, BatchExportConflictModal } from './BatchModals';

function statusAccentColor(status: QueueTaskItem['status']): string {
  switch (status) {
    case 'extracting':
      return 'bg-[var(--status-extracting)]';
    case 'ready':
      return 'bg-[var(--status-ready)]';
    case 'exported':
      return 'bg-[var(--status-exported)]';
    case 'failed':
      return 'bg-[var(--status-failed)]';
    case 'cancelled':
      return 'bg-[var(--status-cancelled)]';
    case 'idle':
    default:
      return 'bg-[var(--status-idle)]';
  }
}

function engineShortLabel(engine: QueueTaskItem['engine']): string {
  switch (engine) {
    case 'apple_vision':
      return 'Vision';
    case 'vlm':
      return 'VLM';
    case 'paddle':
    default:
      return 'Paddle';
  }
}

function roiModeShortLabel(mode: QueueTaskItem['roiMode']): string {
  switch (mode) {
    case 'auto':
      return '自动';
    case 'rect':
      return '矩形';
    case 'band':
    default:
      return '字幕带';
  }
}

interface QueueSidebarProps {
  className?: string;
}

export function QueueSidebar({ className }: QueueSidebarProps) {
  const tasks = useWorkbenchStore((s) => s.tasks);
  const activeTaskId = useWorkbenchStore((s) => s.activeTaskId);
  const queueRunActive = useWorkbenchStore((s) => s.queueRunActive);
  const commands = useWorkbenchStore(selectActiveCommands);

  const pickAndAddVideos = useWorkbenchStore((s) => s.pickAndAddVideos);
  const selectActiveTask = useWorkbenchStore((s) => s.selectActiveTask);
  const removeTask = useWorkbenchStore((s) => s.removeTask);
  const applyConfigToQueue = useWorkbenchStore((s) => s.applyConfigToQueue);
  const startQueueExtraction = useWorkbenchStore((s) => s.startQueueExtraction);
  const cancelAllExtraction = useWorkbenchStore((s) => s.cancelAllExtraction);
  const cancelTaskExtraction = useWorkbenchStore((s) => s.cancelTaskExtraction);
  const startTaskExtraction = useWorkbenchStore((s) => s.startTaskExtraction);

  const [applyModalOpen, setApplyModalOpen] = useState(false);
  const [batchExportOpen, setBatchExportOpen] = useState(false);

  const completedTargets = tasks.filter(
    (t) =>
      t.id !== activeTaskId &&
      (t.status === 'ready' || t.status === 'exported')
  );
  const pendingTargets = tasks.filter(
    (t) =>
      t.id !== activeTaskId &&
      t.status !== 'extracting' &&
      t.status !== 'ready' &&
      t.status !== 'exported'
  );

  const handleApplyClick = () => {
    if (completedTargets.length > 0) {
      setApplyModalOpen(true);
    } else {
      applyConfigToQueue({ includeCompleted: false });
    }
  };

  return (
    <aside
      aria-label="任务队列"
      className={cn(
        'flex h-full flex-col border-r border-[var(--border-subtle)] bg-[var(--bg-panel)] select-none',
        className
      )}
    >
      {/* 头部标题与添加视频 */}
      <div className="flex items-center justify-between border-b border-[var(--border-subtle)] px-3 py-2">
        <span className="text-[11px] font-semibold tracking-wider text-[var(--text-secondary)]">
          任务队列 ({tasks.length})
        </span>
        <Button
          size="xs"
          variant="secondary"
          onClick={() => void pickAndAddVideos()}
        >
          <FolderPlus className="h-3.5 w-3.5" strokeWidth={1.75} />
          <span>添加视频</span>
        </Button>
      </div>

      {/* 多集批处理工具条 */}
      {tasks.length > 1 && (
        <div className="flex flex-wrap items-center gap-1.5 border-b border-[var(--border-subtle)] px-2.5 py-2">
          <Button
            size="xs"
            variant="outline"
            className="flex-1"
            disabled={!commands.applyConfigToQueue.enabled}
            title={commands.applyConfigToQueue.reason ?? '将当前集选区与参数应用到其余剧集'}
            onClick={handleApplyClick}
          >
            <CopyCheck className="h-3 w-3" strokeWidth={1.75} />
            <span>套用配置到待处理项</span>
          </Button>

          {queueRunActive ? (
            <Button
              size="xs"
              variant="danger"
              onClick={() => void cancelAllExtraction()}
            >
              <Square className="h-3 w-3" strokeWidth={1.75} />
              <span>全部取消</span>
            </Button>
          ) : (
            <Button
              size="xs"
              variant="primary"
              onClick={() => void startQueueExtraction()}
            >
              <Play className="h-3 w-3" strokeWidth={1.75} />
              <span>全部开始</span>
            </Button>
          )}
        </div>
      )}

      {/* 队列卡片列表 */}
      <div className="flex-1 overflow-y-auto p-2 space-y-1.5">
        {tasks.length === 0 ? (
          <div className="flex h-full min-h-[160px] flex-col items-center justify-center rounded-[var(--radius-md)] border border-dashed border-[var(--border-default)] p-4 text-center">
            <p className="text-xs font-medium text-[var(--text-secondary)]">
              拖入或添加视频文件
            </p>
            <p className="mt-1 text-[11px] text-[var(--text-muted)]">
              支持 MP4、MKV、MOV、AVI、WEBM、TS
            </p>
            <Button
              size="xs"
              variant="primary"
              className="mt-3"
              onClick={() => void pickAndAddVideos()}
            >
              选择视频文件
            </Button>
          </div>
        ) : (
          tasks.map((item) => {
            const isActive = item.id === activeTaskId;
            const lowConfThreshold = resolveLowConfidenceThreshold(
              item.activeRunConfig?.engine ?? item.engine
            );
            const lowCount = countLowConfidenceCues(item.cues, lowConfThreshold);
            const effSampleFps =
              (item.activeRunConfig?.sampleFps ?? item.sampleFps) > 0
                ? (item.activeRunConfig?.sampleFps ?? item.sampleFps)
                : 5;
            const isVlmSheets =
              item.subPhase === 'vlm_sheets' &&
              (item.progress?.totalSheets ?? 0) > 0;
            const pct =
              isVlmSheets && item.progress
                ? Math.min(
                    100,
                    Math.round(
                      (item.progress.completedSheets /
                        item.progress.totalSheets) *
                        100
                    )
                  )
                : item.progress && item.progress.totalMs > 0
                  ? Math.min(
                      100,
                      Math.round(
                        (item.progress.currentMs / item.progress.totalMs) * 100
                      )
                    )
                  : 0;
            const itemFps = item.progress?.fps ?? 0;

            return (
              <div
                key={item.id}
                role="button"
                tabIndex={0}
                aria-current={isActive ? 'true' : undefined}
                onClick={() => selectActiveTask(item.id)}
                onKeyDown={(e) => {
                  if (e.key === 'Enter' || e.key === ' ') {
                    e.preventDefault();
                    selectActiveTask(item.id);
                  }
                }}
                className={cn(
                  'group relative flex min-h-[68px] cursor-pointer overflow-hidden rounded-[var(--radius-md)] border transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[var(--border-focus)]',
                  isActive
                    ? 'border-[var(--sl-highlight)]/70 bg-[var(--bg-active)]'
                    : 'border-[var(--border-subtle)] bg-[var(--bg-elevated)] hover:bg-[var(--bg-hover)]'
                )}
              >
                <div className={cn('w-[3px] shrink-0', statusAccentColor(item.status))} />

                <div className="flex min-w-0 flex-1 flex-col justify-between px-2.5 py-2">
                  {/* 第 1 行：文件名 + 状态徽标 */}
                  <div className="flex items-center justify-between gap-1.5">
                    <span
                      className="truncate text-xs font-medium text-[var(--text-primary)]"
                      title={item.videoPath}
                    >
                      {item.fileName}
                    </span>
                    <div className="flex shrink-0 items-center gap-1">
                      <StatusBadge
                        status={item.status}
                        subPhase={item.subPhase}
                        progress={item.progress}
                        engine={item.activeRunConfig?.engine ?? item.engine}
                        isAutoDetecting={item.isAutoDetecting}
                      />
                      {item.status !== 'extracting' && (
                        <button
                          type="button"
                          aria-label={`移除 ${item.fileName}`}
                          onClick={(e) => {
                            e.stopPropagation();
                            removeTask(item.id);
                          }}
                          className="hidden rounded-[var(--radius-xs)] p-0.5 text-[var(--text-muted)] hover:bg-[var(--bg-panel)] hover:text-[var(--text-primary)] group-hover:inline-flex group-focus-within:inline-flex focus:inline-flex"
                        >
                          <X className="h-3 w-3" strokeWidth={1.75} />
                        </button>
                      )}
                    </div>
                  </div>

                  {/* 第 2 行：分辨率 + 时长 + 引擎/ROI */}
                  <div className="mt-1 flex items-center justify-between gap-1 text-[11px] text-[var(--text-secondary)]">
                    <span className="tabular-nums-mono truncate">
                      {item.probe
                        ? `${item.probe.width}×${item.probe.height} · ${formatCompactDuration(item.probe.duration_ms)}`
                        : item.probeLoading
                          ? '探测中…'
                          : '探测失败'}
                    </span>
                    <div className="flex shrink-0 items-center gap-1">
                      {item.roiScaledFromQueue && (
                        <span className="rounded-[var(--radius-xs)] bg-[var(--tint-ready)] px-1 py-0.5 text-[10px] text-[var(--status-ready)]">
                          ROI 已按比例缩放
                        </span>
                      )}
                      <span className="text-[10px] text-[var(--text-muted)]">
                        {engineShortLabel(item.engine)} · {roiModeShortLabel(item.roiMode)}
                      </span>
                    </div>
                  </div>

                  {/* 第 3 行：按状态切换进度 / 条数 / 错误 */}
                  <div className="mt-1.5">
                    {item.status === 'extracting' && (
                      <div className="flex items-center gap-2">
                        <div className="h-[3px] flex-1 overflow-hidden rounded-full bg-[var(--border-default)]">
                          <div
                            className="h-full bg-[var(--status-extracting)] transition-all"
                            style={{ width: `${pct}%` }}
                          />
                        </div>
                        <span className="tabular-nums-mono text-[10px] text-[var(--text-secondary)]">
                          {isVlmSheets && item.progress
                            ? `${item.progress.completedSheets}/${item.progress.totalSheets} 张`
                            : itemFps > 0
                              ? `${pct}% · ${itemFps.toFixed(0)}fps (${(
                                  itemFps / effSampleFps
                                ).toFixed(1)}×)`
                              : `${pct}%`}
                        </span>
                        <button
                          type="button"
                          aria-label={`取消提取 ${item.fileName}`}
                          onClick={(e) => {
                            e.stopPropagation();
                            void cancelTaskExtraction(item.id);
                          }}
                          className="rounded-[var(--radius-xs)] px-1 text-[10px] text-[var(--status-failed)] hover:bg-[var(--tint-failed)]"
                        >
                          取消
                        </button>
                      </div>
                    )}

                    {(item.status === 'ready' || item.status === 'exported') && (
                      <div className="flex items-center justify-between text-[11px]">
                        <span className="tabular-nums-mono text-[var(--text-secondary)]">
                          {item.cues.length} 条
                          {item.metrics && item.metrics.wall_time_ms > 0
                            ? ` · ${(item.metrics.wall_time_ms / 1000).toFixed(1)}s`
                            : ''}
                        </span>
                        {lowCount > 0 && (
                          <span className="inline-flex items-center gap-0.5 rounded-[var(--radius-xs)] bg-[var(--tint-warning)] px-1.5 py-0.5 text-[10px] font-medium text-[var(--status-warning)]">
                            <AlertTriangle className="h-2.5 w-2.5" strokeWidth={2} />
                            <span>{lowCount} 存疑</span>
                          </span>
                        )}
                      </div>
                    )}

                    {(item.status === 'failed' || item.status === 'cancelled') && (
                      <div className="flex items-center justify-between gap-2 text-[11px]">
                        <span
                          className="truncate text-[var(--status-failed)]"
                          title={item.error?.message ?? '已取消'}
                        >
                          {item.error?.message ?? '任务已取消'}
                        </span>
                        <button
                          type="button"
                          onClick={(e) => {
                            e.stopPropagation();
                            void startTaskExtraction(item.id);
                          }}
                          className="inline-flex shrink-0 items-center gap-0.5 rounded-[var(--radius-xs)] px-1.5 py-0.5 text-[10px] font-medium text-[var(--text-primary)] hover:bg-[var(--bg-panel)]"
                        >
                          <RotateCcw className="h-2.5 w-2.5" />
                          <span>重试</span>
                        </button>
                      </div>
                    )}

                    {item.status === 'idle' && item.capabilityWarning && (
                      <p className="truncate text-[10px] text-[var(--status-warning)]">
                        {item.capabilityWarning}
                      </p>
                    )}
                  </div>
                </div>
              </div>
            );
          })
        )}
      </div>

      {/* 底部批量导出栏 */}
      {tasks.length > 0 && (
        <div className="border-t border-[var(--border-subtle)] p-2.5">
          <Button
            size="sm"
            variant="secondary"
            className="w-full"
            disabled={!commands.batchExportSrt.enabled}
            title={
              commands.batchExportSrt.reason ??
              '批量导出已完成剧集至原片同目录'
            }
            onClick={() => setBatchExportOpen(true)}
          >
            <Download className="h-3.5 w-3.5" strokeWidth={1.75} />
            <span>批量导出 SRT</span>
          </Button>
        </div>
      )}

      <ApplyConfigConfirmModal
        open={applyModalOpen}
        onOpenChange={setApplyModalOpen}
        completedCount={completedTargets.length}
        pendingCount={pendingTargets.length}
      />

      <BatchExportConflictModal
        open={batchExportOpen}
        onOpenChange={setBatchExportOpen}
      />
    </aside>
  );
}
