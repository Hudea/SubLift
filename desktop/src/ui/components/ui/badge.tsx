import {
  AlertCircle,
  CheckCircle2,
  CircleDashed,
  Loader2,
  Slash,
  Sparkles,
} from 'lucide-react';
import type { ExtractionSubPhase, TaskStatus, WorkbenchEngine } from '../../../core/types';
import type { TaskProgressState } from '../../../store';
import { cn } from '../../lib/utils';

interface StatusBadgeProps {
  status: TaskStatus;
  subPhase?: ExtractionSubPhase | null;
  progress?: TaskProgressState | null;
  engine?: WorkbenchEngine | null;
  isAutoDetecting?: boolean;
  className?: string;
}

export function formatExtractionSubPhaseLabel(
  subPhase?: ExtractionSubPhase | null,
  progress?: TaskProgressState | null,
  options?: { engine?: WorkbenchEngine | null; isAutoDetecting?: boolean }
): string {
  if (options?.isAutoDetecting) {
    return '自动探测选区中…';
  }
  switch (subPhase) {
    case 'probing':
      return '探测媒体中…';
    case 'timing':
      return options?.engine && options.engine !== 'vlm'
        ? '打轴与识别中…'
        : '打轴抽条中…';
    case 'local_ocr':
      return '本地识别中…';
    case 'vlm_sheets':
      return progress && progress.totalSheets > 0
        ? `VLM 识别中 (${progress.completedSheets}/${progress.totalSheets})`
        : 'VLM 识别中…';
    case 'finalizing':
      return '规范化装配中…';
    default:
      return '提取中…';
  }
}

export function StatusBadge({
  status,
  subPhase,
  progress,
  engine,
  isAutoDetecting,
  className,
}: StatusBadgeProps) {
  switch (status) {
    case 'extracting':
      return (
        <span
          className={cn(
            'inline-flex items-center gap-1 rounded-[var(--radius-sm)] bg-[var(--tint-extracting)] px-1.5 py-0.5 text-[11px] font-medium text-[var(--text-primary)]',
            className
          )}
        >
          <Loader2
            className="h-3 w-3 animate-spin text-[var(--status-extracting)]"
            strokeWidth={1.75}
          />
          <span>
            {formatExtractionSubPhaseLabel(subPhase, progress, {
              engine,
              isAutoDetecting,
            })}
          </span>
        </span>
      );
    case 'ready':
      return (
        <span
          className={cn(
            'inline-flex items-center gap-1 rounded-[var(--radius-sm)] bg-[var(--tint-ready)] px-1.5 py-0.5 text-[11px] font-medium text-[var(--text-primary)]',
            className
          )}
        >
          <Sparkles className="h-3 w-3 text-[var(--status-ready)]" strokeWidth={1.75} />
          <span>待校对</span>
        </span>
      );
    case 'exported':
      return (
        <span
          className={cn(
            'inline-flex items-center gap-1 rounded-[var(--radius-sm)] bg-[var(--tint-exported)] px-1.5 py-0.5 text-[11px] font-medium text-[var(--text-primary)]',
            className
          )}
        >
          <CheckCircle2
            className="h-3 w-3 text-[var(--status-exported)]"
            strokeWidth={1.75}
          />
          <span>已导出</span>
        </span>
      );
    case 'failed':
      return (
        <span
          className={cn(
            'inline-flex items-center gap-1 rounded-[var(--radius-sm)] bg-[var(--tint-failed)] px-1.5 py-0.5 text-[11px] font-medium text-[var(--text-primary)]',
            className
          )}
        >
          <AlertCircle
            className="h-3 w-3 text-[var(--status-failed)]"
            strokeWidth={1.75}
          />
          <span>失败</span>
        </span>
      );
    case 'cancelled':
      return (
        <span
          className={cn(
            'inline-flex items-center gap-1 rounded-[var(--radius-sm)] bg-[var(--bg-hover)] px-1.5 py-0.5 text-[11px] font-medium text-[var(--text-primary)]',
            className
          )}
        >
          <Slash className="h-3 w-3 text-[var(--status-cancelled)]" strokeWidth={1.75} />
          <span>已取消</span>
        </span>
      );
    case 'idle':
    default:
      return (
        <span
          className={cn(
            'inline-flex items-center gap-1 rounded-[var(--radius-sm)] bg-[var(--bg-hover)] px-1.5 py-0.5 text-[11px] font-medium text-[var(--text-secondary)]',
            className
          )}
        >
          <CircleDashed className="h-3 w-3 text-[var(--status-idle)]" strokeWidth={1.75} />
          <span>待配置</span>
        </span>
      );
  }
}
