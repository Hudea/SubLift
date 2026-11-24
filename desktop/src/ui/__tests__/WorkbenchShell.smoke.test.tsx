import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeAll, beforeEach, describe, expect, it, vi } from 'vitest';
import type { ExtractionResultDto, IpcBridge } from '../../core/types';
import { useWorkbenchStore } from '../../store';
import App from '../App';
import {
  DEFAULT_TEST_METRICS,
  installJsdomPolyfills,
  makeTestVideoInfo,
  resetWorkbenchStore,
} from './testHarness';

beforeAll(() => {
  installJsdomPolyfills();
});

describe('WorkbenchShell smoke & gating', () => {
  beforeEach(() => {
    Object.defineProperty(window, 'innerWidth', {
      configurable: true,
      writable: true,
      value: 1440,
    });
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('hides Apple Vision when absent from capabilities and renders it when present', async () => {
    resetWorkbenchStore({
      getCapabilities: async () => ({
        engines: ['paddle'],
        ffmpeg_available: true,
        hwaccel_modes: [],
      }),
    });

    const { unmount } = render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });
    expect(screen.queryByRole('button', { name: 'Apple Vision' })).toBeNull();
    unmount();

    resetWorkbenchStore({
      getCapabilities: async () => ({
        engines: ['paddle', 'apple_vision'],
        ffmpeg_available: true,
        hwaccel_modes: [],
      }),
    });
    render(<App />);
    await waitFor(() => {
      expect(screen.getByRole('button', { name: 'Apple Vision' })).toBeInTheDocument();
    });
  });

  it('preserves apple_vision draft with warning banner and blocks startExtraction until user switches engine', async () => {
    resetWorkbenchStore({
      loadSessionDraft: async (videoPath) => ({
        video_path: videoPath,
        engine: 'apple_vision',
        provider_id: null,
        script: 'auto',
        sample_fps: 5,
        roi_mode: 'band',
        region_box: { x: 0, y: 756, width: 1920, height: 324 },
        status: 'idle',
        entries: [],
        updated_at_ms: 1000,
      }),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv']);
    });

    expect(
      screen.getByText(/当前环境不支持 Apple Vision/)
    ).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '开始提取' })).toBeDisabled();

    fireEvent.click(screen.getByRole('button', { name: '切换为 PaddleOCR' }));
    expect(screen.getByRole('button', { name: '开始提取' })).toBeEnabled();
  });

  it('auto-collapses queue for 1 video, expands for 2, toggles via Cmd+B, and switches theme & stacked breakpoint', async () => {
    resetWorkbenchStore();
    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv']);
    });
    expect(screen.queryByLabelText('任务队列')).toBeNull();

    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep02.mkv']);
    });
    expect(screen.getByLabelText('任务队列')).toBeInTheDocument();

    fireEvent.keyDown(window, { key: 'b', metaKey: true });
    expect(screen.queryByLabelText('任务队列')).toBeNull();

    fireEvent.click(screen.getByRole('button', { name: '切换浅色主题' }));
    expect(document.documentElement.getAttribute('data-theme')).toBe('light');

    // < 960px responsive stacked tabs
    act(() => {
      window.innerWidth = 720;
      window.dispatchEvent(new Event('resize'));
    });
    expect(screen.getByRole('tab', { name: '字幕校对' })).toBeInTheDocument();
    expect(screen.getByRole('tab', { name: '选区与参数' })).toBeInTheDocument();
    expect(screen.getByRole('tab', { name: '任务队列' })).toBeInTheDocument();
  });

  it('wraps TopWorkbenchBar and displays alert banners in <960px stacked mode, keeps queue remove button focus-visible, and flushes trailing aria-live announcements', async () => {
    Object.defineProperty(window, 'innerWidth', {
      configurable: true,
      writable: true,
      value: 640,
    });

    let resolveExtract!: (res: ExtractionResultDto) => void;

    resetWorkbenchStore({
      probeVideo: vi.fn(async (path: string) =>
        path.includes('weird')
          ? makeTestVideoInfo(1920, 1080, 60_000, false, '旋转矩阵异常')
          : makeTestVideoInfo(1920, 1080, 60_000, true, null)
      ),
      startExtraction: vi.fn<IpcBridge['startExtraction']>(
        () =>
          new Promise<ExtractionResultDto>((resolve) => {
            resolveExtract = resolve;
          })
      ),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    const header = document.querySelector('header');
    expect(header?.className).toContain('flex-wrap');
    expect(screen.getByRole('button', { name: '开始提取' })).toBeInTheDocument();
    expect(screen.getAllByRole('button', { name: '导出 SRT' })[0]).toBeInTheDocument();

    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/weird.mkv', '/v/ok.mkv']);
    });

    // Stacked mode (<960px) renders alert banner for transformBroken
    expect(screen.getByRole('alert')).toHaveTextContent('旋转矩阵异常');

    // Queue tab remove button has keyboard focus-within visibility
    const queueTab = screen.getByRole('tab', { name: '任务队列' });
    fireEvent.mouseDown(queueTab, { button: 0 });
    fireEvent.click(queueTab);
    const removeBtn = await screen.findByRole('button', { name: '移除 ok.mkv' });
    expect(removeBtn.className).toContain('group-focus-within:inline-flex');

    // Select ok.mkv and verify trailing aria-live timer flushes final ready announcement
    act(() => {
      useWorkbenchStore.getState().selectActiveTask(useWorkbenchStore.getState().tasks[1].id);
    });
    vi.useFakeTimers();
    let runPromise!: Promise<void>;
    act(() => {
      runPromise = useWorkbenchStore.getState().startTaskExtraction();
    });
    const liveEl = document.querySelector('[aria-live="polite"]')!;
    expect(liveEl.textContent).toContain('正在提取 ok.mkv');

    await act(async () => {
      await vi.advanceTimersByTimeAsync(500);
      resolveExtract({
        entries: [{ id: 1, start_ms: 0, end_ms: 1000, text: '完成', confidence: 0.98 }],
        metrics: DEFAULT_TEST_METRICS,
      });
      await runPromise;
    });

    // Before 2000ms trailing window elapses, not yet overwritten; after advancing 1600ms, final ready announcement appears
    await act(async () => {
      await vi.advanceTimersByTimeAsync(1600);
    });
    expect(liveEl.textContent).toBe('已提取 1 条字幕');
  });
});
