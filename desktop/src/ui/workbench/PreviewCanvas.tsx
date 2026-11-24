import { useEffect, useRef, useState } from 'react';
import { buildMediaUrl } from '../../core/mediaUrl';
import { selectActiveTask, useWorkbenchStore } from '../../store';
import { RoiOverlay } from './RoiOverlay';

function isNativeSeekableStream(
  el: HTMLVideoElement,
  probeDurationMs: number | undefined,
  videoPath: string
): boolean {
  if (!probeDurationMs || probeDurationMs <= 0) {
    return false;
  }
  if (!Number.isFinite(el.duration)) {
    return false;
  }
  if (/\.(mkv|webm|avi|ts)$/i.test(videoPath)) {
    return false;
  }
  return Math.abs(el.duration * 1000 - probeDurationMs) < 500;
}

export function PreviewCanvas() {
  const bridge = useWorkbenchStore((s) => s.bridge);
  const activeTask = useWorkbenchStore(selectActiveTask);
  const setTaskRegionBox = useWorkbenchStore((s) => s.setTaskRegionBox);
  const setPlaybackMode = useWorkbenchStore((s) => s.setPlaybackMode);
  const syncPlaybackTime = useWorkbenchStore((s) => s.syncPlaybackTime);

  const stageRef = useRef<HTMLDivElement | null>(null);
  const frameGenRef = useRef(0);
  const lastVideoPathRef = useRef<string | null>(null);

  const [stageSize, setStageSize] = useState({ width: 0, height: 0 });
  const [stillUrl, setStillUrl] = useState<string | null>(null);
  const [isLoadingFrame, setIsLoadingFrame] = useState(false);

  // 切换视频路径时立即清空上一条视频残留静帧并作废在途静帧请求，无论 probe 是否就绪
  useEffect(() => {
    frameGenRef.current += 1;
    setStillUrl(null);
    setIsLoadingFrame(false);
    lastVideoPathRef.current = null;
  }, [activeTask?.videoPath]);

  // 监测纯黑舞台容器尺寸（ResizeObserver + 挂载即读）
  useEffect(() => {
    const el = stageRef.current;
    if (!el) {
      return;
    }
    const measure = () => {
      const rect = el.getBoundingClientRect();
      if (rect.width > 0 && rect.height > 0) {
        setStageSize({ width: rect.width, height: rect.height });
      }
    };
    measure();

    if (typeof ResizeObserver === 'undefined') {
      return;
    }
    const ro = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const { width, height } = entry.contentRect;
        if (width > 0 && height > 0) {
          setStageSize({ width, height });
        }
      }
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, [activeTask?.id]);

  // Tier 1: FFmpeg 静帧画布（90ms 防抖 + frameRequestGen + img.decode 原子替换）
  useEffect(() => {
    const gen = ++frameGenRef.current;
    if (!activeTask || !activeTask.probe || activeTask.playbackMode !== 'still') {
      setIsLoadingFrame(false);
      return;
    }
    const isInitialForVideo = lastVideoPathRef.current !== activeTask.videoPath;
    lastVideoPathRef.current = activeTask.videoPath;
    if (isInitialForVideo) {
      setStillUrl(null);
    }

    const delayMs = isInitialForVideo ? 0 : 90;

    const timer = setTimeout(() => {
      setIsLoadingFrame(true);
      void bridge
        .extractPreviewFrame(
          activeTask.videoPath,
          activeTask.previewTimestampMs,
          undefined,
          1280
        )
        .then(async (res) => {
          if (frameGenRef.current !== gen) {
            return;
          }
          if (typeof Image !== 'undefined') {
            const preImg = new Image();
            preImg.src = res.image_url;
            if (typeof preImg.decode === 'function') {
              await preImg.decode().catch(() => {});
            }
          }
          if (frameGenRef.current === gen) {
            setStillUrl(res.image_url);
            setIsLoadingFrame(false);
          }
        })
        .catch(() => {
          if (frameGenRef.current === gen) {
            setIsLoadingFrame(false);
          }
        });
    }, delayMs);

    return () => clearTimeout(timer);
  }, [
    bridge,
    activeTask?.id,
    activeTask?.videoPath,
    activeTask?.probe,
    activeTask?.previewTimestampMs,
    activeTask?.playbackMode,
  ]);

  // Tier 2: 退出播放态或卸载/切集时，立即调用 stop_media_stream 回收 ffmpeg 子进程
  useEffect(() => {
    if (!activeTask || activeTask.playbackMode !== 'playing') {
      return;
    }
    return () => {
      void bridge.stopMediaStream().catch(() => {});
    };
  }, [bridge, activeTask?.id, activeTask?.playbackMode, activeTask?.streamBaseMs]);

  if (!activeTask) {
    return (
      <div
        ref={stageRef}
        className="flex min-h-[280px] flex-1 items-center justify-center bg-[var(--stage)] text-xs text-[var(--text-muted)]"
      >
        请先添加或拖入视频文件
      </div>
    );
  }

  const probe = activeTask.probe;
  const isPlaying = activeTask.playbackMode === 'playing';
  const roiDisabled =
    !probe || !probe.display_transform_ok || activeTask.status === 'extracting';

  const mediaUrl = isPlaying
    ? buildMediaUrl(
        activeTask.videoPath,
        activeTask.streamBaseMs > 0 ? activeTask.streamBaseMs / 1000 : undefined
      )
    : undefined;

  return (
    <div
      ref={stageRef}
      data-testid="preview-stage"
      className="relative min-h-[280px] flex-1 overflow-hidden bg-[var(--stage)]"
    >
      {/* Tier 2 动态播放层 vs Tier 1 静态帧层 */}
      {isPlaying && mediaUrl ? (
        <video
          data-testid="tier2-video-player"
          src={mediaUrl}
          autoPlay
          playsInline
          onLoadedMetadata={(e) => {
            const el = e.currentTarget;
            if (
              activeTask.streamBaseMs > 0 &&
              isNativeSeekableStream(el, probe?.duration_ms, activeTask.videoPath)
            ) {
              el.currentTime = activeTask.streamBaseMs / 1000;
            }
          }}
          onTimeUpdate={(e) => {
            const el = e.currentTarget;
            if (!Number.isFinite(el.currentTime)) {
              return;
            }
            const isRemuxStream = !isNativeSeekableStream(
              el,
              probe?.duration_ms,
              activeTask.videoPath
            );
            const actualMs = isRemuxStream
              ? activeTask.streamBaseMs + Math.round(el.currentTime * 1000)
              : Math.round(el.currentTime * 1000);
            syncPlaybackTime(actualMs, activeTask.id);
          }}
          onEnded={() => setPlaybackMode('still', activeTask.id)}
          onError={() => setPlaybackMode('still', activeTask.id)}
          className="h-full w-full object-contain"
        />
      ) : stillUrl ? (
        <img
          data-testid="tier1-still-frame"
          src={stillUrl}
          alt={activeTask.fileName}
          draggable={false}
          className="h-full w-full select-none object-contain"
        />
      ) : (
        <div className="flex h-full w-full items-center justify-center text-xs text-[var(--text-muted)]">
          {activeTask.probeLoading || isLoadingFrame
            ? '正在抽取高清原片帧…'
            : activeTask.probeError ?? '暂无预览帧'}
        </div>
      )}

      {/* 探测中/自动探测指示 */}
      {activeTask.isAutoDetecting && (
        <div className="pointer-events-none absolute left-2 top-2 rounded-[var(--radius-sm)] bg-[var(--bg-elevated)] px-2 py-1 text-[11px] text-[var(--text-primary)]">
          正在采样关键帧并自动探测字幕带…
        </div>
      )}

      {/* 严格 contain ROI 选区叠加层（强制采用 probe.width/height） */}
      {probe && stageSize.width > 0 && stageSize.height > 0 && (
        <RoiOverlay
          taskId={activeTask.id}
          stageWidth={stageSize.width}
          stageHeight={stageSize.height}
          srcWidth={probe.width}
          srcHeight={probe.height}
          roiMode={activeTask.roiMode}
          regionBox={activeTask.regionBox}
          disabled={roiDisabled}
          lockedMessage={
            activeTask.status === 'extracting'
              ? '提取进行中，选区已锁定'
              : !probe.display_transform_ok
                ? '显示变换异常，选区已禁用'
                : undefined
          }
          onInteractionStart={() => {
            if (isPlaying) {
              setPlaybackMode('still', activeTask.id);
            }
          }}
          onChange={(box) => setTaskRegionBox(box, activeTask.id)}
        />
      )}
    </div>
  );
}
