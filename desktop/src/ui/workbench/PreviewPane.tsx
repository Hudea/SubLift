import { AlertTriangle } from 'lucide-react';
import { selectActiveTask, useWorkbenchStore } from '../../store';
import { Button } from '../components/ui/button';
import { ExtractionParamStrip } from './ExtractionParamStrip';
import { PreviewCanvas } from './PreviewCanvas';
import { RegionCropPreview } from './RegionCropPreview';
import { TransportBar } from './TransportBar';

export function PreviewAlertBanners() {
  const activeTask = useWorkbenchStore(selectActiveTask);
  const setTaskEngine = useWorkbenchStore((s) => s.setTaskEngine);

  const transformBroken = Boolean(
    activeTask?.probe && !activeTask.probe.display_transform_ok
  );

  if (!transformBroken && !activeTask?.capabilityWarning) {
    return null;
  }

  return (
    <>
      {/* 非方像素比/异常旋转 Fail-Closed 警告横幅 */}
      {transformBroken && activeTask?.probe && (
        <div
          role="alert"
          className="flex items-center gap-2 border-b border-[var(--status-warning)]/40 bg-[var(--tint-warning)] px-3 py-1.5 text-xs text-[var(--text-primary)]"
        >
          <AlertTriangle className="h-4 w-4 shrink-0 text-[var(--status-warning)]" />
          <span className="flex-1">
            {activeTask.probe.transform_note ||
              '检测到非常规视频旋转/显示变换，已禁用自动探测、选区与提取以避免坐标错位。'}
          </span>
        </div>
      )}

      {/* Apple Vision 草稿恢复不匹配保护横幅 */}
      {activeTask?.capabilityWarning && (
        <div
          role="alert"
          className="flex flex-wrap items-center justify-between gap-2 border-b border-[var(--status-warning)]/40 bg-[var(--tint-warning)] px-3 py-1.5 text-xs text-[var(--text-primary)]"
        >
          <div className="flex items-center gap-2">
            <AlertTriangle className="h-4 w-4 shrink-0 text-[var(--status-warning)]" />
            <span>{activeTask.capabilityWarning}</span>
          </div>
          <div className="flex items-center gap-1.5">
            <Button
              size="xs"
              variant="secondary"
              onClick={() => setTaskEngine('paddle', activeTask.id)}
            >
              切换为 PaddleOCR
            </Button>
            <Button
              size="xs"
              variant="secondary"
              onClick={() => setTaskEngine('vlm', activeTask.id)}
            >
              切换为 VLM
            </Button>
          </div>
        </div>
      )}
    </>
  );
}

export function PreviewPane() {
  const activeTask = useWorkbenchStore(selectActiveTask);

  return (
    <section
      aria-label="视频预览与选区"
      className="flex min-w-0 flex-1 flex-col overflow-hidden bg-[var(--bg-canvas)]"
    >
      <PreviewAlertBanners />

      <PreviewCanvas />

      <RegionCropPreview
        videoPath={activeTask?.videoPath ?? null}
        timestampMs={activeTask?.previewTimestampMs ?? 0}
        regionBox={activeTask?.regionBox ?? null}
        displayTransformOk={Boolean(activeTask?.probe?.display_transform_ok)}
      />

      <TransportBar />

      <ExtractionParamStrip />
    </section>
  );
}
