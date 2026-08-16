<template>
  <div 
    class="sl-video-stage"
    tabindex="0"
    @keydown.space.prevent="togglePlay"
    @keydown.left.prevent="stepFrame(false)"
    @keydown.right.prevent="stepFrame(true)"
  >
    <div class="sl-video-canvas-wrapper">
      <video
        ref="videoRef"
        :src="streamUrl"
        class="sl-video-element"
        playsinline
        crossorigin="anonymous"
        @click="togglePlay"
      />

      <!-- 缓冲 Spinner -->
      <div v-if="isBuffering" class="sl-video-spinner-overlay">
        <div class="sl-spinner"></div>
      </div>

      <!-- 错误遮罩 -->
      <div v-if="error" class="sl-video-error-overlay">
        <p>{{ error }}</p>
      </div>

      <!-- ROI 覆盖层插槽 (Feature 12202) -->
      <slot name="overlay" :metadata="metadata" :current-time="currentTime" />
    </div>

    <!-- 底部悬浮控制胶囊 (Apple / Notion Frosted Bar) -->
    <div class="sl-player-bar">
      <!-- 播放/暂停 -->
      <button 
        class="sl-icon-button" 
        :title="isPlaying ? '暂停 (Space)' : '播放 (Space)'"
        @click="togglePlay"
      >
        <svg v-if="!isPlaying" class="sl-ctrl-icon" viewBox="0 0 16 16" fill="currentColor">
          <path d="m11.596 8.697-6.363 3.692c-.54.313-1.233-.066-1.233-.697V4.308c0-.63.692-1.01 1.233-.696l6.363 3.692a.802.802 0 0 1 0 1.393z"/>
        </svg>
        <svg v-else class="sl-ctrl-icon" viewBox="0 0 16 16" fill="currentColor">
          <path d="M5.5 3.5A1.5 1.5 0 0 1 7 5v6a1.5 1.5 0 0 1-3 0V5a1.5 1.5 0 0 1 1.5-1.5zm5 0A1.5 1.5 0 0 1 12 5v6a1.5 1.5 0 0 1-3 0V5a1.5 1.5 0 0 1 1.5-1.5z"/>
        </svg>
      </button>

      <!-- 步退 1 帧 -->
      <button 
        class="sl-icon-button sl-icon-button--sm" 
        title="后退 1 帧 (Left Arrow)"
        @click="stepFrame(false)"
      >
        <svg class="sl-ctrl-icon" viewBox="0 0 16 16" fill="currentColor">
          <path d="M11.354 1.646a.5.5 0 0 1 0 .708L5.707 8l5.647 5.646a.5.5 0 0 1-.708.708l-6-6a.5.5 0 0 1 0-.708l6-6a.5.5 0 0 1 .708 0z"/>
        </svg>
      </button>

      <!-- 步进 1 帧 -->
      <button 
        class="sl-icon-button sl-icon-button--sm" 
        title="前进 1 帧 (Right Arrow)"
        @click="stepFrame(true)"
      >
        <svg class="sl-ctrl-icon" viewBox="0 0 16 16" fill="currentColor">
          <path d="M4.646 1.646a.5.5 0 0 1 .708 0l6 6a.5.5 0 0 1 0 .708l-6 6a.5.5 0 0 1-.708-.708L10.293 8 4.646 2.354a.5.5 0 0 1 0-.708z"/>
        </svg>
      </button>

      <!-- 10Hz 毫秒级时码显示 -->
      <div class="sl-timecode">
        <span class="current-time">{{ currentTimecode }}</span>
        <span class="divider">/</span>
        <span class="duration">{{ durationTimecode }}</span>
      </div>

      <!-- 进度滑块 -->
      <div class="sl-scrubber">
        <div class="sl-scrubber-track">
          <div class="sl-scrubber-progress" :style="{ width: `${progressRatio * 100}%` }"></div>
        </div>
        <input 
          type="range" 
          min="0" 
          :max="duration || 100" 
          step="0.01"
          :value="currentTime"
          class="sl-scrubber-input"
          @input="onSeekInput(Number(($event.target as HTMLInputElement).value))"
          @change="onSeekChange(Number(($event.target as HTMLInputElement).value))"
        />
      </div>

      <!-- 规格标签 -->
      <div v-if="metadata.videoWidth" class="sl-video-meta-badge">
        {{ metadata.videoWidth }}x{{ metadata.videoHeight }}
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted } from 'vue';
import { useVideoPlayer } from '../composables/useVideoPlayer';
import { SubLiftApiClient } from '../api/client';
import { useWorkbenchStore } from '../stores/workbench';

const props = defineProps<{
  videoPath: string;
}>();

const workbenchStore = useWorkbenchStore();
const videoRef = ref<HTMLVideoElement | null>(null);

