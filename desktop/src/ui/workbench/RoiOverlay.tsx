import { useEffect, useMemo, useRef, useState } from 'react';
import { Lock } from 'lucide-react';
import {
  clampPointToContainContent,
  clampSourceBox,
  computeContainRect,
  containPointsToSourceBox,
  isPointInContainContent,
  MIN_NEW_ROI_DRAG_PX,
  nudgeSourceBox,
  roundAndClampEdges,
  sourceBoxToContainRect,
  toBandSourceBox,
} from '../../core/roi';
import type { RoiMode, SourceBox } from '../../core/types';
import { cn } from '../lib/utils';

type HandleKind =
  | 'move'
  | 'create'
  | 'nw'
  | 'n'
  | 'ne'
  | 'e'
  | 'se'
  | 's'
  | 'sw'
  | 'w';

interface DragSession {
  kind: HandleKind;
  startClientX: number;
  startClientY: number;
  startLocalX: number;
  startLocalY: number;
  initialBox: SourceBox | null;
}

interface RoiOverlayProps {
  taskId?: string;
  stageWidth: number;
  stageHeight: number;
  srcWidth: number;
  srcHeight: number;
  roiMode: RoiMode;
  regionBox: SourceBox | null;
  disabled: boolean;
  lockedMessage?: string;
  onChange: (nextBox: SourceBox) => void;
  onInteractionStart?: () => void;
}

const RECT_HANDLES: Array<{
  kind: Exclude<HandleKind, 'move' | 'create'>;
  cursor: string;
  posClass: string;
}> = [
  { kind: 'nw', cursor: 'nwse-resize', posClass: '-left-1 -top-1' },
  { kind: 'n', cursor: 'ns-resize', posClass: 'left-1/2 -top-1 -translate-x-1/2' },
  { kind: 'ne', cursor: 'nesw-resize', posClass: '-right-1 -top-1' },
  { kind: 'e', cursor: 'ew-resize', posClass: '-right-1 top-1/2 -translate-y-1/2' },
  { kind: 'se', cursor: 'nwse-resize', posClass: '-bottom-1 -right-1' },
  { kind: 's', cursor: 'ns-resize', posClass: '-bottom-1 left-1/2 -translate-x-1/2' },
  { kind: 'sw', cursor: 'nesw-resize', posClass: '-bottom-1 -left-1' },
  { kind: 'w', cursor: 'ew-resize', posClass: '-left-1 top-1/2 -translate-y-1/2' },
];

const BAND_HANDLES: Array<{
  kind: 'n' | 's';
  cursor: string;
  posClass: string;
}> = [
  { kind: 'n', cursor: 'ns-resize', posClass: 'left-1/2 -top-1 -translate-x-1/2' },
  { kind: 's', cursor: 'ns-resize', posClass: '-bottom-1 left-1/2 -translate-x-1/2' },
];

