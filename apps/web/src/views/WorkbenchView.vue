<template>
  <!-- 1. 未选择视频时：全屏宽幅居中选择器 -->
  <div v-if="workbenchStore.state === 'Empty'" class="sl-workbench-empty-container">
    <DropZone />
  </div>

  <!-- 2. 已载入视频时：标准双栏工作台 -->
  <div v-else class="sl-workbench-layout">
    <!-- 左侧区域：视频播放器舞台 + 流水线参数设置 -->
    <section class="sl-left-pane">
      <!-- 顶栏：更换视频回退按钮 + 当前视频信息 -->
      <div class="sl-workbench-top-bar">
        <button
          class="sl-change-video-btn"
          :disabled="workbenchStore.isLocked"
          title="返回重新选择或输入其他视频"
          @click="handleBackToSelector"
        >
          <svg class="sl-back-arrow" viewBox="0 0 16 16" fill="currentColor">
            <path fill-rule="evenodd" d="M15 8a.5.5 0 0 0-.5-.5H2.707l3.147-3.146a.5.5 0 1 0-.708-.708l-4 4a.5.5 0 0 0 0 .708l4 4a.5.5 0 0 0 .708-.708L2.707 8.5H14.5A.5.5 0 0 0 15 8z"/>
          </svg>
          <span>更换视频</span>
        </button>

        <div class="sl-active-video-badge" :title="workbenchStore.videoPath">
          <svg class="sl-badge-icon" viewBox="0 0 16 16" fill="currentColor">
            <path d="M0 2a2 2 0 0 1 2-2h12a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2V2zm11.5 5.5-5-3A.5.5 0 0 0 5.75 5v6a.5.5 0 0 0 .75.433l5-3a.5.5 0 0 0 0-.866z"/>
          </svg>
          <span class="sl-badge-name">{{ workbenchStore.videoName }}</span>
        </div>
      </div>

      <div
        v-if="statusBanner"
        class="sl-status-banner"
        :class="bannerKind"
        role="status"
      >
        <span class="sl-status-banner-text">{{ statusBanner }}</span>
        <button class="sl-status-banner-dismiss" aria-label="关闭提示" @click="dismissed = true">×</button>
      </div>

      <div class="sl-video-stage-container">
        <VideoPlayer
          ref="playerRef"
          :video-path="workbenchStore.videoPath"
          :raw-src="workbenchStore.previewSrc || undefined"
        >
          <template #overlay="{ metadata }">
            <RoiOverlay :metadata="metadata" />
          </template>
        </VideoPlayer>
      </div>
      <SidebarControls />
    </section>

    <!-- 右侧区域：全高宽屏实时字幕与精修工作区 -->
    <section class="sl-right-pane">
      <LiveTranscript @seek="handleTranscriptSeek" />
    </section>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import DropZone from '../components/DropZone.vue';
import VideoPlayer from '../components/VideoPlayer.vue';
import RoiOverlay from '../components/RoiOverlay.vue';
import SidebarControls from '../components/SidebarControls.vue';
import LiveTranscript from '../components/LiveTranscript.vue';
import { useWorkbenchStore } from '../stores/workbench';

const workbenchStore = useWorkbenchStore();
const playerRef = ref<InstanceType<typeof VideoPlayer> | null>(null);
const dismissed = ref(false);

const statusBanner = computed(() => {
  if (workbenchStore.state === 'Failed') {
    return `提取失败：${workbenchStore.errorMessage ?? '未知错误'}`;
  }
  if (workbenchStore.state === 'Cancelled') {
    return `已取消：${workbenchStore.errorMessage ?? ''}`;
  }
  if (workbenchStore.needsServerPath && !dismissed.value) {
    return '本地预览模式：正在定位该视频在服务器上的路径；未命中时请在左侧「服务端路径」粘贴绝对路径后再提取。';
  }
  return '';
});

const bannerKind = computed(() => {
  if (workbenchStore.state === 'Failed') return 'is-failed';
  if (workbenchStore.state === 'Cancelled') return 'is-cancelled';
  return 'is-info';
});

