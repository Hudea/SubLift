export type MediaPlatformHost = 'standard' | 'windows';

/**
 * 集中构造 Tier 2 sublift:// 媒体播放地址，禁止在 UI 组件散落硬编码字符串拼接。
 */
export function buildMediaUrl(
  videoPath: string,
  startSec?: number,
  platformHost: MediaPlatformHost = 'standard'
): string {
  const base =
    platformHost === 'windows'
      ? 'http://sublift.localhost/media?path='
      : 'sublift://localhost/media?path=';
  const encodedPath = encodeURIComponent(videoPath);
  if (startSec !== undefined && Number.isFinite(startSec) && startSec > 0) {
    return `${base}${encodedPath}&t=${startSec.toFixed(3)}`;
  }
  return `${base}${encodedPath}`;
}
