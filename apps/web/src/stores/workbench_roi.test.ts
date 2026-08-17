import { describe, it, expect, beforeEach, vi } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import { useWorkbenchStore } from './workbench';
import { SubLiftApiClient } from '../api/client';

describe('Workbench Auto ROI Detection (Feature 12509)', () => {
  beforeEach(() => {
    setActivePinia(createPinia());
    vi.restoreAllMocks();
  });

  it('updates regionBox and feedback when detection succeeds', async () => {
    const store = useWorkbenchStore();

    vi.spyOn(SubLiftApiClient, 'detectSubtitleRegion').mockResolvedValueOnce({
      detected: true,
      sample_time_s: 5.2,
      suggested_box: { x: 0.0, y: 0.78, width: 1.0, height: 0.16 },
      preview_text: '测试字幕预览',
      confidence: 0.95,
      total_candidates: 2,
    });

    store.loadVideo('/tmp/test_video.mp4');
    expect(store.isDetectingRegion).toBe(true);

    // Wait for async autoDetectSubtitleRegion to resolve
    await vi.waitFor(() => expect(store.isDetectingRegion).toBe(false));

    expect(store.regionBox.y).toBe(0.78);
    expect(store.regionBox.height).toBe(0.16);
    expect(store.roiDetectionFeedback).toContain('测试字幕预览');
  });

  it('preserves manual box in silent auto-detect when roiModifiedByUser is true', async () => {
    const store = useWorkbenchStore();

    let resolvePromise: (val: any) => void;
    const promise = new Promise((resolve) => {
      resolvePromise = resolve;
    });
    vi.spyOn(SubLiftApiClient, 'detectSubtitleRegion').mockReturnValueOnce(promise as any);

    store.loadVideo('/tmp/test_video.mp4');
    expect(store.isDetectingRegion).toBe(true);

    // User manually modifies the ROI box before silent response returns
    store.roiModifiedByUser = true;
    store.updateRegionBox({ x: 0.1, y: 0.5, width: 0.8, height: 0.2 });

    // Response arrives
    resolvePromise!({
      detected: true,
      sample_time_s: 5.2,
      suggested_box: { x: 0.0, y: 0.78, width: 1.0, height: 0.16 },
      preview_text: '测试字幕预览',
      confidence: 0.95,
      total_candidates: 2,
    });

    await vi.waitFor(() => expect(store.isDetectingRegion).toBe(false));

    // Must NOT overwrite the user's manual box
    expect(store.regionBox.x).toBe(0.1);
    expect(store.regionBox.y).toBe(0.5);
  });

  it('discards late responses when video is switched or reset (generation counter)', async () => {
    const store = useWorkbenchStore();

    let resolveFirst: (val: any) => void;
    const firstPromise = new Promise((resolve) => {
      resolveFirst = resolve;
    });

    vi.spyOn(SubLiftApiClient, 'detectSubtitleRegion').mockReturnValueOnce(firstPromise as any);

    // Video 1 loaded
    store.loadVideo('/tmp/video1.mp4');

    // Switch to Video 2 immediately
    vi.spyOn(SubLiftApiClient, 'detectSubtitleRegion').mockResolvedValueOnce({
      detected: true,
      sample_time_s: 1.0,
      suggested_box: { x: 0.0, y: 0.85, width: 1.0, height: 0.12 },
      preview_text: 'Video 2 Subtitle',
      confidence: 0.98,
      total_candidates: 1,
    });
    store.loadVideo('/tmp/video2.mp4');

    // Video 1 late response arrives
    resolveFirst!({
      detected: true,
      sample_time_s: 99.0,
      suggested_box: { x: 0.0, y: 0.40, width: 1.0, height: 0.50 },
      preview_text: 'Old Video 1',
      confidence: 0.5,
      total_candidates: 1,
    });

    await vi.waitFor(() => expect(store.isDetectingRegion).toBe(false));

    // Must have applied Video 2's box, not the stale Video 1
    expect(store.regionBox.y).toBe(0.85);
    expect(store.regionBox.height).toBe(0.12);
  });

  it('gracefully handles detection failure without destroying existing box', async () => {
    const store = useWorkbenchStore();

    vi.spyOn(SubLiftApiClient, 'detectSubtitleRegion').mockRejectedValueOnce(new Error('Network error'));

    store.loadVideo('/tmp/test_video.mp4');
    await vi.waitFor(() => expect(store.isDetectingRegion).toBe(false));

    // Default bottom 30% remains intact
    expect(store.regionBox.y).toBe(0.7);
    expect(store.regionBox.height).toBe(0.3);
  });
});