export function RoiOverlay({
  taskId,
  stageWidth,
  stageHeight,
  srcWidth,
  srcHeight,
  roiMode,
  regionBox,
  disabled,
  lockedMessage,
  onChange,
  onInteractionStart,
}: RoiOverlayProps) {
  const overlayRef = useRef<HTMLDivElement | null>(null);
  const dragRef = useRef<DragSession | null>(null);
  const preKeyboardBoxRef = useRef<SourceBox | null>(null);
  const draftBoxRef = useRef<SourceBox | null>(null);

  const [draftBox, setDraftBox] = useState<SourceBox | null>(null);
  const updateDraftBox = (next: SourceBox | null) => {
    draftBoxRef.current = next;
    setDraftBox(next);
  };
  const [numericOpen, setNumericOpen] = useState(false);
  const [numericValues, setNumericValues] = useState({
    x: '0',
    y: '0',
    width: '0',
    height: '0',
  });

  const contain = useMemo(
    () => computeContainRect(stageWidth, stageHeight, srcWidth, srcHeight),
    [stageWidth, stageHeight, srcWidth, srcHeight]
  );

  const activeBox = draftBox ?? regionBox;
  const displayRect = useMemo(() => {
    if (!contain || !activeBox) {
      return null;
    }
    return sourceBoxToContainRect(activeBox, contain);
  }, [contain, activeBox]);

  useEffect(() => {
    dragRef.current = null;
    preKeyboardBoxRef.current = null;
    updateDraftBox(null);
    setNumericOpen(false);
  }, [taskId, disabled]);

  useEffect(() => {
    if (activeBox) {
      setNumericValues({
        x: String(activeBox.x),
        y: String(activeBox.y),
        width: String(activeBox.width),
        height: String(activeBox.height),
      });
    }
  }, [activeBox?.x, activeBox?.y, activeBox?.width, activeBox?.height]);

  const toLocalPoint = (clientX: number, clientY: number) => {
    const rect = overlayRef.current?.getBoundingClientRect();
    if (!rect) {
      return { x: clientX, y: clientY };
    }
    return {
      x: clientX - rect.left,
      y: clientY - rect.top,
    };
  };

  useEffect(() => {
    const handlePointerMove = (e: PointerEvent) => {
      const drag = dragRef.current;
      if (!drag || !contain) {
        return;
      }
      const local = toLocalPoint(e.clientX, e.clientY);

      if (drag.kind === 'create') {
        const dist = Math.hypot(
          local.x - drag.startLocalX,
          local.y - drag.startLocalY
        );
        if (dist < MIN_NEW_ROI_DRAG_PX) {
          return;
        }
        const box = containPointsToSourceBox(
          { x: drag.startLocalX, y: drag.startLocalY },
          local,
          contain,
          srcWidth,
          srcHeight
        );
        if (box) {
          updateDraftBox(roiMode === 'band' ? toBandSourceBox(box, srcWidth, srcHeight) : box);
        }
        return;
      }

      if (!drag.initialBox) {
        return;
      }
      const clampedStart = clampPointToContainContent(
        drag.startLocalX,
        drag.startLocalY,
        contain
      );
      const clampedCurr = clampPointToContainContent(local.x, local.y, contain);
      const dxSrc = (clampedCurr.x - clampedStart.x) / contain.scale;
      const dySrc = (clampedCurr.y - clampedStart.y) / contain.scale;
      const init = drag.initialBox;

      if (drag.kind === 'move') {
        if (roiMode === 'band') {
          const maxY = Math.max(0, srcHeight - init.height);
          const nextY = Math.max(0, Math.min(maxY, Math.round(init.y + dySrc)));
          updateDraftBox({ x: 0, y: nextY, width: Math.floor(srcWidth), height: init.height });
        } else {
          const maxX = Math.max(0, srcWidth - init.width);
          const maxY = Math.max(0, srcHeight - init.height);
          const nextX = Math.max(0, Math.min(maxX, Math.round(init.x + dxSrc)));
          const nextY = Math.max(0, Math.min(maxY, Math.round(init.y + dySrc)));
          updateDraftBox({ x: nextX, y: nextY, width: init.width, height: init.height });
        }
        return;
      }

      let fx0 = init.x;
      let fy0 = init.y;
      let fx1 = init.x + init.width;
      let fy1 = init.y + init.height;

      if (drag.kind.includes('w')) fx0 += dxSrc;
      if (drag.kind.includes('e')) fx1 += dxSrc;
      if (drag.kind.includes('n')) fy0 += dySrc;
      if (drag.kind.includes('s')) fy1 += dySrc;

      const updated = roundAndClampEdges(fx0, fy0, fx1, fy1, srcWidth, srcHeight);
      if (updated) {
        updateDraftBox(roiMode === 'band' ? toBandSourceBox(updated, srcWidth, srcHeight) : updated);
      }
    };

    const handlePointerUp = () => {
      const drag = dragRef.current;
      if (!drag) {
        return;
      }
      dragRef.current = null;
      const latest = draftBoxRef.current;
      updateDraftBox(null);
      if (latest) {
        onChange(latest);
      }
    };

    window.addEventListener('pointermove', handlePointerMove);
    window.addEventListener('pointerup', handlePointerUp);
    window.addEventListener('pointercancel', handlePointerUp);
    return () => {
      window.removeEventListener('pointermove', handlePointerMove);
      window.removeEventListener('pointerup', handlePointerUp);
      window.removeEventListener('pointercancel', handlePointerUp);
    };
  }, [contain, srcWidth, srcHeight, roiMode, onChange]);

  const startDrag = (kind: HandleKind, e: React.PointerEvent) => {
    if (disabled || !contain) {
      return;
    }
    const local = toLocalPoint(e.clientX, e.clientY);
    if (kind === 'create' && !isPointInContainContent(local.x, local.y, contain)) {
      // 黑边拒签：落点在有效内容矩形之外直接拒绝新建框选
      return;
    }
    e.stopPropagation();
    onInteractionStart?.();
    dragRef.current = {
      kind,
      startClientX: e.clientX,
      startClientY: e.clientY,
      startLocalX: local.x,
      startLocalY: local.y,
      initialBox: activeBox,
    };
  };

  const handleKeyDown = (e: React.KeyboardEvent<HTMLDivElement>) => {
    if (disabled || !activeBox) {
      return;
    }
    if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      if (preKeyboardBoxRef.current) {
        onChange(preKeyboardBoxRef.current);
        preKeyboardBoxRef.current = null;
      }
      return;
    }
    if (e.key === 'Enter') {
      e.preventDefault();
      e.stopPropagation();
      preKeyboardBoxRef.current = null;
      return;
    }

    const step = e.shiftKey ? 10 : 1;
    let delta: { dx?: number; dy?: number; dh?: number; lockBandWidth?: boolean } | null =
      null;

    if (e.altKey && (e.key === 'ArrowUp' || e.key === 'ArrowDown')) {
      delta = {
        dh: e.key === 'ArrowUp' ? -step : step,
        lockBandWidth: roiMode === 'band',
      };
    } else if (e.key === 'ArrowUp') {
      delta = { dy: -step, lockBandWidth: roiMode === 'band' };
    } else if (e.key === 'ArrowDown') {
      delta = { dy: step, lockBandWidth: roiMode === 'band' };
    } else if (e.key === 'ArrowLeft') {
      e.preventDefault();
      e.stopPropagation();
      if (roiMode !== 'band') {
        delta = { dx: -step };
      }
    } else if (e.key === 'ArrowRight') {
      e.preventDefault();
      e.stopPropagation();
      if (roiMode !== 'band') {
        delta = { dx: step };
      }
    }

    if (delta) {
      e.preventDefault();
      e.stopPropagation();
      onInteractionStart?.();
      if (!preKeyboardBoxRef.current) {
        preKeyboardBoxRef.current = activeBox;
      }
      const nudged = nudgeSourceBox(activeBox, srcWidth, srcHeight, delta);
      if (nudged) {
        onChange(nudged);
      }
    }
  };

  const handleNumericSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    onInteractionStart?.();
    const raw: SourceBox = {
      x: Number(numericValues.x) || 0,
      y: Number(numericValues.y) || 0,
      width: Math.max(1, Number(numericValues.width) || 1),
      height: Math.max(1, Number(numericValues.height) || 1),
    };
    const next =
      roiMode === 'band'
        ? toBandSourceBox(raw, srcWidth, srcHeight)
        : clampSourceBox(raw, srcWidth, srcHeight);
    if (next) {
      onChange(next);
    }
    setNumericOpen(false);
  };

  if (!contain) {
    return null;
  }

  const pctH =
    activeBox && srcHeight > 0
      ? ((activeBox.height / srcHeight) * 100).toFixed(1)
      : '0.0';

  return (
    <div
      ref={overlayRef}
      data-testid="roi-stage-overlay"
      onPointerDown={(e) => {
        if (roiMode === 'rect' || roiMode === 'auto') {
          startDrag('create', e);
        }
      }}
      className={cn(
        'absolute inset-0 select-none',
        disabled ? 'cursor-not-allowed' : roiMode === 'rect' ? 'cursor-crosshair' : 'cursor-default'
      )}
    >
      {/* 字幕带模式：框外 45% 遮罩 */}
      {roiMode === 'band' && displayRect && (
        <>
          <div
            className="pointer-events-none absolute bg-[var(--roi-mask)]"
            style={{
              left: contain.offsetX,
              top: contain.offsetY,
              width: contain.drawW,
              height: Math.max(0, displayRect.top - contain.offsetY),
            }}
          />
          <div
            className="pointer-events-none absolute bg-[var(--roi-mask)]"
            style={{
              left: contain.offsetX,
              top: displayRect.top + displayRect.height,
              width: contain.drawW,
              height: Math.max(
                0,
                contain.offsetY + contain.drawH - (displayRect.top + displayRect.height)
              ),
            }}
          />
        </>
      )}

      {/* ROI 选区框体 */}
      {displayRect && activeBox && (
        <div
          role="region"
          aria-label="字幕选区 ROI"
          tabIndex={disabled ? -1 : 0}
          onKeyDown={handleKeyDown}
          onPointerDown={(e) => startDrag('move', e)}
          style={{
            left: displayRect.left,
            top: displayRect.top,
            width: Math.max(2, displayRect.width),
            height: Math.max(2, displayRect.height),
          }}
          className={cn(
            'absolute border-2 border-[var(--roi)] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[var(--roi-accent)]',
            disabled ? 'cursor-not-allowed opacity-75' : 'cursor-move'
          )}
        >
          {/* 模式角标 */}
          <span className="pointer-events-none absolute left-1 top-1 rounded-[var(--radius-xs)] bg-black/75 px-1.5 py-0.5 text-[10px] font-medium text-[var(--roi)]">
            {roiMode === 'band' ? '字幕带' : roiMode === 'auto' ? '自动选区' : '自由矩形'}
          </span>

          {!disabled &&
            (roiMode === 'band' ? BAND_HANDLES : RECT_HANDLES).map((h) => (
              <div
                key={h.kind}
                onPointerDown={(e) => startDrag(h.kind, e)}
                style={{ cursor: h.cursor }}
                className={cn(
                  'absolute flex h-4 w-4 items-center justify-center',
                  h.posClass
                )}
              >
                <span className="h-2 w-2 border border-black bg-[var(--roi)]" />
              </div>
            ))}
        </div>
      )}

      {/* 提取锁定态胶囊 */}
      {disabled && lockedMessage && (
        <div
          title={lockedMessage}
          className="pointer-events-none absolute right-2 top-2 inline-flex items-center gap-1 rounded-[var(--radius-sm)] border border-[var(--border-default)] bg-[var(--bg-elevated)] px-2 py-1 text-[11px] text-[var(--text-primary)]"
        >
          <Lock className="h-3 w-3 text-[var(--status-warning)]" />
          <span>{lockedMessage}</span>
        </div>
      )}

      {/* 底部 HUD 坐标胶囊 + 紧凑像素输入 */}
      {activeBox && (
        <div
          className="absolute bottom-2 left-1/2 z-10 -translate-x-1/2"
          onPointerDown={(e) => e.stopPropagation()}
        >
          {numericOpen && !disabled ? (
            <form
              onSubmit={handleNumericSubmit}
              onKeyDown={(e) => {
                if (e.key === 'Escape') {
                  e.preventDefault();
                  e.stopPropagation();
                  setNumericOpen(false);
                }
              }}
              className="flex items-center gap-1 rounded-[var(--radius-md)] border border-[var(--border-default)] bg-[var(--bg-elevated)] px-2 py-1 text-[11px] shadow-lg"
            >
              {roiMode !== 'band' && (
                <>
                  <label className="text-[var(--text-muted)]">X</label>
                  <input
                    aria-label="ROI X"
                    type="number"
                    value={numericValues.x}
                    onChange={(e) =>
                      setNumericValues({ ...numericValues, x: e.target.value })
                    }
                    className="tabular-nums-mono w-12 rounded border border-[var(--border-input)] bg-[var(--bg-panel)] px-1 text-[11px]"
                  />
                </>
              )}
              <label className="text-[var(--text-muted)]">Y</label>
              <input
                aria-label="ROI Y"
                type="number"
                value={numericValues.y}
                onChange={(e) =>
                  setNumericValues({ ...numericValues, y: e.target.value })
                }
                className="tabular-nums-mono w-12 rounded border border-[var(--border-input)] bg-[var(--bg-panel)] px-1 text-[11px]"
              />
              {roiMode !== 'band' && (
                <>
                  <label className="text-[var(--text-muted)]">W</label>
                  <input
                    aria-label="ROI Width"
                    type="number"
                    value={numericValues.width}
                    onChange={(e) =>
                      setNumericValues({ ...numericValues, width: e.target.value })
                    }
                    className="tabular-nums-mono w-12 rounded border border-[var(--border-input)] bg-[var(--bg-panel)] px-1 text-[11px]"
                  />
                </>
              )}
              <label className="text-[var(--text-muted)]">H</label>
              <input
                aria-label="ROI Height"
                type="number"
                value={numericValues.height}
                onChange={(e) =>
                  setNumericValues({ ...numericValues, height: e.target.value })
                }
                className="tabular-nums-mono w-12 rounded border border-[var(--border-input)] bg-[var(--bg-panel)] px-1 text-[11px]"
              />
              <button
                type="submit"
                className="rounded bg-[var(--primary)] px-1.5 py-0.5 text-[10px] text-[var(--primary-foreground)]"
              >
                应用
              </button>
              <button
                type="button"
                aria-label="取消坐标输入"
                onClick={() => setNumericOpen(false)}
                className="rounded px-1.5 py-0.5 text-[10px] text-[var(--text-secondary)] hover:bg-[var(--bg-hover)]"
              >
                取消
              </button>
            </form>
          ) : (
            <button
              type="button"
              disabled={disabled}
              onClick={() => {
                if (activeBox) {
                  setNumericValues({
                    x: String(activeBox.x),
                    y: String(activeBox.y),
                    width: String(activeBox.width),
                    height: String(activeBox.height),
                  });
                }
                setNumericOpen(true);
              }}
              className="tabular-nums-mono rounded-[var(--radius-sm)] border border-[var(--border-default)] bg-[var(--bg-elevated)]/95 px-2 py-0.5 text-[11px] text-[var(--text-primary)] hover:bg-[var(--bg-hover)] disabled:opacity-75"
              title="点击手动输入精确源像素坐标"
            >
              x:{activeBox.x} y:{activeBox.y} · {activeBox.width}×{activeBox.height} px · {pctH}%H
            </button>
          )}
        </div>
      )}
    </div>
  );
}
