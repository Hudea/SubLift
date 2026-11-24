import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeAll, beforeEach, describe, expect, it, vi } from 'vitest';
import type { ExtractionResultDto, IpcBridge, ReRecognizeResult, TaskEvent } from '../../core/types';
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

describe('ProofreaderPane & ProofreadRow', () => {
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

  it('virtualizes 2,500 cues and seeks canvas to integer midpoint on row click', async () => {
    const cues = Array.from({ length: 2500 }, (_, i) => ({
      id: i + 1,
      start_ms: i * 2000,
      end_ms: i * 2000 + 1001,
      text: `字幕条目 ${i + 1}`,
      confidence: i === 1 ? 0.45 : 0.95,
    }));

    resetWorkbenchStore({
      startExtraction: vi.fn(async (taskId, _params, onEvent) => {
        const res = { entries: cues, metrics: DEFAULT_TEST_METRICS };
        onEvent({ type: 'completed', task_id: taskId, result: res });
        return res;
      }),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });
    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv']);
      await useWorkbenchStore.getState().startTaskExtraction();
    });

    const renderedRows = screen.getAllByTestId(/^proofread-row-/);
    expect(renderedRows.length).toBeGreaterThan(0);
    expect(renderedRows.length).toBeLessThan(50);

    fireEvent.click(renderedRows[1]);
    // Cue #2: start=2000, end=3001 -> Math.floor((2000 + 3001) / 2) = 2500
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(2500);
  });

  it('enforces inline validation, IME compositionend guard, dirty edit shortcut lock, UTF-16 split, low-conf VLM re-OCR, and apply_merge: false export', async () => {
    const bridge = resetWorkbenchStore({
      startExtraction: vi.fn(async (taskId, _params, onEvent) => {
        const res = {
          entries: [
            { id: 1, start_ms: 1000, end_ms: 4000, text: 'a😀', confidence: 0.92 },
            { id: 2, start_ms: 5000, end_ms: 7000, text: '[待补录]', confidence: 0.0 },
          ],
          metrics: DEFAULT_METRICS_SAFE,
        };
        onEvent({ type: 'completed', task_id: taskId, result: res });
        return res;
      }),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });
    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv']);
      await useWorkbenchStore.getState().startTaskExtraction();
    });

    const firstRow = screen.getByTestId('proofread-row-cue_1_0');
    fireEvent.doubleClick(screen.getByText('a😀'));

    const textarea = screen.getByLabelText('编辑第 1 条字幕文本') as HTMLTextAreaElement;
    expect(useWorkbenchStore.getState().isDirtyInlineEdit).toBe(true);

    // Dirty inline edit locks Space / ArrowRight / Export / Undo
    fireEvent.keyDown(window, { code: 'Space', key: ' ' });
    expect(useWorkbenchStore.getState().tasks[0].playbackMode).toBe('still');
    for (const exportBtn of screen.getAllByRole('button', { name: '导出 SRT' })) {
      expect(exportBtn).toBeDisabled();
    }

    // IME trap: compositionstart -> compositionend -> immediate Enter must NOT submit
    fireEvent.compositionStart(textarea);
    fireEvent.compositionEnd(textarea);
    fireEvent.keyDown(textarea, { key: 'Enter', isComposing: false });
    expect(screen.getByLabelText('编辑第 1 条字幕文本')).toBeInTheDocument();

    // keyCode 229 must NOT submit
    fireEvent.keyDown(textarea, { key: 'Enter', keyCode: 229 });
    expect(screen.getByLabelText('编辑第 1 条字幕文本')).toBeInTheDocument();

    // Timecode validation: end <= start blocks submit
    const endInput = screen.getByLabelText('结束时间');
    fireEvent.change(endInput, { target: { value: '00:00:00,500' } });
    expect(screen.getByText(/结束时间必须大于开始时间/)).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '提交 (Enter)' })).toBeDisabled();

    // Restore valid end time and cancel or split
    fireEvent.change(endInput, { target: { value: '00:00:04,000' } });
    textarea.setSelectionRange(2, 2); // surrogate middle in "a😀"

    // Wait for 30ms IME window to clear then trigger Cmd+Enter split
    await act(async () => {
      await new Promise((r) => setTimeout(r, 45));
    });
    fireEvent.keyDown(textarea, { key: 'Enter', metaKey: true });

    await waitFor(() => {
      expect(useWorkbenchStore.getState().tasks[0].cues).toHaveLength(3);
    });
    expect(useWorkbenchStore.getState().tasks[0].cues[0].text).toBe('a');
    expect(useWorkbenchStore.getState().tasks[0].cues[1].text).toBe('😀');

    // Undo restores original single cue
    fireEvent.keyDown(window, { key: 'z', metaKey: true });
    expect(useWorkbenchStore.getState().tasks[0].cues).toHaveLength(2);
    expect(firstRow).toBeInTheDocument();

    // Low-conf VLM re-recognize
    fireEvent.click(screen.getByRole('button', { name: /复核全部低置信度 \(1\)/ }));
    await waitFor(() => {
      expect(bridge.reRecognizeEntries).toHaveBeenCalled();
      expect(useWorkbenchStore.getState().tasks[0].cues[1].text).toBe('VLM 修正');
    });

    // Export SRT passes apply_merge: false
    fireEvent.click(screen.getAllByRole('button', { name: '导出 SRT' })[0]);
    await waitFor(() => {
      expect(bridge.exportSrt).toHaveBeenCalledWith(
        '/tmp/ep01.srt',
        expect.any(Array),
        false
      );
    });
  });

  it('prevents failed-split deadlock, persists inline ±100ms nudges on commit, locks TransportBar & dual-row edits, and clears dirty state on unmount', async () => {
    const cues = Array.from({ length: 120 }, (_, i) => ({
      id: i + 1,
      start_ms: 1000 + i * 3000,
      end_ms: 3000 + i * 3000,
      text: i === 0 ? '第一条字幕' : `字幕条目 ${i + 1}`,
      confidence: i === 50 ? 0.35 : 0.95,
    }));

    resetWorkbenchStore({
      startExtraction: vi.fn(async (taskId, _params, onEvent) => {
        const res = { entries: cues, metrics: DEFAULT_METRICS_SAFE };
        onEvent({ type: 'completed', task_id: taskId, result: res });
        return res;
      }),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });
    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv']);
      await useWorkbenchStore.getState().startTaskExtraction();
    });

    // Open Row 1 inline edit
    fireEvent.doubleClick(screen.getByText('第一条字幕'));
    const textarea = screen.getByLabelText('编辑第 1 条字幕文本') as HTMLTextAreaElement;
    expect(useWorkbenchStore.getState().isDirtyInlineEdit).toBe(true);
    expect(useWorkbenchStore.getState().editingCueUid).toBe('cue_1_0');

    // TransportBar Play/Pause, -5s/-1s/+1s/+5s, and timeline Slider are disabled during dirty inline edit
    expect(screen.getByRole('button', { name: '播放视频' })).toBeDisabled();
    expect(screen.getByRole('button', { name: '-5s' })).toBeDisabled();
    expect(screen.getByRole('button', { name: '-1s' })).toBeDisabled();
    expect(screen.getByRole('button', { name: '+1s' })).toBeDisabled();
    expect(screen.getByRole('button', { name: '+5s' })).toBeDisabled();
    expect(screen.getByRole('slider')).toHaveAttribute('data-disabled');

    // Single-active-row lock: double-clicking Row 2 does NOT open a second editor
    fireEvent.doubleClick(screen.getByText('字幕条目 2'));
    expect(screen.queryByLabelText('编辑第 2 条字幕文本')).toBeNull();

    // Failed split at offset 0 keeps editor open with draftText intact
    fireEvent.change(textarea, { target: { value: '重要未保存文本' } });
    textarea.setSelectionRange(0, 0);
    const splitBtn = screen.getByRole('button', { name: '在此拆分 (⌘Enter)' });
    splitBtn.focus();
    fireEvent.click(splitBtn);
    expect(screen.getByLabelText('编辑第 1 条字幕文本')).toHaveValue('重要未保存文本');
    expect(screen.getAllByText(/拆分后两侧文本均不能为空/).length).toBeGreaterThanOrEqual(1);
    expect(useWorkbenchStore.getState().isDirtyInlineEdit).toBe(true);

    // Pressing Escape directly while focus remains on split button closes editor cleanly (BUG-1b)
    fireEvent.keyDown(splitBtn, { key: 'Escape' });
    expect(screen.queryByLabelText('编辑第 1 条字幕文本')).toBeNull();
    expect(useWorkbenchStore.getState().isDirtyInlineEdit).toBe(false);
    expect(useWorkbenchStore.getState().editingCueUid).toBeNull();
    expect(screen.getByRole('button', { name: '播放视频' })).toBeEnabled();

    // Also verify 取消 (Esc) button discards cleanly and unlocks
    fireEvent.doubleClick(screen.getByText('第一条字幕'));
    expect(useWorkbenchStore.getState().isDirtyInlineEdit).toBe(true);
    fireEvent.click(screen.getByRole('button', { name: '取消 (Esc)' }));
    expect(useWorkbenchStore.getState().isDirtyInlineEdit).toBe(false);
    expect(useWorkbenchStore.getState().editingCueUid).toBeNull();
    expect(screen.getByRole('button', { name: '播放视频' })).toBeEnabled();

    // Inline -100ms start nudge and +100ms end nudge persist through 提交 (Enter)
    fireEvent.doubleClick(screen.getByText('第一条字幕'));
    fireEvent.click(screen.getByRole('button', { name: '开始时间 -100ms' }));
    fireEvent.click(screen.getByRole('button', { name: '结束时间 +100ms' }));
    expect(screen.getByLabelText('开始时间')).toHaveValue('00:00:00,900');
    expect(screen.getByLabelText('结束时间')).toHaveValue('00:00:03,100');
    fireEvent.click(screen.getByRole('button', { name: '提交 (Enter)' }));
    expect(useWorkbenchStore.getState().tasks[0].cues[0].start_ms).toBe(900);
    expect(useWorkbenchStore.getState().tasks[0].cues[0].end_ms).toBe(3100);
    expect(useWorkbenchStore.getState().isDirtyInlineEdit).toBe(false);

    // Unmounting an active ProofreadRow via toggling 仅看存疑 clears isDirtyInlineEdit
    fireEvent.doubleClick(screen.getByText('第一条字幕'));
    expect(useWorkbenchStore.getState().isDirtyInlineEdit).toBe(true);
    fireEvent.click(screen.getByRole('button', { name: /仅看存疑 \(1\)/ }));
    await waitFor(() => {
      expect(screen.queryByLabelText('编辑第 1 条字幕文本')).toBeNull();
      expect(useWorkbenchStore.getState().isDirtyInlineEdit).toBe(false);
      expect(useWorkbenchStore.getState().editingCueUid).toBeNull();
    });

    // Cmd+F while Find/Replace is already open re-focuses and selects search input
    fireEvent.click(screen.getByRole('button', { name: /仅看存疑 \(1\)/ }));
    fireEvent.keyDown(window, { key: 'f', metaKey: true });
    const findInput = await screen.findByLabelText('查找关键词') as HTMLInputElement;
    fireEvent.change(findInput, { target: { value: '字幕' } });
    (screen.getByRole('button', { name: '切换浅色主题' })).focus();
    fireEvent.keyDown(window, { key: 'f', metaKey: true });
    await waitFor(() => {
      expect(document.activeElement).toBe(findInput);
      expect(findInput.selectionStart).toBe(0);
      expect(findInput.selectionEnd).toBe(2);
    });
  });

  it('seeks canvas to streaming cue midpoint on click during extracting', async () => {
    let emitEvent!: (ev: {
      type: 'subtitle_entry';
      task_id: string;
      entry: { id: number; start_ms: number; end_ms: number; text: string; confidence: number };
    }) => void;
    let resolveExtract!: (res: ExtractionResultDto) => void;

    resetWorkbenchStore({
      startExtraction: vi.fn<IpcBridge['startExtraction']>(
        (taskId, _params, onEvent) =>
          new Promise<ExtractionResultDto>((resolve) => {
            emitEvent = (ev) => onEvent({ ...ev, task_id: taskId });
            resolveExtract = resolve;
          })
      ),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });
    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv']);
    });

    let runPromise!: Promise<void>;
    act(() => {
      runPromise = useWorkbenchStore.getState().startTaskExtraction();
    });
    await waitFor(() => {
      expect(emitEvent).toBeTypeOf('function');
    });
    act(() => {
      emitEvent({
        type: 'subtitle_entry',
        task_id: 'task_1:1',
        entry: {
          id: 42,
          start_ms: 10_000,
          end_ms: 14_001,
          text: '流式预览字幕',
          confidence: 0.91,
        },
      });
    });

    const streamRow = await screen.findByTestId('proofread-row-stream_42_0');
    fireEvent.click(streamRow);
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(12_000);

    await act(async () => {
      resolveExtract({ entries: [], metrics: DEFAULT_METRICS_SAFE });
      await runPromise;
    });
  });

  it('auto-resets onlyLowConf filter when resolving last low-conf cue or switching to task with 0 low-conf cues [HIGH-1]', async () => {
    let extractCall = 0;
    resetWorkbenchStore({
      startExtraction: vi.fn(async (taskId, _params, onEvent) => {
        extractCall += 1;
        const res =
          extractCall === 1
            ? {
                entries: [
                  { id: 1, start_ms: 1000, end_ms: 2000, text: '清晰字幕一', confidence: 0.96 },
                  { id: 2, start_ms: 3000, end_ms: 4000, text: '[待补录]', confidence: 0.0 },
                ],
                metrics: DEFAULT_METRICS_SAFE,
              }
            : {
                entries: [
                  { id: 1, start_ms: 1000, end_ms: 2000, text: '第二集全部清晰', confidence: 0.98 },
                ],
                metrics: DEFAULT_METRICS_SAFE,
              };
        onEvent({ type: 'completed', task_id: taskId, result: res });
        return res;
      }),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });
    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv', '/v/ep02.mkv']);
      await useWorkbenchStore.getState().startTaskExtraction(useWorkbenchStore.getState().tasks[0].id);
      await useWorkbenchStore.getState().startTaskExtraction(useWorkbenchStore.getState().tasks[1].id);
    });

    // Enable 仅看存疑 on ep01 -> hides 清晰字幕一
    const lowConfToggle = screen.getByRole('button', { name: /仅看存疑 \(1\)/ });
    fireEvent.click(lowConfToggle);
    expect(screen.queryByText('清晰字幕一')).toBeNull();
    expect(screen.getByText('[待补录]')).toBeInTheDocument();

    // Switching to ep02 (which has 0 low-conf cues) must auto-reset onlyLowConf instead of stranding on empty message
    fireEvent.click(screen.getByText('ep02.mkv'));
    await waitFor(() => {
      expect(screen.getByText('第二集全部清晰')).toBeInTheDocument();
      expect(screen.queryByText('没有符合条件的存疑条目')).toBeNull();
    });

    // Switch back to ep01, re-enable 仅看存疑, then resolve the last low-conf cue via VLM re-OCR -> auto-resets and shows all cues
    fireEvent.click(screen.getByText('ep01.mkv'));
    fireEvent.click(screen.getByRole('button', { name: /仅看存疑 \(1\)/ }));
    expect(screen.queryByText('清晰字幕一')).toBeNull();

    fireEvent.click(screen.getByRole('button', { name: /复核全部低置信度 \(1\)/ }));
    await waitFor(() => {
      expect(screen.getByText('清晰字幕一')).toBeInTheDocument();
      expect(screen.getByText('VLM 修正')).toBeInTheDocument();
      expect(screen.queryByText('没有符合条件的存疑条目')).toBeNull();
    });
  });

  it('locks ProofreadRow inline edit, Replace buttons during isReRecognizing [HIGH-2], combines time+text commitEdit into single Undo [MEDIUM-3], and guards Escape during IME composition [LOW-1]', async () => {
    let resolveReRec!: (res: ReRecognizeResult[]) => void;

    resetWorkbenchStore({
      startExtraction: vi.fn(async (taskId, _params, onEvent) => {
        const res = {
          entries: [
            { id: 1, start_ms: 1000, end_ms: 3000, text: '你好世界', confidence: 0.96 },
            { id: 2, start_ms: 4000, end_ms: 6000, text: '[待补录]', confidence: 0.0 },
          ],
          metrics: DEFAULT_METRICS_SAFE,
        };
        onEvent({ type: 'completed', task_id: taskId, result: res });
        return res;
      }),
      reRecognizeEntries: vi.fn(
        () =>
          new Promise<ReRecognizeResult[]>((resolve) => {
            resolveReRec = resolve;
          })
      ),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });
    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv']);
      await useWorkbenchStore.getState().startTaskExtraction();
    });

    // [MEDIUM-3] Edit BOTH timecode and text in one commitEdit -> single Undo reverts both!
    fireEvent.doubleClick(screen.getByText('你好世界'));
    const startInput = screen.getByLabelText('开始时间');
    const textarea = screen.getByLabelText('编辑第 1 条字幕文本') as HTMLTextAreaElement;

    // [LOW-1] IME compositionstart -> Escape must NOT discard inline edit while composing
    fireEvent.compositionStart(textarea);
    fireEvent.keyDown(textarea, { key: 'Escape' });
    expect(screen.getByLabelText('编辑第 1 条字幕文本')).toBeInTheDocument();
    fireEvent.compositionEnd(textarea);

    fireEvent.change(startInput, { target: { value: '00:00:01,500' } });
    fireEvent.change(textarea, { target: { value: '改字文本' } });
    fireEvent.click(screen.getByRole('button', { name: '提交 (Enter)' }));

    expect(useWorkbenchStore.getState().tasks[0].cues[0].start_ms).toBe(1500);
    expect(useWorkbenchStore.getState().tasks[0].cues[0].text).toBe('改字文本');

    // Single Undo call reverts BOTH start_ms AND text
    fireEvent.click(screen.getByRole('button', { name: '撤销' }));
    expect(useWorkbenchStore.getState().tasks[0].cues[0].start_ms).toBe(1000);
    expect(useWorkbenchStore.getState().tasks[0].cues[0].text).toBe('你好世界');

    // [HIGH-2] Open Find & Replace, check Replace disabled during dirty inline edit, then during isReRecognizing
    fireEvent.keyDown(window, { key: 'h', ctrlKey: true });
    const findInput = await screen.findByLabelText('查找关键词');
    fireEvent.change(findInput, { target: { value: '你好' } });
    const replaceBtn = screen.getByRole('button', { name: '替换' });
    const replaceAllBtn = screen.getByRole('button', { name: '全部替换' });
    expect(replaceBtn).toBeEnabled();
    expect(replaceAllBtn).toBeEnabled();

    // Dirty inline edit disables Replace & Replace All
    fireEvent.doubleClick(screen.getByText('世界'));
    expect(replaceBtn).toBeDisabled();
    expect(replaceAllBtn).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: '取消 (Esc)' }));
    expect(replaceBtn).toBeEnabled();

    // Start VLM re-recognition -> ProofreadRow becomes read-only, Undo/Redo/Replace disabled
    fireEvent.click(screen.getByRole('button', { name: /复核全部低置信度 \(1\)/ }));
    expect(useWorkbenchStore.getState().tasks[0].isReRecognizing).toBe(true);

    // Double-clicking row 1 must NOT open inline editor while isReRecognizing
    fireEvent.doubleClick(screen.getByText('世界'));
    expect(screen.queryByLabelText('编辑第 1 条字幕文本')).toBeNull();
    expect(screen.getByRole('button', { name: '替换' })).toBeDisabled();
    expect(screen.getByRole('button', { name: '全部替换' })).toBeDisabled();
    expect(screen.getByRole('button', { name: '撤销' })).toBeDisabled();

    await act(async () => {
      resolveReRec([
        {
          cue_id: 2,
          ok: true,
          text: '重识完成',
          category: 'dialogue',
          confidence: 0.98,
        },
      ]);
    });
    await waitFor(() => {
      expect(useWorkbenchStore.getState().tasks[0].isReRecognizing).toBe(false);
      expect(screen.getByText('重识完成')).toBeInTheDocument();
    });
  });

  it('displays live extraction progress banner (fps/multiplier/sub-phase), post-run metrics summary, and auto-dismissing completion toast', async () => {
    let emitEvent!: (ev: TaskEvent) => void;
    let resolveExtract!: (res: ExtractionResultDto) => void;

    resetWorkbenchStore({
      startExtraction: vi.fn<IpcBridge['startExtraction']>(
        (taskId, _params, onEvent) =>
          new Promise<ExtractionResultDto>((resolve) => {
            emitEvent = (ev) => onEvent({ ...ev, task_id: taskId });
            resolveExtract = resolve;
          })
      ),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });
    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv']);
    });

    vi.useFakeTimers();

    let runPromise!: Promise<void>;
    act(() => {
      runPromise = useWorkbenchStore.getState().startTaskExtraction();
    });

    await act(async () => {
      await Promise.resolve();
      await Promise.resolve();
    });

    expect(emitEvent).toBeTypeOf('function');
    const banner = screen.getByTestId('active-extraction-progress');
    expect(banner).toHaveTextContent('探测媒体中…');

    act(() => {
      emitEvent({ type: 'phase', task_id: 'task_1:1', sub_phase: 'timing' });
      emitEvent({
        type: 'progress',
        task_id: 'task_1:1',
        current_ms: 30_000,
        total_ms: 60_000,
        fps: 25,
      });
      emitEvent({
        type: 'subtitle_entry',
        task_id: 'task_1:1',
        entry: { id: 1, start_ms: 1000, end_ms: 2000, text: '第一句', confidence: 0.95 },
      });
    });

    expect(banner).toHaveTextContent('打轴与识别中…');
    expect(banner).toHaveTextContent('50% · 25 fps · 5.0× · 已出 1 条');

    await act(async () => {
      const doneRes: ExtractionResultDto = {
        entries: [
          { id: 1, start_ms: 1000, end_ms: 2000, text: '第一句', confidence: 0.95 },
        ],
        metrics: {
          total_frames_processed: 300,
          total_segments_detected: 1,
          total_ocr_calls: 1,
          total_entries_produced: 1,
          wall_time_ms: 1200,
          effective_fps: 250,
        },
      };
      emitEvent({ type: 'completed', task_id: 'task_1:1', result: doneRes });
      resolveExtract(doneRes);
      await runPromise;
    });

    expect(screen.queryByTestId('active-extraction-progress')).toBeNull();
    const summary = screen.getByTestId('extraction-metrics-summary');
    expect(summary).toHaveTextContent('提取完成 · 耗时 1.2s · 平均 250 fps (50.0×) · 有效字幕 1 条');
    expect(screen.getByText('提取完成：共 1 条字幕')).toBeInTheDocument();

    // Completion toast auto-dismisses after 4500ms
    await act(async () => {
      await vi.advanceTimersByTimeAsync(4600);
    });
    expect(screen.queryByText('提取完成：共 1 条字幕')).toBeNull();
  });
});

const DEFAULT_METRICS_SAFE = DEFAULT_TEST_METRICS;
