<template>
  <div class="sl-workbench-layout">
    <!-- 左侧区域：视频播放器舞台 + 流水线参数设置 -->
    <section class="sl-left-pane">
      <div class="sl-video-stage-container">
        <DropZone v-if="workbenchStore.state === 'Empty'" />
        <VideoPlayer 
          v-else 
          ref="playerRef"
          :video-path="workbenchStore.videoPath"
        />
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
import { ref } from 'vue';
import DropZone from '../components/DropZone.vue';
import VideoPlayer from '../components/VideoPlayer.vue';
import SidebarControls from '../components/SidebarControls.vue';
import LiveTranscript from '../components/LiveTranscript.vue';
import { useWorkbenchStore } from '../stores/workbench';

const workbenchStore = useWorkbenchStore();
const playerRef = ref<InstanceType<typeof VideoPlayer> | null>(null);

function handleTranscriptSeek(timeSec: number) {
  playerRef.value?.seekTo(timeSec);
}
</script>

<style scoped>
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
  gap: 14px;
  height: 100%;
  min-height: 0;
  overflow-y: auto;
}

.sl-video-stage-container {
  width: 100%;
  aspect-ratio: 16 / 9;
  max-height: 480px;
  min-height: 240px;
  border-radius: var(--sl-radius-md);
  overflow: hidden;
  flex-shrink: 0;
}

.sl-right-pane {
  display: flex;
  flex-direction: column;
  height: 100%;
  min-height: 0;
  overflow: hidden;
}
</style>
