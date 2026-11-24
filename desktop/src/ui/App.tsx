import { useEffect, useRef, useState } from 'react';
import { getCurrentWebview } from '@tauri-apps/api/webview';
import {
  isTauriRuntime,
  selectActiveTask,
  useWorkbenchStore,
} from '../store';
import { Tabs, TabsContent, TabsList, TabsTrigger } from './components/ui/tabs';
import { ToastViewport } from './components/ui/toast';
import { TooltipProvider } from './components/ui/tooltip';
import { useResponsiveBreakpoint } from './hooks/useResponsiveBreakpoint';
import { useWorkbenchShortcuts } from './hooks/useWorkbenchShortcuts';
import { ExtractionParamStrip } from './workbench/ExtractionParamStrip';
import { PreviewAlertBanners, PreviewPane } from './workbench/PreviewPane';
import { PreviewCanvas } from './workbench/PreviewCanvas';
import { ProofreaderPane } from './workbench/ProofreaderPane';
import { QueueSidebar } from './workbench/QueueSidebar';
import { RegionCropPreview } from './workbench/RegionCropPreview';
import { TopWorkbenchBar } from './workbench/TopWorkbenchBar';
import { TransportBar } from './workbench/TransportBar';
import { VlmSettingsModal } from './workbench/VlmSettingsModal';