const {
  bindVideo,
  isPlaying,
  isBuffering,
  currentTime,
  duration,
  currentTimecode,
  durationTimecode,
  progressRatio,
  metadata,
  error,
  togglePlay,
  seekTo,
  onSeekInput,
  onSeekChange,
  stepFrame,
} = useVideoPlayer();

const streamUrl = computed(() => {
  if (!props.videoPath) return '';
  return SubLiftApiClient.getVideoStreamUrl(props.videoPath);
});

// 10Hz 时码同步到 Workbench Store
watch(currentTime, (t) => {
  workbenchStore.updatePlaybackTime(Math.floor(t * 1000));
});

onMounted(() => {
  if (videoRef.value) {
    bindVideo(videoRef.value);
  }
});

defineExpose({
  seekTo,
  togglePlay,
  currentTime,
  duration,
  metadata,
});
</script>

<style scoped>
.sl-video-stage {
  position: relative;
  width: 100%;
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  background: #000000;
  border-radius: var(--sl-radius-lg);
  border: 1px solid var(--sl-border-standard);
  overflow: hidden;
  box-shadow: var(--sl-shadow-md), var(--sl-inner-shadow);
  outline: none;
}

.sl-video-canvas-wrapper {
  position: relative;
  width: 100%;
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
}

.sl-video-element {
  max-width: 100%;
  max-height: 100%;
  object-fit: contain;
  cursor: pointer;
}

.sl-video-spinner-overlay,
.sl-video-error-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgba(0, 0, 0, 0.5);
  backdrop-filter: var(--sl-glass-blur-sm);
  pointer-events: none;
}

.sl-video-error-overlay {
  color: var(--sl-color-error);
  font-size: var(--sl-font-size-sm);
  font-weight: 500;
}

.sl-spinner {
  width: 32px;
  height: 32px;
  border: 3px solid rgba(255, 255, 255, 0.15);
  border-top-color: var(--sl-color-accent);
  border-radius: 50%;
  animation: sl-spin 0.8s linear infinite;
}

@keyframes sl-spin {
  to { transform: rotate(360deg); }
}

.sl-player-bar {
  position: absolute;
  bottom: 14px;
  left: 50%;
  transform: translateX(-50%);
  width: calc(100% - 32px);
  max-width: 680px;
  height: 40px;
  padding: 0 12px;
  display: flex;
  align-items: center;
  gap: 10px;
  background: var(--sl-glass-bar);
  backdrop-filter: var(--sl-glass-blur);
  -webkit-backdrop-filter: var(--sl-glass-blur);
  border: 1px solid var(--sl-border-focus);
  border-radius: var(--sl-radius-pill);
  box-shadow: var(--sl-shadow-floating), var(--sl-inner-highlight);
  z-index: 20;
}

.sl-icon-button {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 26px;
  height: 26px;
  border: none;
  background: transparent;
  color: var(--sl-text-primary);
  border-radius: var(--sl-radius-pill);
  cursor: pointer;
  transition: var(--sl-transition-snappy);
}

.sl-icon-button--sm {
  width: 22px;
  height: 22px;
  color: var(--sl-text-secondary);
}

.sl-icon-button:hover {
  background: rgba(255, 255, 255, 0.15);
  color: #ffffff;
}

.sl-icon-button:active {
  transform: scale(0.9);
}

.sl-ctrl-icon {
  width: 14px;
  height: 14px;
}

.sl-timecode {
  font-family: var(--sl-font-family-mono);
  font-size: var(--sl-font-size-xs);
  font-variant-numeric: tabular-nums;
  letter-spacing: -0.01em;
  color: var(--sl-text-secondary);
  white-space: nowrap;
  user-select: none;
  margin-left: 2px;
}

.sl-timecode .current-time {
  color: var(--sl-color-accent-hover);
  font-weight: 600;
}

.sl-timecode .divider {
  margin: 0 3px;
  opacity: 0.4;
}

.sl-scrubber {
  flex: 1;
  position: relative;
  height: 16px;
  display: flex;
  align-items: center;
}

.sl-scrubber-track {
  width: 100%;
  height: 4px;
  background: rgba(255, 255, 255, 0.15);
  border-radius: var(--sl-radius-pill);
  overflow: hidden;
  transition: height 120ms ease;
}

.sl-scrubber:hover .sl-scrubber-track {
  height: 6px;
}

.sl-scrubber-progress {
  height: 100%;
  background: var(--sl-color-accent);
  border-radius: var(--sl-radius-pill);
}

.sl-scrubber-input {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  opacity: 0;
  cursor: pointer;
}

.sl-video-meta-badge {
  font-family: var(--sl-font-family-mono);
  font-size: 10px;
  color: var(--sl-text-tertiary);
  padding: 1px 5px;
  background: rgba(255, 255, 255, 0.05);
  border-radius: var(--sl-radius-xs);
  border: 1px solid var(--sl-border-subtle);
  white-space: nowrap;
}
</style>
