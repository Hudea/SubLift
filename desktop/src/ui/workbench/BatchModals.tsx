import { useState } from 'react';
import { CheckCircle2, FileWarning } from 'lucide-react';
import type { BatchExportConflictPolicy } from '../../core/types';
import { useWorkbenchStore } from '../../store';
import { Button } from '../components/ui/button';
import { Checkbox } from '../components/ui/checkbox';
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '../components/ui/dialog';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '../components/ui/select';

interface ApplyConfigModalProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  completedCount: number;
  pendingCount: number;
}

export function ApplyConfigConfirmModal({
  open,
  onOpenChange,
  completedCount,
  pendingCount,
}: ApplyConfigModalProps) {
  const [includeCompleted, setIncludeCompleted] = useState(false);
  const applyConfigToQueue = useWorkbenchStore((s) => s.applyConfigToQueue);

  const handleConfirm = () => {
    applyConfigToQueue({ includeCompleted });
    onOpenChange(false);
    setIncludeCompleted(false);
  };

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>套用当前选区与引擎参数到队列</DialogTitle>
          <DialogDescription>
            将当前选中视频的识别引擎、语言脚本、采样率与 ROI 选区同步到队列其余剧集（跨分辨率自动按比例缩放）。
          </DialogDescription>
        </DialogHeader>

        <div className="mt-3 space-y-3 text-xs">
          <div className="rounded-[var(--radius-md)] border border-[var(--border-subtle)] bg-[var(--bg-panel)] p-2.5 text-[var(--text-secondary)]">
            <p>待处理视频：{pendingCount} 个（将直接更新配置）</p>
            <p className="mt-1">已完成 / 已导出视频：{completedCount} 个（默认受保护跳过）</p>
          </div>

          {completedCount > 0 && (
            <label className="flex cursor-pointer items-center gap-2 text-xs text-[var(--text-primary)]">
              <Checkbox
                checked={includeCompleted}
                onCheckedChange={(checked) => setIncludeCompleted(checked === true)}
              />
              <span>覆盖已完成剧集的选区配置并重置为待提取</span>
            </label>
          )}
        </div>

        <DialogFooter>
          <Button variant="ghost" onClick={() => onOpenChange(false)}>
            取消
          </Button>
          <Button variant="primary" onClick={handleConfirm}>
            确认套用
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}

interface BatchExportModalProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
}

