import { ref, shallowRef, computed, onUnmounted } from 'vue';
import { formatTimecode } from '../utils/timecode';

export interface VideoMetadata {
  duration: number;
  videoWidth: number;
  videoHeight: number;
  aspectRatio: number;
}

export function useVideoPlayer() {
  const videoElement = shallowRef<HTMLVideoElement | null>(null);

  const isPlaying = ref(false);
  const isBuffering = ref(false);
  const isSeeking = ref(false);
  const currentTime = ref(0);
  const duration = ref(0);
  const isReady = ref(false);
  const error = ref<string | null>(null);

  const metadata = ref<VideoMetadata>({
    duration: 0,
    videoWidth: 0,
    videoHeight: 0,
    aspectRatio: 16 / 9,
  });

  const currentTimecode = computed(() => formatTimecode(currentTime.value));
  const durationTimecode = computed(() => formatTimecode(duration.value));
  const progressRatio = computed(() => (duration.value > 0 ? currentTime.value / duration.value : 0));

  let rafId: number | null = null;
  let lastSyncTimestamp = 0;
  const SYNC_INTERVAL_MS = 100; // 10Hz clock

  const syncTimeLoop = (now: DOMHighResTimeStamp) => {
    const el = videoElement.value;
    if (el && !el.paused && !isSeeking.value) {
      if (now - lastSyncTimestamp >= SYNC_INTERVAL_MS) {
        currentTime.value = el.currentTime;
        lastSyncTimestamp = now;
      }
      rafId = requestAnimationFrame(syncTimeLoop);
    }
  };

  const startClock = () => {
    stopClock();
    lastSyncTimestamp = performance.now();
    rafId = requestAnimationFrame(syncTimeLoop);
  };

  const stopClock = () => {
    if (rafId !== null) {
      cancelAnimationFrame(rafId);
      rafId = null;
    }
    if (videoElement.value) {
      currentTime.value = videoElement.value.currentTime;
    }
  };

  const bindVideo = (el: HTMLVideoElement) => {
    videoElement.value = el;

    el.addEventListener('loadedmetadata', () => {
      const d = Number.isFinite(el.duration) ? Math.max(0, el.duration) : 0;
      duration.value = d;
      metadata.value = {
        duration: d,
        videoWidth: el.videoWidth,
        videoHeight: el.videoHeight,
        aspectRatio: el.videoWidth && el.videoHeight ? el.videoWidth / el.videoHeight : 16 / 9,
      };
      isReady.value = true;
      error.value = null;
    });

    el.addEventListener('play', () => {
      isPlaying.value = true;
      startClock();
    });

    el.addEventListener('pause', () => {
      isPlaying.value = false;
      stopClock();
    });

    el.addEventListener('ended', () => {
      isPlaying.value = false;
      stopClock();
      currentTime.value = duration.value;
    });

    el.addEventListener('waiting', () => {
      isBuffering.value = true;
    });

    el.addEventListener('canplay', () => {
      isBuffering.value = false;
    });

    el.addEventListener('error', () => {
      isBuffering.value = false;
      isPlaying.value = false;
      isReady.value = false;
      stopClock();
      if (el.error) {
        // 如果遇到 Format error (code 4) 且尚未启用转码参数，自动尝试请求服务端兼容转码流
        const currentSrc = el.currentSrc || el.src;
        if (
          el.error.code === 4 &&
          currentSrc &&
          currentSrc.includes('/api/video/stream') &&
          !currentSrc.includes('transcode=1')
        ) {
          const sep = currentSrc.includes('?') ? '&' : '?';
          const retryUrl = `${currentSrc}${sep}transcode=1`;
          isBuffering.value = true;
          error.value = null;
          el.src = retryUrl;
          el.load();
          return;
        }

        let detail = el.error.message || '';
        switch (el.error.code) {
          case 1:
            detail = '视频加载被中止 (MEDIA_ERR_ABORTED)';
            break;
          case 2:
            detail = '网络错误导致视频下载失败 (MEDIA_ERR_NETWORK)';
            break;
          case 3:
            detail = '视频解码失败，数据损坏 (MEDIA_ERR_DECODE)';
            break;
          case 4:
            detail = `当前视频封装/编码不受浏览器内核直接支持 (${detail || 'Format error'})`;
            break;
          default:
            detail = detail || '未知媒体源错误';
        }
        error.value = `视频播放异常: ${detail}`;
      } else {
        error.value = '无法加载视频流';
      }
    });
  };

  const resetState = () => {
    stopClock();
    if (seekDebounceTimer !== null) {
      window.clearTimeout(seekDebounceTimer);
      seekDebounceTimer = null;
    }
    isPlaying.value = false;
    isBuffering.value = false;
    isSeeking.value = false;
    currentTime.value = 0;
    duration.value = 0;
    isReady.value = false;
    error.value = null;
    metadata.value = {
      duration: 0,
      videoWidth: 0,
      videoHeight: 0,
      aspectRatio: 16 / 9,
    };
  };

  const togglePlay = async () => {
    const el = videoElement.value;
    if (!el || !isReady.value) return;
    if (el.paused) {
      try {
        await el.play();
      } catch (err) {
        console.warn('Playback prevented:', err);
      }
    } else {
      el.pause();
    }
  };

  let seekDebounceTimer: number | null = null;

  const onSeekInput = (targetSec: number) => {
    isSeeking.value = true;
    currentTime.value = Math.max(0, Math.min(targetSec, duration.value));

    if (seekDebounceTimer !== null) {
      window.clearTimeout(seekDebounceTimer);
    }
    seekDebounceTimer = window.setTimeout(() => {
      const el = videoElement.value;
      if (el && Number.isFinite(targetSec)) {
        if ('fastSeek' in el && typeof el.fastSeek === 'function') {
          el.fastSeek(targetSec);
        } else {
          el.currentTime = targetSec;
        }
      }
    }, 30);
  };

  const onSeekChange = (targetSec: number) => {
    if (seekDebounceTimer !== null) {
      window.clearTimeout(seekDebounceTimer);
      seekDebounceTimer = null;
    }
    const el = videoElement.value;
    const finalSec = Math.max(0, Math.min(targetSec, duration.value));
    if (el && Number.isFinite(finalSec)) {
      el.currentTime = finalSec;
      currentTime.value = finalSec;
    }
    isSeeking.value = false;
    if (isPlaying.value) {
      startClock();
    }
  };

  const seekTo = (seconds: number) => {
    const el = videoElement.value;
    if (!el) return;
    const target = Math.max(0, Math.min(seconds, duration.value));
    el.currentTime = target;
    currentTime.value = target;
  };

  const stepFrame = (forward = true, fps = 25) => {
    const delta = 1 / fps;
    seekTo(currentTime.value + (forward ? delta : -delta));
  };

  onUnmounted(() => {
    stopClock();
    if (seekDebounceTimer !== null) {
      window.clearTimeout(seekDebounceTimer);
    }
  });

  return {
    videoElement,
    bindVideo,
    isPlaying,
    isBuffering,
    isSeeking,
    currentTime,
    duration,
    currentTimecode,
    durationTimecode,
    progressRatio,
    metadata,
    isReady,
    error,
    togglePlay,
    seekTo,
    onSeekInput,
    onSeekChange,
    stepFrame,
    resetState,
  };
}
