import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { beforeAll, beforeEach, describe, expect, it, vi } from 'vitest';
import type { ExtractionResultDto, IpcBridge, TaskEvent } from '../../core/types';
import { useWorkbenchStore } from '../../store';
import App from '../App';
import {
  DEFAULT_TEST_METRICS,
  installJsdomPolyfills,
  resetWorkbenchStore,
} from './testHarness';

beforeAll(() => {
  installJsdomPolyfills();
});

describe('QueueSidebar & BatchModals', () => {
  beforeEach(() => {
    Object.defineProperty(window, 'innerWidth', {
      configurable: true,
      writable: true,
      value: 1440,
    });
  });

  it('applies config across same-res and diff-res queue items and protects ready items until modal confirmed', async () => {
    resetWorkbenchStore();
    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    await act(async () => {
      await useWorkbenchStore
        .getState()
        .addVideoPaths(['/v/ep01.mkv', '/v/ep02_720p.mkv', '/v/ep03.mkv']);
    });

    const tasks = useWorkbenchStore.getState().tasks;
    // Mark ep03 as ready so confirmation modal triggers
    act(() => {
      useWorkbenchStore.setState((s) => ({
        tasks: s.tasks.map((t) =>
          t.id === tasks[2].id
            ? {
                ...t,
                status: 'ready',
                cues: [
                  {
                    id: 1,
                    local_uid: 'c1',
                    start_ms: 0,
                    end_ms: 1000,
                    text: '已完成字幕',
                    confidence: 0.9,
                  },
                ],
              }
            : t
        ),
      }));
    });

    fireEvent.click(screen.getByRole('button', { name: '套用配置到待处理项' }));
    expect(
      screen.getByText('套用当前选区与引擎参数到队列')
    ).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: '确认套用' }));

    expect(screen.getByText('ROI 已按比例缩放')).toBeInTheDocument();
    expect(useWorkbenchStore.getState().tasks[2].status).toBe('ready');
  });

  it('tracks background extraction TaskEvents across activeTaskId switches and runs batch export modal', async () => {
    let emitBg!: (ev: TaskEvent) => void;
    let resolveBg!: (val: {
      entries: Array<{ id: number; start_ms: number; end_ms: number; text: string; confidence: number }>;
      metrics: typeof DEFAULT_TEST_METRICS;
    }) => void;

    const batchExportSpy = vi.fn(async (items) => ({
      results: items.map((it: { video_path: string; out_path: string; entries: unknown[] }) => ({
        video_path: it.video_path,
        out_path: it.out_path,
        status: 'renamed' as const,
        cue_count: it.entries.length,
      })),
      exported_count: items.length,
      skipped_count: 0,
      failed_count: 0,
    }));

    resetWorkbenchStore({
      startExtraction: vi.fn<IpcBridge['startExtraction']>(
        (_taskId, _params, onEvent) =>
          new Promise<ExtractionResultDto>((resolve) => {
            emitBg = onEvent;
            resolveBg = resolve;
          })
      ),
      batchExportSrt: batchExportSpy,
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    await act(async () => {
      await useWorkbenchStore
        .getState()
        .addVideoPaths(['/v/ep01.mkv', '/v/ep02.mkv']);
    });

    const [t1, t2] = useWorkbenchStore.getState().tasks;
    let runPromise!: Promise<void>;
    act(() => {
      runPromise = useWorkbenchStore.getState().startTaskExtraction(t1.id);
    });

    // Switch active task to ep02 while ep01 runs in background
    fireEvent.click(screen.getByText('ep02.mkv'));
    expect(useWorkbenchStore.getState().activeTaskId).toBe(t2.id);

    await waitFor(() => {
      expect(emitBg).toBeTypeOf('function');
    });

    const donePayload = {
      entries: [
        { id: 1, start_ms: 0, end_ms: 1500, text: '后台完成', confidence: 0.94 },
      ],
      metrics: DEFAULT_TEST_METRICS,
    };

    await act(async () => {
      emitBg({ type: 'completed', task_id: `${t1.id}:1`, result: donePayload });
      resolveBg(donePayload);
      await runPromise;
    });

    expect(useWorkbenchStore.getState().tasks[0].status).toBe('ready');

    fireEvent.click(screen.getByRole('button', { name: '批量导出 SRT' }));
    expect(
      screen.getByText('批量导出 SRT 至原片同目录')
    ).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: '开始批量导出' }));
    await waitFor(() => {
      expect(batchExportSpy).toHaveBeenCalledWith(
        [
          expect.objectContaining({
            video_path: '/v/ep01.mkv',
            out_path: '/v/ep01.srt',
          }),
        ],
        'rename'
      );
    });
    expect(screen.getByText(/成功导出 1 个/)).toBeInTheDocument();
  });
});
