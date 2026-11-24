import { useEffect, useRef, useState } from 'react';
import { formatHudTimecode } from '../../core/timecode';
import type { SourceBox } from '../../core/types';
import { useWorkbenchStore } from '../../store';

interface RegionCropPreviewProps {
  videoPath: string | null;
  timestampMs: number;
  regionBox: SourceBox | null;
  displayTransformOk: boolean;
}

export function RegionCropPreview({
  videoPath,
  timestampMs,
  regionBox,
  displayTransformOk,
}: RegionCropPreviewProps) {
  const bridge = useWorkbenchStore((s) => s.bridge);
  const [cropUrl, setCropUrl] = useState<string | null>(null);
  const reqGenRef = useRef(0);

  useEffect(() => {
    reqGenRef.current += 1;
    setCropUrl(null);
  }, [videoPath]);

  useEffect(() => {
    const gen = ++reqGenRef.current;
    if (!videoPath || !regionBox || !displayTransformOk) {
      setCropUrl(null);
      return;
    }
    const timer = setTimeout(() => {
      void bridge
        .extractPreviewFrame(videoPath, timestampMs, regionBox, 960)
        .then((res) => {
          if (reqGenRef.current === gen) {
            setCropUrl(res.image_url);
          }
        })
        .catch(() => {
          // 裁剪预览失败保持上一帧或空态，不打断主流程
        });
    }, 100);

    return () => clearTimeout(timer);
  }, [
    bridge,
    videoPath,
    timestampMs,
    regionBox?.x,
    regionBox?.y,
    regionBox?.width,
    regionBox?.height,
    displayTransformOk,
  ]);

  return (
    <div
      data-testid="region-crop-preview"
      className="flex h-[72px] shrink-0 items-center justify-between gap-3 border-t border-[var(--border-subtle)] bg-[var(--bg-canvas)] px-3 py-1.5"
    >
      <div className="flex h-full min-w-0 flex-1 items-center justify-center overflow-hidden rounded-[var(--radius-sm)] border border-[var(--border-subtle)] bg-[var(--stage)]">
        {cropUrl ? (
          <img
            src={cropUrl}
            alt="字幕选区放大校验"
            className="max-h-full max-w-full object-contain"
          />
        ) : (
          <span className="text-[11px] text-[var(--text-muted)]">
            {regionBox ? '正在载入选区校验图…' : '未选定字幕区域'}
          </span>
        )}
      </div>

      <div className="tabular-nums-mono shrink-0 text-right text-[11px] leading-4 text-[var(--text-secondary)]">
        <div>{formatHudTimecode(timestampMs)}</div>
        <div className="text-[var(--text-muted)]">
          {regionBox ? `${regionBox.width} × ${regionBox.height} px` : '—'}
        </div>
      </div>
    </div>
  );
}
