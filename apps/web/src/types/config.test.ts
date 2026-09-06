import { describe, it, expect, beforeEach } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import {
  SAMPLING_QUALITY_MAP,
  QUALITY_FPS_MAP,
  DEFAULT_BOTTOM_ROI,
  DEFAULT_CONFIDENCE_THRESHOLD,
  createDefaultExtractionConfig,
  qualityToFps,
  fpsToQuality,
} from './config';
import { useWorkbenchStore } from '../stores/workbench';
import { useBatchStore } from '../stores/batch';
import { useSystemStore } from '../stores/system';

describe('Shared Config DTO & Defaults Truth Source (Feature 12513)', () => {
  beforeEach(() => {
    setActivePinia(createPinia());
  });

  it('provides standardized sampling quality FPS mapping and descriptions', () => {
    expect(SAMPLING_QUALITY_MAP.fast.fps).toBe(5.0);
    expect(SAMPLING_QUALITY_MAP.fast.label).toBe('快速');
    expect(QUALITY_FPS_MAP.fast).toBe(5.0);

    expect(SAMPLING_QUALITY_MAP.balanced.fps).toBe(8.0);
    expect(SAMPLING_QUALITY_MAP.balanced.label).toBe('平衡');
    expect(QUALITY_FPS_MAP.balanced).toBe(8.0);

    expect(SAMPLING_QUALITY_MAP.fine.fps).toBe(12.0);
    expect(SAMPLING_QUALITY_MAP.fine.label).toBe('精细');
    expect(QUALITY_FPS_MAP.fine).toBe(12.0);
  });

  it('converts between quality enum and target FPS bidirectionally', () => {
    expect(qualityToFps('fast')).toBe(5.0);
    expect(qualityToFps('balanced')).toBe(8.0);
    expect(qualityToFps('fine')).toBe(12.0);

    expect(fpsToQuality(5.0)).toBe('fast');
    expect(fpsToQuality(4.0)).toBe('fast');
    expect(fpsToQuality(8.0)).toBe('balanced');
    expect(fpsToQuality(7.0)).toBe('balanced');
    expect(fpsToQuality(12.0)).toBe('fine');
    expect(fpsToQuality(15.0)).toBe('fine');
  });

  it('creates default extraction config with unified truth source constants', () => {
    const config = createDefaultExtractionConfig('vision');
    expect(config.engine).toBe('vision');
    expect(config.quality).toBe('fast');
    expect(config.confidence_threshold).toBe(DEFAULT_CONFIDENCE_THRESHOLD);
    expect(config.roi_policy).toBe('auto');
    expect(config.region_box).toBeNull();
    expect(config.script).toBeUndefined();

    // Overrides
    const custom = createDefaultExtractionConfig('paddle', {
      quality: 'fine',
      confidence_threshold: 0.5,
      roi_policy: 'fixed',
      region_box: { x: 0.1, y: 0.8, width: 0.8, height: 0.2 },
      script: 'Hans',
    });
    expect(custom.engine).toBe('paddle');
    expect(custom.quality).toBe('fine');
    expect(custom.confidence_threshold).toBe(0.5);
    expect(custom.roi_policy).toBe('fixed');
    expect(custom.region_box).toEqual({ x: 0.1, y: 0.8, width: 0.8, height: 0.2 });
    expect(custom.script).toBe('Hans');
  });

  it('guarantees default configuration schema and equality across workbench and batch stores', () => {
    const systemStore = useSystemStore();
    systemStore.systemInfo = {
      version: 'v0.1.0',
      runtime: 'cpp',
      capabilities: [],
      engines: [
        { name: 'vision', available: true, detail: 'Apple Vision' },
        { name: 'paddle', available: true, detail: 'PaddleOCR' },
      ],
      ffmpeg: { available: true },
    };

    const workbenchStore = useWorkbenchStore();
    const batchStore = useBatchStore();

    // Preferred engine is vision
    expect(workbenchStore.selectedEngine).toBe('vision');
    expect(workbenchStore.quality).toBe('fast');
    expect(workbenchStore.targetFps).toBe(5.0);
    expect(workbenchStore.confidenceThreshold).toBe(DEFAULT_CONFIDENCE_THRESHOLD);
    expect(workbenchStore.roiPolicy).toBe('auto');
    expect(workbenchStore.regionBox).toEqual(DEFAULT_BOTTOM_ROI);

    const batchDef = batchStore.defaultBatchConfig;
    expect(batchDef.engine).toBe('vision');
    expect(batchDef.quality).toBe('fast');
    expect(batchDef.confidence_threshold).toBe(DEFAULT_CONFIDENCE_THRESHOLD);
    expect(batchDef.roi_policy).toBe('auto');
    expect(batchDef.region_box).toBeNull();

    // Dynamic preference shift to paddle
    systemStore.systemInfo.engines[0].available = false; // vision unavailable
    expect(systemStore.preferredEngine).toBe('paddle');

    const updatedBatchDef = batchStore.defaultBatchConfig;
    expect(updatedBatchDef.engine).toBe('paddle');

    // Paddle 也不可用时仍保持产品默认 paddle，不得切换 mock（ADR-0038）
    systemStore.systemInfo.engines[1].available = false;
    systemStore.systemInfo.engines.push({ name: 'mock', available: true, detail: 'Mock' });
    expect(systemStore.preferredEngine).toBe('paddle');
    expect(batchStore.defaultBatchConfig.engine).toBe('paddle');
  });
});
