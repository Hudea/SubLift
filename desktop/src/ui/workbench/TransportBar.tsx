import { Pause, Play } from 'lucide-react';
import { formatHudTimecode } from '../../core/timecode';
import { selectActiveTask, useWorkbenchStore } from '../../store';
import { Button } from '../components/ui/button';
import { Slider } from '../components/ui/slider';

export function TransportBar() {
  const activeTask = useWorkbenchStore(selectActiveTask);
  const isDirtyInlineEdit = useWorkbenchStore((s) => s.isDirtyInlineEdit);
  const setPlaybackMode = useWorkbenchStore((s) => s.setPlaybackMode);
  const stepPreview = useWorkbenchStore((s) => s.stepPreview);
  const seekPreview = useWorkbenchStore((s) => s.seekPreview);

  const probe = activeTask?.probe ?? null;
  const durationMs = probe?.duration_ms ?? 0;
  const currentMs = activeTask?.previewTimestampMs ?? 0;
  const isPlaying = activeTask?.playbackMode === 'playing';
  const disabled = !activeTask || !probe || isDirtyInlineEdit;
  const lockedTitle = isDirtyInlineEdit
    ? '存在未提交的行内编辑，播控已锁定'
    : undefined;

  return (
    <div className="flex h-[var(--spacing-transport)] shrink-0 items-center gap-2 border-t border-[var(--border-subtle)] bg-[var(--bg-panel)] px-3">
      <Button
        size="icon"
        variant="secondary"
        disabled={disabled}
        aria-label={isPlaying ? '暂停播放' : '播放视频'}
        title={lockedTitle ?? (isPlaying ? '暂停 (Space)' : '播放 (Space)')}
        onClick={() =>
          activeTask &&
          setPlaybackMode(isPlaying ? 'still' : 'playing', activeTask.id)
        }
      >
        {isPlaying ? (
          <Pause className="h-3.5 w-3.5" strokeWidth={1.75} />
        ) : (
          <Play className="h-3.5 w-3.5" strokeWidth={1.75} />
        )}
      </Button>

      <Button
        size="xs"
        variant="ghost"
        disabled={disabled}
        title={lockedTitle ?? '后退 5 秒 (Shift+←)'}
        onClick={() => activeTask && stepPreview(-5000, activeTask.id)}
      >
        -5s
      </Button>
      <Button
        size="xs"
        variant="ghost"
        disabled={disabled}
        title={lockedTitle ?? '后退 1 秒 (←)'}
        onClick={() => activeTask && stepPreview(-1000, activeTask.id)}
      >
        -1s
      </Button>
      <Button
        size="xs"
        variant="ghost"
        disabled={disabled}
        title={lockedTitle ?? '前进 1 秒 (→)'}
        onClick={() => activeTask && stepPreview(1000, activeTask.id)}
      >
        +1s
      </Button>
      <Button
        size="xs"
        variant="ghost"
        disabled={disabled}
        title={lockedTitle ?? '前进 5 秒 (Shift+→)'}
        onClick={() => activeTask && stepPreview(5000, activeTask.id)}
      >
        +5s
      </Button>

      <div className="flex-1 px-1">
        <Slider
          aria-label="播放时间轴"
          disabled={disabled || durationMs <= 0}
          min={0}
          max={Math.max(1, durationMs)}
          step={50}
          value={[Math.min(currentMs, Math.max(1, durationMs))]}
          onValueChange={([val]) => {
            if (activeTask && val !== undefined) {
              seekPreview(val, activeTask.id);
            }
          }}
        />
      </div>

      <span className="tabular-nums-mono shrink-0 text-[11px] text-[var(--text-secondary)]">
        {formatHudTimecode(currentMs)} / {formatHudTimecode(durationMs)}
      </span>

      <span className="hidden shrink-0 rounded-[var(--radius-sm)] border border-[var(--border-subtle)] bg-[var(--bg-elevated)] px-1.5 py-0.5 text-[10px] text-[var(--text-secondary)] sm:inline-block">
        {isPlaying ? '流式播放 (sublift://)' : '静态帧 (FFmpeg)'}
      </span>
    </div>
  );
}
