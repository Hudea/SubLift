<template>
  <!-- 1. 未选择视频时：全屏宽幅居中选择器 -->
  <div v-if="workbenchStore.state === 'Empty'" class="sl-workbench-empty-container">
    <DropZone />
  </div>

  <!-- 2. 已载入视频时：标准双栏工作台（紧凑顶栏快速设置） -->
  <div v-else class="sl-workbench-layout">
    <!-- 左侧区域：视频播放器舞台 + 顶栏流水线设置 -->
    <section class="sl-left-pane">
      <!-- 顶栏：更换视频 + 视频标题 + 引擎下拉 + 采样下拉 + 提取按钮 (SwiftUI 紧凑模式) -->
      <div class="sl-workbench-top-bar">
        <div class="sl-top-bar-left">
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

        <div class="sl-top-bar-right">
          <!-- 1. OCR 引擎下拉框 -->
          <div class="sl-select-wrapper">
            <select
              v-model="workbenchStore.selectedEngine"
              class="sl-native-select"
              :disabled="workbenchStore.isLocked"
              title="选择 OCR 提取引擎"
            >
              <option v-for="eng in productionEngines" :key="eng.name" :value="eng.name">
                {{ eng.name === 'vision' ? 'Apple Vision' : eng.name === 'paddle' ? 'PaddleOCR' : 'Mock' }}
              </option>
            </select>
            <svg class="sl-select-arrow" viewBox="0 0 16 16" fill="currentColor">
              <path fill-rule="evenodd" d="M1.646 4.646a.5.5 0 0 1 .708 0L8 10.293l5.646-5.647a.5.5 0 0 1 .708.708l-6 6a.5.5 0 0 1-.708 0l-6-6a.5.5 0 0 1 0-.708z"/>
            </svg>
          </div>

          <!-- 2. 采样质量下拉框（完全对齐 SwiftUI: 快速 / 平衡 / 精细） -->
          <div class="sl-select-wrapper">
            <select
              v-model.number="workbenchStore.targetFps"
              class="sl-native-select"
              :disabled="workbenchStore.isLocked"
              title="选择采样质量"
            >
              <option :value="5.0">快速</option>
              <option :value="8.0">平衡</option>
              <option :value="12.0">精细</option>
            </select>
            <svg class="sl-select-arrow" viewBox="0 0 16 16" fill="currentColor">
              <path fill-rule="evenodd" d="M1.646 4.646a.5.5 0 0 1 .708 0L8 10.293l5.646-5.647a.5.5 0 0 1 .708.708l-6 6a.5.5 0 0 1-.708 0l-6-6a.5.5 0 0 1 0-.708z"/>
            </svg>
          </div>

          <!-- 3. 开始 / 取消提取 按钮 -->
          <button
            v-if="!workbenchStore.isLocked"
            class="sl-extract-btn"
            :disabled="!workbenchStore.canStart"
            title="开始提取字幕"
            @click="workbenchStore.startExtraction"
          >
            <svg class="sl-btn-icon" viewBox="0 0 16 16" fill="currentColor">
              <path d="m11.596 8.697-6.363 3.692c-.54.313-1.233-.066-1.233-.697V4.308c0-.63.692-1.01 1.233-.696l6.363 3.692a.802.802 0 0 1 0 1.393z"/>
            </svg>
            <span>开始提取</span>
          </button>

          <button
            v-else
            class="sl-cancel-btn"
            title="取消当前提取任务"
            @click="workbenchStore.cancelExtraction"
          >
            <div class="sl-btn-spinner"></div>
            <span>取消提取 ({{ Math.round(workbenchStore.progress.pct) }}%)</span>
          </button>
        </div>
      </div>

      <!-- 提取进度条（运行中展示） -->
      <div v-if="workbenchStore.state === 'Processing'" class="sl-progress-strip">
        <div class="sl-progress-fill" :style="{ width: `${workbenchStore.progress.pct}%` }"></div>
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

      <!-- 视频播放器舞台 -->
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

      <!-- 底部快捷辅助操作（ROI 智能识别与快速复位） -->
      <div class="sl-quick-aux-bar">
        <div class="sl-aux-left">
          <span v-if="workbenchStore.isDetectingRegion" class="sl-aux-detecting">
            <span class="sl-aux-spinner"></span>
            <span>正在智能探测字幕区域…</span>
          </span>
          <span v-else-if="workbenchStore.roiDetectionFeedback" class="sl-aux-feedback">
            {{ workbenchStore.roiDetectionFeedback }}
          </span>
          <span v-else class="sl-aux-hint">
            📐 画面字幕选区：可直接在上方拖拽，或点击右侧智能识别
          </span>
        </div>
        <div class="sl-aux-right">
          <button
            class="sl-aux-btn sl-aux-btn--highlight"
            :disabled="workbenchStore.isLocked || workbenchStore.isDetectingRegion || !workbenchStore.videoPath"
            title="全视频多点智能扫描字幕所在区域并自动吸附"
            @click="handleAutoDetect"
          >
            ✨ 智能识别字幕区
          </button>
          <button
            class="sl-aux-btn sl-aux-btn--highlight"
            :disabled="workbenchStore.isLocked || workbenchStore.isDetectingRegion || !workbenchStore.videoPath"
            title="对当前播放画面截帧并吸附文字选区"
            @click="handleDetectCurrentPlayhead"
          >
            🎯 识别当前画面
          </button>
          <button
            class="sl-aux-btn"
            :disabled="workbenchStore.isLocked"
            title="重置选区为画面底部 30%"
            @click="workbenchStore.resetDefaultBottomRoi"
          >
            底部 30%
          </button>
          <button
            class="sl-aux-btn"
            :disabled="workbenchStore.isLocked"
            title="设置选区为全画幅 100%"
            @click="workbenchStore.updateRegionBox({ x: 0, y: 0, width: 1, height: 1 })"
          >
            全画幅
          </button>
        </div>
      </div>
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
import LiveTranscript from '../components/LiveTranscript.vue';
import { useWorkbenchStore } from '../stores/workbench';
import { useSystemStore } from '../stores/system';

