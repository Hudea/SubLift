import { useRef } from 'react';
import {
  Download,
  Moon,
  PanelLeft,
  Play,
  Settings,
  Square,
  Sun,
} from 'lucide-react';
import {
  selectActiveCommands,
  selectActiveTask,
  selectEffectiveProvider,
  useWorkbenchStore,
} from '../../store';
import type { WorkbenchEngine } from '../../core/types';
import { StatusBadge } from '../components/ui/badge';
import { Button } from '../components/ui/button';
import { cn } from '../lib/utils';

export function TopWorkbenchBar() {
  const capabilities = useWorkbenchStore((s) => s.capabilities);
  const activeTask = useWorkbenchStore(selectActiveTask);
  const commands = useWorkbenchStore(selectActiveCommands);
  const effectiveProvider = useWorkbenchStore((s) =>
    selectEffectiveProvider(s, activeTask)
  );
  const theme = useWorkbenchStore((s) => s.theme);
  const queueCollapsed = useWorkbenchStore((s) => s.queueCollapsed);
  const lastExporting = useWorkbenchStore((s) => s.lastExporting);

  const toggleQueueSidebar = useWorkbenchStore((s) => s.toggleQueueSidebar);
  const setTaskEngine = useWorkbenchStore((s) => s.setTaskEngine);
  const setVlmSettingsOpen = useWorkbenchStore((s) => s.setVlmSettingsOpen);
  const startTaskExtraction = useWorkbenchStore((s) => s.startTaskExtraction);
  const cancelTaskExtraction = useWorkbenchStore((s) => s.cancelTaskExtraction);
  const exportActiveSrt = useWorkbenchStore((s) => s.exportActiveSrt);
  const setTheme = useWorkbenchStore((s) => s.setTheme);

  const exportBtnRef = useRef<HTMLButtonElement | null>(null);
  const vlmGearBtnRef = useRef<HTMLButtonElement | null>(null);

  const supportsVision = Boolean(
    capabilities?.engines.includes('apple_vision')
  );
  const currentEngine: WorkbenchEngine = activeTask?.engine ?? 'paddle';
  const isExtracting = activeTask?.status === 'extracting';

  const engineOptions: Array<{ id: WorkbenchEngine; label: string }> = [
    { id: 'paddle', label: 'PaddleOCR' },
    ...(supportsVision
      ? [{ id: 'apple_vision' as const, label: 'Apple Vision' }]
      : []),
    { id: 'vlm', label: 'VLM' },
  ];

  const handleExport = async () => {
    await exportActiveSrt();
    requestAnimationFrame(() => {
      exportBtnRef.current?.focus();
    });
  };

  return (
    <header
      /* 76px macOS traffic-light safe zone */
      className="flex min-h-[var(--spacing-toolbar)] h-auto shrink-0 flex-wrap items-center justify-between gap-x-2 gap-y-1 border-b border-[var(--border-subtle)] bg-[var(--bg-panel)] py-1 pl-[var(--spacing-traffic)] pr-3 lg:h-[var(--spacing-toolbar)] lg:flex-nowrap lg:py-0"
      data-tauri-drag-region
    >
      {/* 左侧：队列折叠 + 活动文件摘要 */}
      <div className="flex min-w-0 items-center gap-2">
        <Button
          size="icon"
          variant="ghost"
          aria-label="切换任务队列"
          aria-pressed={!queueCollapsed}
          title="切换任务队列 (⌘B)"
          onClick={toggleQueueSidebar}
        >
          <PanelLeft className="h-4 w-4" strokeWidth={1.75} />
        </Button>

        <span className="hidden text-xs font-semibold tracking-tight text-[var(--text-primary)] sm:inline">
          SubLift
        </span>

        {activeTask ? (
          <div className="flex min-w-0 items-center gap-1.5 text-xs text-[var(--text-secondary)]">
            <span className="hidden text-[var(--text-muted)] sm:inline">·</span>
            <span
              className="max-w-[140px] truncate font-medium text-[var(--text-primary)] sm:max-w-[200px]"
              title={activeTask.videoPath}
            >
              {activeTask.fileName}
            </span>
            {activeTask.probe && (
              <span className="tabular-nums-mono hidden text-[11px] text-[var(--text-muted)] md:inline">
                ({activeTask.probe.width}×{activeTask.probe.height})
              </span>
            )}
            <StatusBadge
              status={activeTask.status}
              subPhase={activeTask.subPhase}
              progress={activeTask.progress}
              engine={activeTask.activeRunConfig?.engine ?? activeTask.engine}
              isAutoDetecting={activeTask.isAutoDetecting}
              className="hidden md:inline-flex"
            />
          </div>
        ) : (
          <span className="hidden text-xs text-[var(--text-muted)] md:inline">未加载视频</span>
        )}
      </div>

      {/* 中间拖拽留白区 */}
      <div className="hidden h-full flex-1 lg:block" data-tauri-drag-region />

      {/* 右侧：三引擎切换 + VLM 设置 + 主 CTA + 导出 */}
      <div className="flex flex-wrap items-center gap-1.5 sm:gap-2">
        <div
          role="group"
          aria-label="识别引擎"
          className="inline-flex items-center rounded-[var(--radius-md)] border border-[var(--border-default)] bg-[var(--bg-app)] p-0.5"
        >
          {engineOptions.map((opt) => {
            const active = currentEngine === opt.id;
            return (
              <button
                key={opt.id}
                type="button"
                aria-pressed={active}
                disabled={!activeTask || isExtracting}
                onClick={() => setTaskEngine(opt.id)}
                className={cn(
                  'rounded-[var(--radius-sm)] px-2 py-0.5 text-[11px] font-medium transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[var(--border-focus)] disabled:opacity-45 cursor-pointer',
                  active
                    ? 'bg-[var(--bg-active)] text-[var(--text-primary)] ring-1 ring-[var(--sl-highlight)]/60'
                    : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)]'
                )}
              >
                {opt.label}
              </button>
            );
          })}
        </div>

        {/* VLM Provider 胶囊与设置入口 */}
        <div className="flex items-center gap-1">
          {currentEngine === 'vlm' && (
            <button
              type="button"
              onClick={() => setVlmSettingsOpen(true)}
              className={cn(
                'inline-flex h-7 items-center rounded-[var(--radius-md)] border px-2 text-[11px] transition-colors cursor-pointer focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[var(--border-focus)]',
                effectiveProvider
                  ? 'border-[var(--border-default)] bg-[var(--bg-elevated)] text-[var(--text-primary)] hover:bg-[var(--bg-hover)]'
                  : 'border-[var(--status-warning)]/60 bg-[var(--tint-warning)] text-[var(--text-primary)]'
              )}
              title="配置 VLM 服务档案"
            >
              {effectiveProvider ? effectiveProvider.name : '未配置 · 点击设置'}
            </button>
          )}
          <Button
            ref={vlmGearBtnRef}
            size="icon"
            variant="ghost"
            aria-label="打开 VLM 设置"
            title="VLM 多配置管理"
            onClick={() => setVlmSettingsOpen(true)}
          >
            <Settings className="h-3.5 w-3.5" strokeWidth={1.75} />
          </Button>
        </div>

        {/* 主题切换 */}
        <Button
          size="icon"
          variant="ghost"
          aria-label={theme === 'dark' ? '切换浅色主题' : '切换深色主题'}
          title={theme === 'dark' ? '切换浅色主题' : '切换深色主题'}
          onClick={() => setTheme(theme === 'dark' ? 'light' : 'dark')}
        >
          {theme === 'dark' ? (
            <Sun className="h-3.5 w-3.5" strokeWidth={1.75} />
          ) : (
            <Moon className="h-3.5 w-3.5" strokeWidth={1.75} />
          )}
        </Button>

        {/* 主提取 CTA */}
        {isExtracting ? (
          <Button
            variant="danger"
            size="sm"
            disabled={!commands.cancelExtraction.enabled || activeTask?.isCancelling}
            onClick={() => void cancelTaskExtraction()}
          >
            <Square className="h-3 w-3" strokeWidth={2} />
            <span>{activeTask?.isCancelling ? '取消中…' : '取消提取'}</span>
          </Button>
        ) : (
          <Button
            variant="primary"
            size="sm"
            disabled={!commands.startExtraction.enabled}
            title={commands.startExtraction.reason ?? '开始提取硬字幕'}
            onClick={() => void startTaskExtraction()}
          >
            <Play className="h-3 w-3" strokeWidth={2} />
            <span>开始提取</span>
          </Button>
        )}

        {/* 导出 SRT */}
        <Button
          ref={exportBtnRef}
          variant="secondary"
          size="sm"
          disabled={!commands.exportActiveSrt.enabled || lastExporting}
          title={commands.exportActiveSrt.reason ?? '导出规范 SRT (⌘S)'}
          onClick={() => void handleExport()}
        >
          <Download className="h-3.5 w-3.5" strokeWidth={1.75} />
          <span>{lastExporting ? '导出中…' : '导出 SRT'}</span>
        </Button>
      </div>
    </header>
  );
}
