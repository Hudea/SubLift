import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { beforeAll, beforeEach, describe, expect, it, vi } from 'vitest';
import type { FramePreviewDto, VideoInfoDto } from '../../core/types';
import { useWorkbenchStore } from '../../store';
import App from '../App';
import {
  installJsdomPolyfills,
  makeTestVideoInfo,
  resetWorkbenchStore,
} from './testHarness';

beforeAll(() => {
  installJsdomPolyfills();
});

describe('PreviewCanvas & RoiOverlay', () => {
  beforeEach(() => {
    Object.defineProperty(window, 'innerWidth', {
      configurable: true,
      writable: true,
      value: 1440,
    });
  });

  it('rejects letterbox black-bar drag starts and allows valid content drag in rect mode', async () => {
    resetWorkbenchStore();
    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv']);
    });

    fireEvent.click(screen.getByRole('button', { name: '自由矩形' }));
    const overlay = screen.getByTestId('roi-stage-overlay');
    const initialBox = useWorkbenchStore.getState().tasks[0].regionBox;

    // 1920x1080 in 800x600 has top letterbox bar [0..75). Pointerdown at y=10 must be ignored!
    fireEvent.pointerDown(overlay, { clientX: 100, clientY: 10 });
    fireEvent.pointerMove(window, { clientX: 400, clientY: 300 });
    fireEvent.pointerUp(window);
    expect(useWorkbenchStore.getState().tasks[0].regionBox).toEqual(initialBox);

    // Drag inside valid video content [0..800] x [75..525]
    fireEvent.pointerDown(overlay, { clientX: 100, clientY: 200 });
    fireEvent.pointerMove(window, { clientX: 500, clientY: 400 });
    fireEvent.pointerUp(window);

    const updatedBox = useWorkbenchStore.getState().tasks[0].regionBox!;
    expect(updatedBox).not.toEqual(initialBox);
    expect(updatedBox.width).toBeGreaterThan(0);
    expect(updatedBox.x + updatedBox.width).toBeLessThanOrEqual(1920);
    expect(updatedBox.y + updatedBox.height).toBeLessThanOrEqual(1080);
  });

  it('disables ROI and extraction fail-closed when display_transform_ok is false', async () => {
    resetWorkbenchStore({
      probeVideo: async () =>
        makeTestVideoInfo(1920, 1080, 60_000, false, '存在非方像素比 SAR'),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });
    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/weird.mkv']);
    });

    expect(screen.getByText('存在非方像素比 SAR')).toBeInTheDocument();
    for (const btn of screen.getAllByRole('button', { name: '自动探测' })) {
      expect(btn).toBeDisabled();
    }
    expect(screen.getByRole('button', { name: '开始提取' })).toBeDisabled();
  });

  it('uses local paddle fallback for detectAutoRegion under VLM and cleans up Tier 2 sublift:// via stopMediaStream', async () => {
    const bridge = resetWorkbenchStore();
    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });
    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv']);
    });

    // Switch to VLM and run auto-detect
    fireEvent.click(screen.getByRole('button', { name: 'VLM' }));
    fireEvent.click(screen.getAllByRole('button', { name: '自动探测' })[0]);

    await waitFor(() => {
      expect(bridge.detectAutoRegion).toHaveBeenCalledWith(
        '/v/ep01.mkv',
        'paddle',
        'auto',
        0.45
      );
    });
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(9500);

    // Toggle Tier 2 playback via Space -> mounts <video src="sublift://...">
    fireEvent.keyDown(window, { code: 'Space', key: ' ' });
    const videoEl = screen.getByTestId('tier2-video-player') as HTMLVideoElement;
    expect(videoEl.getAttribute('src')).toContain('sublift://localhost/media?path=');

    // Step -1s unmounts <video> and calls stopMediaStream()
    fireEvent.click(screen.getByRole('button', { name: '-1s' }));
    await waitFor(() => {
      expect(screen.queryByTestId('tier2-video-player')).toBeNull();
      expect(vi.mocked(bridge.stopMediaStream)).toHaveBeenCalled();
    });
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(8500);

    // Verify 1s/5s button steps and ArrowLeft/Right + Shift+ArrowLeft/Right shortcuts
    fireEvent.click(screen.getByRole('button', { name: '+5s' }));
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(13_500);
    fireEvent.click(screen.getByRole('button', { name: '-5s' }));
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(8500);
    fireEvent.click(screen.getByRole('button', { name: '+1s' }));
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(9500);

    fireEvent.keyDown(window, { key: 'ArrowRight' });
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(10_500);
    fireEvent.keyDown(window, { key: 'ArrowLeft' });
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(9500);
    fireEvent.keyDown(window, { key: 'ArrowRight', shiftKey: true });
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(14_500);
    fireEvent.keyDown(window, { key: 'ArrowLeft', shiftKey: true });
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(9500);
  });

  it('isolates RoiOverlay arrow nudges from stepPreview, stops Tier 2 on ROI/mode interaction, and guards focused controls & open dialogs', async () => {
    const bridge = resetWorkbenchStore();
    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });
    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv']);
    });

    fireEvent.click(screen.getByRole('button', { name: '自由矩形' }));
    act(() => {
      useWorkbenchStore.getState().seekPreview(5000);
    });
    const beforeBox = useWorkbenchStore.getState().tasks[0].regionBox!;

    // Start Tier 2 playback, focus ROI region, and nudge left: must update ROI, stop playback, and NOT stepPreview(-100)
    fireEvent.click(screen.getByRole('button', { name: '播放视频' }));
    expect(useWorkbenchStore.getState().tasks[0].playbackMode).toBe('playing');

    const roiRegion = screen.getByRole('region', { name: '字幕选区 ROI' });
    roiRegion.focus();
    fireEvent.keyDown(roiRegion, { key: 'ArrowLeft' });
    expect(useWorkbenchStore.getState().tasks[0].regionBox!.x).toBe(beforeBox.x); // x was 0, clamped
    fireEvent.keyDown(roiRegion, { key: 'ArrowUp' });
    expect(useWorkbenchStore.getState().tasks[0].regionBox!.y).toBe(beforeBox.y - 1);
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(5000);
    expect(useWorkbenchStore.getState().tasks[0].playbackMode).toBe('still');
    expect(vi.mocked(bridge.stopMediaStream)).toHaveBeenCalled();

    // Switching ROI mode button while playing stops Tier 2 playback
    fireEvent.click(screen.getByRole('button', { name: '播放视频' }));
    expect(useWorkbenchStore.getState().tasks[0].playbackMode).toBe('playing');
    fireEvent.click(screen.getByRole('button', { name: '字幕带' }));
    expect(useWorkbenchStore.getState().tasks[0].playbackMode).toBe('still');

    // Space on focused button or slider must NOT toggle playback; ArrowRight on slider only steps native Slider (+50ms), NOT stepPreview(+100ms)
    const themeBtn = screen.getByRole('button', { name: '切换浅色主题' });
    themeBtn.focus();
    fireEvent.keyDown(themeBtn, { code: 'Space', key: ' ' });
    expect(useWorkbenchStore.getState().tasks[0].playbackMode).toBe('still');

    const slider = screen.getByRole('slider');
    slider.focus();
    fireEvent.keyDown(slider, { key: 'ArrowRight' });
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(5050);

    // When VlmSettingsModal dialog is open, Cmd+B / Cmd+F must be suppressed
    fireEvent.click(screen.getByRole('button', { name: '打开 VLM 设置' }));
    await waitFor(() => {
      expect(screen.getByRole('dialog')).toBeInTheDocument();
    });
    const prevCollapsed = useWorkbenchStore.getState().queueCollapsed;
    fireEvent.keyDown(screen.getByRole('dialog'), { key: 'b', metaKey: true });
    fireEvent.keyDown(screen.getByRole('dialog'), { key: 'f', metaKey: true });
    expect(useWorkbenchStore.getState().queueCollapsed).toBe(prevCollapsed);
    expect(useWorkbenchStore.getState().findReplace.isOpen).toBe(false);
  });

  it('clears stale stillUrl on pending video switch and syncs Tier 2 loadedmetadata seek & timeupdate for native and remux streams', async () => {
    let resolveSecondProbe!: (info: VideoInfoDto) => void;
    const bridge = resetWorkbenchStore({
      probeVideo: vi.fn((path: string): Promise<VideoInfoDto> => {
        if (path.includes('ep02')) {
          return new Promise<VideoInfoDto>((resolve) => {
            resolveSecondProbe = resolve;
          });
        }
        return Promise.resolve(makeTestVideoInfo(1920, 1080, 60_000));
      }),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });
    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mp4']);
    });

    // Wait for Tier 1 still frame on ep01
    await waitFor(() => {
      expect(screen.getByTestId('tier1-still-frame')).toBeInTheDocument();
    });

    // NativeDirect Tier 2 seek & timeupdate: duration === 60 matches probe.duration_ms (60000)
    act(() => {
      useWorkbenchStore.getState().seekPreview(30_000);
    });
    fireEvent.click(screen.getByRole('button', { name: '播放视频' }));
    const nativeVideo = screen.getByTestId('tier2-video-player') as HTMLVideoElement;
    Object.defineProperty(nativeVideo, 'duration', { configurable: true, value: 60 });
    fireEvent.loadedMetadata(nativeVideo);
    expect(nativeVideo.currentTime).toBe(30);

    Object.defineProperty(nativeVideo, 'currentTime', {
      configurable: true,
      writable: true,
      value: 35.25,
    });
    fireEvent.timeUpdate(nativeVideo);
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(35_250);
    expect(useWorkbenchStore.getState().tasks[0].playbackMode).toBe('playing');

    // Remux stream timeupdate: 20s windowed clip (duration=20 vs probe=60000) adds streamBaseMs (30000)
    Object.defineProperty(nativeVideo, 'duration', { configurable: true, value: 20 });
    Object.defineProperty(nativeVideo, 'currentTime', {
      configurable: true,
      writable: true,
      value: 4.5,
    });
    fireEvent.timeUpdate(nativeVideo);
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(34_500);

    // Pause preserves synced playback position
    fireEvent.click(screen.getByRole('button', { name: '暂停播放' }));
    expect(useWorkbenchStore.getState().tasks[0].playbackMode).toBe('still');
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(34_500);
    expect(vi.mocked(bridge.stopMediaStream)).toHaveBeenCalled();

    // Switching to pending ep02 immediately clears prior video's stillUrl before probe resolves
    await waitFor(() => {
      expect(screen.getByTestId('tier1-still-frame')).toBeInTheDocument();
    });
    let addEp02!: Promise<void>;
    act(() => {
      addEp02 = useWorkbenchStore.getState().addVideoPaths(['/v/ep02.mkv']);
    });
    act(() => {
      const ep02 = useWorkbenchStore.getState().tasks[1];
      useWorkbenchStore.getState().selectActiveTask(ep02.id);
    });
    expect(screen.queryByTestId('tier1-still-frame')).toBeNull();

    await act(async () => {
      resolveSecondProbe(makeTestVideoInfo(1920, 1080, 60_000));
      await addEp02;
    });
  });

  it('treats non-finite fMP4 duration and near-EOF mkv duration as remux streams (BUG-7)', async () => {
    resetWorkbenchStore();
    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });
    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep_remux.mkv']);
    });

    // Seek to 30_000ms and enter Tier 2 playback (streamBaseMs = 30_000)
    act(() => {
      useWorkbenchStore.getState().seekPreview(30_000);
    });
    fireEvent.click(screen.getByRole('button', { name: '播放视频' }));
    const mkvVideo = screen.getByTestId('tier2-video-player') as HTMLVideoElement;

    // Case 1: non-finite fMP4 duration (Infinity) -> leaves currentTime at 0, timeupdate adds streamBaseMs
    Object.defineProperty(mkvVideo, 'duration', { configurable: true, value: Infinity });
    Object.defineProperty(mkvVideo, 'currentTime', {
      configurable: true,
      writable: true,
      value: 0,
    });
    fireEvent.loadedMetadata(mkvVideo);
    expect(mkvVideo.currentTime).toBe(0);

    mkvVideo.currentTime = 3.5;
    fireEvent.timeUpdate(mkvVideo);
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(33_500);

    // Case 2: near-EOF .mkv remux reporting duration === probe.duration_ms (60s) -> STILL treated as remux
    Object.defineProperty(mkvVideo, 'duration', { configurable: true, value: 60 });
    mkvVideo.currentTime = 0;
    fireEvent.loadedMetadata(mkvVideo);
    expect(mkvVideo.currentTime).toBe(0);

    mkvVideo.currentTime = 5.25;
    fireEvent.timeUpdate(mkvVideo);
    expect(useWorkbenchStore.getState().tasks[0].previewTimestampMs).toBe(35_250);
  });

  it('discards stale in-flight extractPreviewFrame (full & crop) after switching to unprobed or transform-broken video [HIGH-3]', async () => {
    let resolveFullEp1!: (res: FramePreviewDto) => void;
    let resolveCropEp1!: (res: FramePreviewDto) => void;
    let resolveProbeEp2!: (info: VideoInfoDto) => void;

    resetWorkbenchStore({
      probeVideo: vi.fn((path: string) => {
        if (path.includes('ep02')) {
          return new Promise<VideoInfoDto>((resolve) => {
            resolveProbeEp2 = resolve;
          });
        }
        return Promise.resolve(makeTestVideoInfo(1920, 1080, 60_000));
      }),
      extractPreviewFrame: vi.fn((path: string, _ts: number, crop?: unknown) => {
        if (path.includes('ep01')) {
          return new Promise<FramePreviewDto>((resolve) => {
            if (crop) {
              resolveCropEp1 = resolve;
            } else {
              resolveFullEp1 = resolve;
            }
          });
        }
        return Promise.resolve({
          image_url: 'data:image/png;base64,EP02_VALID',
          width: 640,
          height: 360,
        });
      }),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mp4']);
    });

    // Wait for both full-frame (0ms) and crop-preview (100ms) extractPreviewFrame calls for ep01
    await waitFor(() => {
      expect(resolveFullEp1).toBeTypeOf('function');
      expect(resolveCropEp1).toBeTypeOf('function');
    });

    // Add unprobed ep02 and switch active task BEFORE ep01 promises settle
    let addEp02!: Promise<void>;
    act(() => {
      addEp02 = useWorkbenchStore.getState().addVideoPaths(['/v/ep02.mkv']);
    });
    act(() => {
      const ep02 = useWorkbenchStore.getState().tasks[1];
      useWorkbenchStore.getState().selectActiveTask(ep02.id);
    });

    // Resolve stale ep01 promises -> must NOT overwrite ep02 still frame or crop preview!
    await act(async () => {
      resolveFullEp1({ image_url: 'data:image/png;base64,STALE_EP01', width: 640, height: 360 });
      resolveCropEp1({ image_url: 'data:image/png;base64,STALE_CROP1', width: 320, height: 90 });
    });

    expect(screen.queryByTestId('tier1-still-frame')).toBeNull();
    expect(screen.queryByAltText('字幕选区放大校验')).toBeNull();

    // Even if ep02 resolves with display_transform_ok: false, stale frames remain suppressed
    await act(async () => {
      resolveProbeEp2(makeTestVideoInfo(1920, 1080, 60_000, false, '非方像素比'));
      await addEp02;
    });
    expect(screen.queryByAltText('字幕选区放大校验')).toBeNull();
  });

  it('closes RoiOverlay numeric popover on Escape/Cancel and resets active pointer drag on task switch [LOW-1]', async () => {
    resetWorkbenchStore();
    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv', '/v/ep02.mkv']);
    });

    const [t1, t2] = useWorkbenchStore.getState().tasks;
    const t1InitialBox = { ...t1.regionBox! };
    const t2InitialBox = { ...t2.regionBox! };

    // Open numeric popover, edit Y, press Escape -> closes without mutating ROI
    fireEvent.click(screen.getByTitle('点击手动输入精确源像素坐标'));
    const yInput = screen.getByLabelText('ROI Y') as HTMLInputElement;
    fireEvent.change(yInput, { target: { value: '123' } });
    fireEvent.keyDown(yInput, { key: 'Escape' });
    expect(screen.queryByLabelText('ROI Y')).toBeNull();
    expect(useWorkbenchStore.getState().tasks[0].regionBox).toEqual(t1InitialBox);

    // Reopen popover -> restores activeBox value (not cancelled '123'), Cancel button closes cleanly
    fireEvent.click(screen.getByTitle('点击手动输入精确源像素坐标'));
    expect(screen.getByLabelText('ROI Y')).toHaveValue(t1InitialBox.y);
    fireEvent.click(screen.getByRole('button', { name: '取消坐标输入' }));
    expect(screen.queryByLabelText('ROI Y')).toBeNull();

    // Start pointer drag in rect mode on t1, switch activeTask to t2 mid-drag, then release pointerUp -> t2 must NOT be mutated
    fireEvent.click(screen.getByRole('button', { name: '自由矩形' }));
    const overlay = screen.getByTestId('roi-stage-overlay');
    fireEvent.pointerDown(overlay, { clientX: 120, clientY: 200 });
    fireEvent.pointerMove(window, { clientX: 480, clientY: 360 });

    act(() => {
      useWorkbenchStore.getState().selectActiveTask(t2.id);
    });
    fireEvent.pointerUp(window);

    expect(useWorkbenchStore.getState().tasks[1].regionBox).toEqual(t2InitialBox);
  });
});