const workbenchStore = useWorkbenchStore();
const systemStore = useSystemStore();
const playerRef = ref<InstanceType<typeof VideoPlayer> | null>(null);
const dismissed = ref(false);

const productionEngines = computed(() => {
  const list = systemStore.availableEngines.filter((e) => e.name !== 'mock');
  return list.length > 0 ? list : systemStore.availableEngines;
});

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

function handleAutoDetect() {
  workbenchStore.autoDetectSubtitleRegion({ silent: false });
}

function handleDetectCurrentPlayhead() {
  const timeSec = playerRef.value?.currentTime ?? 0;
  workbenchStore.autoDetectSubtitleRegion({ timeSec, silent: false });
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
  grid-template-columns: minmax(0, 1.6fr) minmax(380px, 1fr);
  gap: 16px;
  width: 100%;
  height: calc(100vh - 48px);
  padding: 14px 16px;
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
  gap: 10px;
  height: 100%;
  min-height: 0;
  overflow-y: auto;
}

/* 顶栏工具条：极简 macOS 紧凑风格 */
.sl-workbench-top-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 2px 0;
  flex-shrink: 0;
}

.sl-top-bar-left {
  display: flex;
  align-items: center;
  gap: 10px;
  min-width: 0;
  flex: 1;
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
  font-size: 12px;
  font-weight: 500;
  cursor: pointer;
  white-space: nowrap;
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
  width: 12px;
  height: 12px;
}

.sl-active-video-badge {
  display: flex;
  align-items: center;
  gap: 6px;
  background: rgba(255, 255, 255, 0.03);
  border: 1px solid var(--sl-border-subtle);
  border-radius: var(--sl-radius-sm);
  padding: 5px 10px;
  max-width: 320px;
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

.sl-top-bar-right {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
}

/* 下拉菜单包装器 (Apple 风格 Menu) */
.sl-select-wrapper {
  position: relative;
  display: flex;
  align-items: center;
}

.sl-native-select {
  appearance: none;
  background: var(--sl-surface-base);
  border: 1px solid var(--sl-border-standard);
  color: var(--sl-text-primary);
  border-radius: var(--sl-radius-sm);
  padding: 6px 26px 6px 10px;
  font-size: 12px;
  font-weight: 500;
  cursor: pointer;
  outline: none;
  transition: var(--sl-transition-snappy);
}

.sl-native-select:hover:not(:disabled) {
  border-color: var(--sl-color-accent);
  background: rgba(255, 255, 255, 0.06);
}

.sl-native-select:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.sl-select-arrow {
  position: absolute;
  right: 8px;
  width: 10px;
  height: 10px;
  color: var(--sl-text-tertiary);
  pointer-events: none;
}

/* 开始提取主按钮 */
.sl-extract-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  background: var(--sl-color-accent);
  border: none;
  border-radius: var(--sl-radius-sm);
  color: #ffffff;
  padding: 6px 14px;
  font-size: 12.5px;
  font-weight: 600;
  cursor: pointer;
  white-space: nowrap;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.3);
  transition: var(--sl-transition-snappy);
}