export function BatchExportConflictModal({
  open,
  onOpenChange,
}: BatchExportModalProps) {
  const tasks = useWorkbenchStore((s) => s.tasks);
  const lastExporting = useWorkbenchStore((s) => s.lastExporting);
  const lastBatchReport = useWorkbenchStore((s) => s.lastBatchReport);
  const batchExportSrt = useWorkbenchStore((s) => s.batchExportSrt);
  const clearBatchReport = useWorkbenchStore((s) => s.clearBatchReport);

  const [conflictPolicy, setConflictPolicy] =
    useState<BatchExportConflictPolicy>('rename');
  const [includeEmpty, setIncludeEmpty] = useState(false);

  const readyOrExported = tasks.filter(
    (t) => t.status === 'ready' || t.status === 'exported'
  );
  const nonEmptyCount = readyOrExported.filter((t) => t.cues.length > 0).length;
  const emptyCount = readyOrExported.length - nonEmptyCount;

  const handleClose = (nextOpen: boolean) => {
    if (!nextOpen) {
      clearBatchReport();
    }
    onOpenChange(nextOpen);
  };

  const handleExport = async () => {
    await batchExportSrt(conflictPolicy, includeEmpty);
  };

  return (
    <Dialog open={open} onOpenChange={handleClose}>
      <DialogContent className="max-w-xl">
        <DialogHeader>
          <DialogTitle>批量导出 SRT 至原片同目录</DialogTitle>
          <DialogDescription>
            所有已完成剧集将按原片同目录同名 `.srt` 经 C++ 权威规范化写入磁盘（不产生临时缓存文件）。
          </DialogDescription>
        </DialogHeader>

        {lastBatchReport ? (
          <div className="mt-3 space-y-3 text-xs">
            <div className="flex items-center gap-2 rounded-[var(--radius-md)] border border-[var(--border-default)] bg-[var(--bg-panel)] p-2.5">
              <CheckCircle2 className="h-4 w-4 text-[var(--status-exported)]" />
              <span>
                成功导出 {lastBatchReport.exported_count} 个 · 跳过{' '}
                {lastBatchReport.skipped_count} 个 · 失败{' '}
                {lastBatchReport.failed_count} 个
              </span>
            </div>

            <div className="max-h-56 overflow-y-auto rounded-[var(--radius-md)] border border-[var(--border-subtle)]">
              <table className="w-full text-left text-[11px]">
                <thead className="bg-[var(--bg-panel)] text-[var(--text-secondary)]">
                  <tr>
                    <th className="px-2 py-1.5">输出路径</th>
                    <th className="px-2 py-1.5">状态</th>
                    <th className="px-2 py-1.5">条数</th>
                  </tr>
                </thead>
                <tbody className="divide-y divide-[var(--border-subtle)]">
                  {lastBatchReport.results.map((r) => (
                    <tr key={`${r.video_path}:${r.out_path}`}>
                      <td className="max-w-[280px] truncate px-2 py-1.5" title={r.out_path}>
                        {r.out_path}
                      </td>
                      <td className="px-2 py-1.5">
                        {r.status}
                        {r.error_message ? ` (${r.error_message})` : ''}
                      </td>
                      <td className="tabular-nums-mono px-2 py-1.5">{r.cue_count}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>

            <DialogFooter>
              <Button variant="primary" onClick={() => handleClose(false)}>
                完成
              </Button>
            </DialogFooter>
          </div>
        ) : (
          <div className="mt-3 space-y-3 text-xs">
            <div className="rounded-[var(--radius-md)] border border-[var(--border-subtle)] bg-[var(--bg-panel)] p-2.5 text-[var(--text-secondary)]">
              <p>可导出剧集：{nonEmptyCount} 集（非空字幕）</p>
              {emptyCount > 0 && (
                <p className="mt-1 flex items-center gap-1.5 text-[var(--status-warning)]">
                  <FileWarning className="h-3.5 w-3.5" />
                  <span>检测到 {emptyCount} 集为空字幕，默认跳过不写入 0 字节 `.srt`</span>
                </p>
              )}
            </div>

            {emptyCount > 0 && (
              <label className="flex cursor-pointer items-center gap-2 text-xs text-[var(--text-primary)]">
                <Checkbox
                  checked={includeEmpty}
                  onCheckedChange={(checked) => setIncludeEmpty(checked === true)}
                />
                <span>允许为空字幕剧集写入 0 字节 .srt</span>
              </label>
            )}

            <div className="space-y-1.5">
              <label
                htmlFor="batch-conflict-policy"
                className="block text-[11px] font-medium text-[var(--text-secondary)]"
              >
                同名 .srt 文件冲突策略
              </label>
              <Select
                value={conflictPolicy}
                onValueChange={(v) =>
                  setConflictPolicy(v as BatchExportConflictPolicy)
                }
              >
                <SelectTrigger id="batch-conflict-policy" className="w-full">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value="rename">自动编号避让 (ep01 (1).srt)</SelectItem>
                  <SelectItem value="overwrite">覆盖已存在 .srt</SelectItem>
                  <SelectItem value="skip">跳过已存在文件</SelectItem>
                </SelectContent>
              </Select>
            </div>

            <DialogFooter>
              <Button variant="ghost" onClick={() => handleClose(false)}>
                取消
              </Button>
              <Button
                variant="primary"
                disabled={lastExporting || (nonEmptyCount === 0 && !includeEmpty)}
                onClick={() => void handleExport()}
              >
                {lastExporting ? '批量导出中…' : '开始批量导出'}
              </Button>
            </DialogFooter>
          </div>
        )}
      </DialogContent>
    </Dialog>
  );
}