watch(
  () => workbenchStore.state,
  () => {
    dismissed.value = false;
  }
);

function handleTranscriptSeek(timeSec: number) {
  playerRef.value?.seekTo(timeSec);
}

function handleBackToSelector() {
  workbenchStore.reset();
}
</script>

<style scoped>
.sl-workbench-empty-container {
  width: 100%;
  height: calc(100vh - 48px);
  background: var(--sl-surface-ground);
  display: flex;
  align-items: center;
  justify-content: center;
  overflow-y: auto;
}

.sl-workbench-layout {
  display: grid;
  grid-template-columns: minmax(0, 1.75fr) minmax(360px, 1fr);
  gap: 16px;
  width: 100%;
  height: calc(100vh - 48px);
  padding: 16px;
  background: var(--sl-surface-ground);
  overflow: hidden;
}

@media (max-width: 960px) {
  .sl-workbench-layout {
    grid-template-columns: 1fr;
    grid-template-rows: auto 1fr;
    overflow-y: auto;
  }
}

.sl-left-pane {
  display: flex;
  flex-direction: column;
  gap: 12px;
  height: 100%;
  min-height: 0;
  overflow-y: auto;
}

.sl-workbench-top-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 4px 2px;
}

.sl-change-video-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  background: rgba(255, 255, 255, 0.05);
  border: 1px solid var(--sl-border-standard);
  color: var(--sl-text-secondary);
  border-radius: var(--sl-radius-sm);
  padding: 6px 12px;
  font-size: 12.5px;
  font-weight: 500;
  cursor: pointer;
  transition: var(--sl-transition-snappy);
}

.sl-change-video-btn:hover:not(:disabled) {
  background: rgba(255, 255, 255, 0.1);
  color: var(--sl-text-primary);
  border-color: var(--sl-color-accent);
}

.sl-change-video-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.sl-back-arrow {
  width: 13px;
  height: 13px;
}

.sl-active-video-badge {
  display: flex;
  align-items: center;
  gap: 6px;
  background: rgba(255, 255, 255, 0.03);
  border: 1px solid var(--sl-border-subtle);
  border-radius: var(--sl-radius-sm);
  padding: 4px 10px;
  max-width: 400px;
  overflow: hidden;
}

.sl-badge-icon {
  width: 12px;
  height: 12px;
  color: var(--sl-color-accent);
  flex-shrink: 0;
}

.sl-badge-name {
  font-size: 12px;
  font-weight: 500;
  color: var(--sl-text-primary);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.sl-status-banner {
  display: flex;
  align-items: center;
  gap: 10px;
  width: 100%;
  padding: 8px 12px;
  border-radius: var(--sl-radius-sm);
  border: 1px solid var(--sl-border-standard);
  background: var(--sl-surface-card);
  font-size: var(--sl-font-size-sm);
  flex-shrink: 0;
}

.sl-status-banner.is-failed {
  border-color: rgba(255, 99, 99, 0.45);
  color: #ff8080;
}

.sl-status-banner.is-cancelled {
  border-color: rgba(212, 167, 44, 0.45);
  color: #d4a72c;
}

.sl-status-banner.is-info {
  border-color: rgba(90, 140, 255, 0.45);
  color: #8ab0ff;
}

.sl-status-banner-text {
  flex: 1;
  min-width: 0;
}

.sl-status-banner-dismiss {
  border: none;
  background: transparent;
  color: var(--sl-text-tertiary);
  font-size: 16px;
  cursor: pointer;
  padding: 0 4px;
}

.sl-video-stage-container {
  width: 100%;
  aspect-ratio: 16 / 9;
  max-height: 480px;
  min-height: 240px;
  border-radius: var(--sl-radius-md);
  overflow: hidden;
  flex-shrink: 0;
  display: flex;
}

.sl-right-pane {
  display: flex;
  flex-direction: column;
  height: 100%;
  min-height: 0;
  overflow: hidden;
}
</style>
