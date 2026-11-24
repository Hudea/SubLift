import { Loader2, ScanSearch } from 'lucide-react';
import type { RoiMode, SubtitleScript } from '../../core/types';
import {
  selectActiveCommands,
  selectActiveTask,
  useWorkbenchStore,
} from '../../store';
import { Button } from '../components/ui/button';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '../components/ui/select';
import { cn } from '../lib/utils';

const ROI_MODES: Array<{ id: RoiMode; label: string }> = [
  { id: 'auto', label: '自动探测' },
  { id: 'band', label: '字幕带' },
  { id: 'rect', label: '自由矩形' },
];

export function ExtractionParamStrip() {
  const activeTask = useWorkbenchStore(selectActiveTask);
  const commands = useWorkbenchStore(selectActiveCommands);
  const detectAutoRegionForTask = useWorkbenchStore(
    (s) => s.detectAutoRegionForTask
  );
  const setTaskRoiMode = useWorkbenchStore((s) => s.setTaskRoiMode);
  const setTaskScript = useWorkbenchStore((s) => s.setTaskScript);
  const setTaskSampleFps = useWorkbenchStore((s) => s.setTaskSampleFps);

  const isExtracting = activeTask?.status === 'extracting';
  const currentRoiMode: RoiMode = activeTask?.roiMode ?? 'auto';
  const currentScript: SubtitleScript = activeTask?.script ?? 'auto';
  const currentFps = activeTask?.sampleFps ?? 5.0;

  return (
    <div className="flex min-h-[var(--spacing-param-strip)] shrink-0 flex-wrap items-center justify-between gap-2 border-t border-[var(--border-subtle)] bg-[var(--bg-panel)] px-3 py-1.5">
      {/* 左侧：自动探测按钮 + ROI 模式切换 */}
      <div className="flex items-center gap-2">
        <Button
          size="xs"
          variant="outline"
          disabled={!commands.detectAutoRegion.enabled}
          title={commands.detectAutoRegion.reason ?? '自动采样探测字幕带'}
          onClick={() => void detectAutoRegionForTask()}
        >
          {activeTask?.isAutoDetecting ? (
            <Loader2 className="h-3 w-3 animate-spin" />
          ) : (
            <ScanSearch className="h-3 w-3" strokeWidth={1.75} />
          )}
          <span>自动探测</span>
        </Button>

        <div
          role="group"
          aria-label="选区模式"
          className="inline-flex items-center rounded-[var(--radius-md)] border border-[var(--border-default)] bg-[var(--bg-app)] p-0.5"
        >
          {ROI_MODES.map((m) => {
            const active = currentRoiMode === m.id;
            return (
              <button
                key={m.id}
                type="button"
                aria-pressed={active}
                disabled={!commands.editRoi.enabled}
                onClick={() => setTaskRoiMode(m.id)}
                className={cn(
                  'rounded-[var(--radius-sm)] px-2 py-0.5 text-[11px] font-medium transition-colors cursor-pointer disabled:opacity-45',
                  active
                    ? 'bg-[var(--bg-active)] text-[var(--text-primary)]'
                    : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)]'
                )}
              >
                {m.label}
              </button>
            );
          })}
        </div>

        {activeTask?.autoRegionInfo && activeTask.autoRegionInfo.previewText && (
          <span
            className="hidden max-w-[180px] truncate rounded-[var(--radius-xs)] bg-[var(--tint-ready)] px-1.5 py-0.5 text-[10px] text-[var(--status-ready)] xl:inline-block"
            title={`样本识别：${activeTask.autoRegionInfo.previewText} (${Math.round(activeTask.autoRegionInfo.confidence * 100)}%)`}
          >
            样本: {activeTask.autoRegionInfo.previewText}
          </span>
        )}
      </div>

      {/* 右侧：语言脚本 + 采样率 */}
      <div className="flex items-center gap-2.5 text-xs">
        <div className="flex items-center gap-1.5">
          <span className="text-[11px] text-[var(--text-secondary)]">语言脚本</span>
          <Select
            disabled={!activeTask || isExtracting}
            value={currentScript}
            onValueChange={(v) => setTaskScript(v as SubtitleScript)}
          >
            <SelectTrigger aria-label="语言脚本" className="h-6 w-[108px] text-[11px]">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="auto">自动 (auto)</SelectItem>
              <SelectItem value="cjk">中日韩 (cjk)</SelectItem>
              <SelectItem value="latin">拉丁语系 (latin)</SelectItem>
            </SelectContent>
          </Select>
        </div>

        <div className="flex items-center gap-1.5">
          <label
            htmlFor="sample-fps-input"
            className="text-[11px] text-[var(--text-secondary)]"
          >
            采样率 FPS
          </label>
          <input
            id="sample-fps-input"
            type="number"
            min={1}
            max={15}
            step={0.5}
            disabled={!activeTask || isExtracting}
            value={currentFps}
            onChange={(e) => setTaskSampleFps(Number(e.target.value))}
            className="tabular-nums-mono h-6 w-14 rounded-[var(--radius-sm)] border border-[var(--border-input)] bg-[var(--bg-app)] px-1.5 text-[11px] text-[var(--text-primary)] focus:border-[var(--border-focus)] focus:outline-none disabled:opacity-45"
          />
        </div>
      </div>
    </div>
  );
}
