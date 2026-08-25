import type { OcrEngineName, NormalizedRegionBox } from './api';

/**
 * 08308 / 12513 对齐：ROI 提取策略契约
 * - auto: 逐视频独立智能识别字幕区域（preparing 阶段探测）
 * - fixed: 使用用户设定的固定选区（全量应用指定 region_box）
 * - default: 明确使用默认底边 30% 区域（标准原生流水线锚点）
 */
export type RoiPolicy = 'auto' | 'fixed' | 'default';

/**
 * 采样质量 3 档分级（UI 统一展示中文名与基准 FPS）
 */
export type SamplingQuality = 'fast' | 'balanced' | 'fine';

export const SAMPLING_QUALITY_MAP: Record<
  SamplingQuality,
  { fps: number; label: string; desc: string }
> = {
  fast: { fps: 5.0, label: '快速', desc: '5 FPS 采样，更快完成，适合常规字幕' },
  balanced: { fps: 8.0, label: '平衡', desc: '8 FPS 采样，速度与短字幕召回更均衡' },
  fine: { fps: 12.0, label: '精细', desc: '12 FPS 采样，更高采样密度' },
};

export const QUALITY_FPS_MAP: Record<SamplingQuality, number> = {
  fast: 5.0,
  balanced: 8.0,
  fine: 12.0,
};

export const DEFAULT_BOTTOM_ROI: Readonly<NormalizedRegionBox> = Object.freeze({
  x: 0.0,
  y: 0.7,
  width: 1.0,
  height: 0.3,
});

export const DEFAULT_CONFIDENCE_THRESHOLD = 0.0;

/**
 * Feature 12513: 单视频与批量提取统一配置 DTO
 */
export interface ExtractionConfig {
  engine: OcrEngineName;
  quality: SamplingQuality;
  confidence_threshold: number;
  roi_policy: RoiPolicy;
  region_box?: NormalizedRegionBox | null;
  script?: string;
}

/**
 * 默认配置真源工厂函数
 */
export function createDefaultExtractionConfig(
  engine: OcrEngineName = 'vision',
  overrides?: Partial<ExtractionConfig>
): ExtractionConfig {
  return {
    engine,
    quality: 'fast',
    confidence_threshold: DEFAULT_CONFIDENCE_THRESHOLD,
    roi_policy: 'auto',
    region_box: null,
    ...overrides,
  };
}

export function qualityToFps(quality: SamplingQuality): number {
  return SAMPLING_QUALITY_MAP[quality]?.fps ?? 5.0;
}

export function fpsToQuality(fps: number): SamplingQuality {
  if (fps >= 10.0) return 'fine';
  if (fps >= 6.5) return 'balanced';
  return 'fast';
}