.sl-extract-btn:hover:not(:disabled) {
  background: var(--sl-color-accent-hover);
}

.sl-extract-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.sl-btn-icon {
  width: 12px;
  height: 12px;
}

/* 取消按钮 */
.sl-cancel-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  background: rgba(255, 69, 58, 0.15);
  border: 1px solid rgba(255, 69, 58, 0.35);
  border-radius: var(--sl-radius-sm);
  color: #ff453a;
  padding: 6px 12px;
  font-size: 12px;
  font-weight: 500;
  cursor: pointer;
  white-space: nowrap;
  transition: var(--sl-transition-snappy);
}

.sl-cancel-btn:hover {
  background: rgba(255, 69, 58, 0.25);
  border-color: #ff453a;
}

.sl-btn-spinner {
  width: 12px;
  height: 12px;
  border: 2px solid rgba(255, 69, 58, 0.3);
  border-top-color: #ff453a;
  border-radius: 50%;
  animation: sl-spin 0.8s linear infinite;
}

@keyframes sl-spin {
  to { transform: rotate(360deg); }
}

/* 进度微条 */
.sl-progress-strip {
  width: 100%;
  height: 3px;
  background: rgba(255, 255, 255, 0.08);
  border-radius: 2px;
  overflow: hidden;
  flex-shrink: 0;
}

.sl-progress-fill {
  height: 100%;
  background: var(--sl-color-accent);
  transition: width 0.2s ease-out;
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
  max-height: calc(100vh - 170px);
  min-height: 280px;
  border-radius: var(--sl-radius-md);
  overflow: hidden;
  flex-shrink: 0;
  display: flex;
  background: #000000;
  box-shadow: var(--sl-shadow-md);
}

/* 底部辅助栏 */
.sl-quick-aux-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  padding: 2px 4px;
  flex-shrink: 0;
}

.sl-aux-hint {
  font-size: 11.5px;
  color: var(--sl-text-tertiary);
}

.sl-aux-right {
  display: flex;
  gap: 6px;
}

.sl-aux-btn {
  background: transparent;
  border: 1px solid var(--sl-border-subtle);
  border-radius: var(--sl-radius-xs);
  color: var(--sl-text-secondary);
  padding: 2px 8px;
  font-size: 11px;
  cursor: pointer;
  transition: var(--sl-transition-snappy);
}

.sl-aux-btn:hover:not(:disabled) {
  background: rgba(255, 255, 255, 0.06);
  color: var(--sl-text-primary);
  border-color: var(--sl-border-standard);
}

.sl-aux-btn--highlight {
  background: rgba(10, 132, 255, 0.1);
  border-color: rgba(10, 132, 255, 0.3);
  color: #5ac8fa;
}

.sl-aux-btn--highlight:hover:not(:disabled) {
  background: rgba(10, 132, 255, 0.2);
  border-color: #0a84ff;
  color: #ffffff;
}

.sl-aux-btn:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.sl-aux-detecting {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  font-size: 11.5px;
  color: #5ac8fa;
}

.sl-aux-spinner {
  width: 10px;
  height: 10px;
  border: 1.5px solid rgba(90, 200, 250, 0.3);
  border-top-color: #5ac8fa;
  border-radius: 50%;
  animation: sl-spin 0.8s linear infinite;
}

.sl-aux-feedback {
  font-size: 11.5px;
  color: #30d158;
  animation: sl-feedback-fade 3s ease-out forwards;
}

@keyframes sl-feedback-fade {
  0% { opacity: 1; }
  70% { opacity: 1; }
  100% { opacity: 0.7; }
}

.sl-right-pane {
  display: flex;
  flex-direction: column;
  height: 100%;
  min-height: 0;
  overflow: hidden;
}
</style>