export default function App() {
  const initialize = useWorkbenchStore((s) => s.initialize);
  const addVideoPaths = useWorkbenchStore((s) => s.addVideoPaths);
  const theme = useWorkbenchStore((s) => s.theme);
  const queueCollapsed = useWorkbenchStore((s) => s.queueCollapsed);
  const setQueueCollapsed = useWorkbenchStore((s) => s.setQueueCollapsed);
  const activeTask = useWorkbenchStore(selectActiveTask);

  const breakpoint = useResponsiveBreakpoint();
  useWorkbenchShortcuts();

  const [stackedTab, setStackedTab] = useState<'proofread' | 'roi' | 'queue'>(
    'proofread'
  );
  const [liveAnnouncement, setLiveAnnouncement] = useState('');
  const lastAnnounceTsRef = useRef(0);
  const announceTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  // 初始化 Rust 能力与 VLM 档案
  useEffect(() => {
    void initialize();
  }, [initialize]);

  // 同步 <html data-theme="...">
  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
  }, [theme]);

  // 监听原生 Tauri OS 文件拖拽
  useEffect(() => {
    if (!isTauriRuntime()) {
      return;
    }
    let unlisten: (() => void) | undefined;
    let cancelled = false;
    void (async () => {
      try {
        const webview = getCurrentWebview();
        const dispose = await webview.onDragDropEvent((event) => {
          if (event.payload.type === 'drop' && event.payload.paths.length > 0) {
            void addVideoPaths(event.payload.paths);
          }
        });
        if (cancelled) {
          dispose();
        } else {
          unlisten = dispose;
        }
      } catch {
        // 非桌面测试宿主忽略
      }
    })();

    return () => {
      cancelled = true;
      unlisten?.();
    };
  }, [addVideoPaths]);

  // 节流（<= 1 次 / 2 秒，含尾随刷新防止丢失终态播报）屏幕阅读器摘要播报
  useEffect(() => {
    if (announceTimerRef.current !== null) {
      clearTimeout(announceTimerRef.current);
      announceTimerRef.current = null;
    }
    if (!activeTask) {
      return;
    }
    let nextMsg: string | null = null;
    if (activeTask.status === 'extracting') {
      nextMsg = `正在提取 ${activeTask.fileName}，已收到 ${activeTask.streamingPreviewCues.length} 条字幕`;
    } else if (activeTask.status === 'ready') {
      nextMsg = `已提取 ${activeTask.cues.length} 条字幕`;
    } else if (activeTask.status === 'failed') {
      nextMsg = `提取失败：${activeTask.fileName}${activeTask.error?.message ? `，${activeTask.error.message}` : ''}`;
    }
    if (!nextMsg) {
      return;
    }
    const now = Date.now();
    const elapsed = now - lastAnnounceTsRef.current;
    if (elapsed >= 2000) {
      lastAnnounceTsRef.current = now;
      setLiveAnnouncement(nextMsg);
    } else {
      const msgToFlush = nextMsg;
      announceTimerRef.current = setTimeout(() => {
        announceTimerRef.current = null;
        lastAnnounceTsRef.current = Date.now();
        setLiveAnnouncement(msgToFlush);
      }, 2000 - elapsed);
    }
    return () => {
      if (announceTimerRef.current !== null) {
        clearTimeout(announceTimerRef.current);
        announceTimerRef.current = null;
      }
    };
  }, [
    activeTask?.id,
    activeTask?.status,
    activeTask?.cues.length,
    activeTask?.streamingPreviewCues.length,
    activeTask?.error?.message,
  ]);

  const leftWidthClass =
    breakpoint === 'compact' ? 'w-[228px]' : 'w-[264px]';
  const rightWidthClass =
    breakpoint === 'compact' ? 'w-[360px]' : 'w-[420px]';

  return (
    <TooltipProvider delayDuration={300}>
      <div className="flex h-screen w-screen flex-col overflow-hidden bg-[var(--bg-app)] text-[var(--text-primary)]">
        <TopWorkbenchBar />

        {/* 主工作区：200% 缩放 (<960px) 切换垂直堆叠 + Tabs；否则三栏/抽屉 */}
        {breakpoint === 'stacked' ? (
          <main className="flex min-h-0 flex-1 flex-col overflow-hidden">
            <PreviewAlertBanners />
            <div className="flex h-[clamp(160px,38dvh,320px)] shrink-0 flex-col bg-[var(--bg-canvas)]">
              <PreviewCanvas />
            </div>

            <Tabs
              value={stackedTab}
              onValueChange={(v) =>
                setStackedTab(v as 'proofread' | 'roi' | 'queue')
              }
              className="flex min-h-0 flex-1 flex-col overflow-hidden"
            >
              <div className="border-y border-[var(--border-subtle)] bg-[var(--bg-panel)] px-3 py-1.5">
                <TabsList className="w-full justify-start">
                  <TabsTrigger value="proofread">字幕校对</TabsTrigger>
                  <TabsTrigger value="roi">选区与参数</TabsTrigger>
                  <TabsTrigger value="queue">任务队列</TabsTrigger>
                </TabsList>
              </div>

              <TabsContent value="proofread" className="min-h-0 flex-1 overflow-hidden">
                <ProofreaderPane className="h-full border-l-0" />
              </TabsContent>
              <TabsContent value="roi" className="min-h-0 flex-1 overflow-y-auto">
                <RegionCropPreview
                  videoPath={activeTask?.videoPath ?? null}
                  timestampMs={activeTask?.previewTimestampMs ?? 0}
                  regionBox={activeTask?.regionBox ?? null}
                  displayTransformOk={Boolean(activeTask?.probe?.display_transform_ok)}
                />
                <TransportBar />
                <ExtractionParamStrip />
              </TabsContent>
              <TabsContent value="queue" className="min-h-0 flex-1 overflow-hidden">
                <QueueSidebar className="h-full border-r-0" />
              </TabsContent>
            </Tabs>
          </main>
        ) : (
          <main className="relative flex min-h-0 flex-1 overflow-hidden">
            {/* 左栏队列（标准三栏展开 vs 960..1159 浮层抽屉） */}
            {!queueCollapsed &&
              (breakpoint === 'drawer' ? (
                <>
                  <div
                    className="fixed inset-0 top-[var(--spacing-toolbar)] z-20 bg-black/50"
                    onClick={() => setQueueCollapsed(true, true)}
                  />
                  <div className="fixed bottom-0 left-0 top-[var(--spacing-toolbar)] z-30 w-[264px] shadow-2xl">
                    <QueueSidebar className="h-full" />
                  </div>
                </>
              ) : (
                <div className={`shrink-0 ${leftWidthClass}`}>
                  <QueueSidebar className="h-full" />
                </div>
              ))}

            {/* 中栏预览画布与 ROI */}
            <div className="flex min-w-[440px] flex-1 overflow-hidden">
              <PreviewPane />
            </div>

            {/* 右栏虚拟列表校对器 */}
            <div className={`shrink-0 ${rightWidthClass}`}>
              <ProofreaderPane className="h-full" />
            </div>
          </main>
        )}

        <VlmSettingsModal />
        <ToastViewport />

        {/* 节流屏幕阅读器摘要播报 */}
        <div
          aria-live="polite"
          aria-atomic="true"
          className="sr-only"
        >
          {liveAnnouncement}
        </div>
      </div>
    </TooltipProvider>
  );
}
