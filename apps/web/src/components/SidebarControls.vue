<template>
  <div class="sl-sidebar-controls">
    <!-- 流水线设置卡片 -->
    <div class="sl-card">
      <div class="sl-card-header">
        <span class="sl-card-title">流水线设置 (Pipeline)</span>
      </div>

      <div class="sl-card-body">
        <!-- 引擎选择 -->
        <div class="sl-form-row">
          <label class="sl-form-label">OCR 引擎</label>
          <div class="sl-segmented-control">
            <button 
              v-for="eng in availableEngines" 
              :key="eng.name"
              class="sl-segmented-item"
              :class="{ 'is-selected': workbenchStore.selectedEngine === eng.name }"
              :disabled="workbenchStore.isLocked"
              @click="workbenchStore.selectedEngine = eng.name"
            >
              {{ eng.name === 'vision' ? 'Apple Vision' : eng.name === 'paddle' ? 'PaddleOCR' : 'Mock' }}
            </button>
          </div>
        </div>

        <!-- 采样 FPS -->
        <div class="sl-form-row">
          <div class="sl-form-row-header">
            <label class="sl-form-label">采样频率 (FPS)</label>
            <span class="sl-form-val">{{ workbenchStore.targetFps.toFixed(1) }} fps</span>
          </div>
          <div class="sl-segmented-control">
            <button 
              v-for="fps in [1.0, 2.0, 5.0, 10.0]"
              :key="fps"
              class="sl-segmented-item"
              :class="{ 'is-selected': workbenchStore.targetFps === fps }"
              :disabled="workbenchStore.isLocked"
              @click="workbenchStore.targetFps = fps"
            >
              {{ fps }} fps
            </button>
          </div>
        </div>

        <!-- ROI 选区预设 -->
        <div class="sl-form-row">
          <label class="sl-form-label">选区预设</label>
          <div class="sl-segmented-control">
            <button 
              class="sl-segmented-item"
              :class="{ 'is-selected': isBottom30Roi }"
              :disabled="workbenchStore.isLocked"
              @click="workbenchStore.resetDefaultBottomRoi()"
            >
              底部 30%
            </button>
            <button 
              class="sl-segmented-item"
              :class="{ 'is-selected': isFullScreenRoi }"
              :disabled="workbenchStore.isLocked"
              @click="workbenchStore.updateRegionBox({ x: 0, y: 0, width: 1, height: 1 })"
            >
              全画幅
            </button>
            <button 
              class="sl-segmented-item"
              :class="{ 'is-selected': isCustomRoi }"
              :disabled="true"
            >
              自定义
            </button>
          </div>
        </div>

        <!-- 操作按钮组 -->
        <div class="sl-actions-group">
          <button 
            v-if="!workbenchStore.isLocked"
            class="sl-button-primary"
            :disabled="!workbenchStore.canStart"
            @click="workbenchStore.startExtraction()"
          >
            <svg class="sl-btn-icon" viewBox="0 0 16 16" fill="currentColor">
              <path d="m11.596 8.697-6.363 3.692c-.54.313-1.233-.066-1.233-.697V4.308c0-.63.692-1.01 1.233-.696l6.363 3.692a.802.802 0 0 1 0 1.393z"/>
            </svg>
            <span>{{ workbenchStore.state === 'Review' ? '重新提取' : '开始提取字幕' }}</span>
          </button>

          <button 
            v-else
            class="sl-button-danger"
            @click="workbenchStore.cancelExtraction()"
          >
            <svg class="sl-btn-icon" viewBox="0 0 16 16" fill="currentColor">
              <path d="M4 4h8v8H4z"/>
            </svg>
            <span>取消提取</span>
          </button>

          <button 
            v-if="workbenchStore.canExport"
            class="sl-button-export"
            @click="workbenchStore.downloadSrt()"
          >
            <svg class="sl-btn-icon" viewBox="0 0 16 16" fill="currentColor">
              <path d="M.5 9.9a.5.5 0 0 1 .5.5v2.5a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-2.5a.5.5 0 0 1 1 0v2.5a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2v-2.5a.5.5 0 0 1 .5-.5z"/>
              <path d="M7.646 11.854a.5.5 0 0 0 .708 0l3-3a.5.5 0 0 0-.708-.708L8.5 10.293V1.5a.5.5 0 0 0-1 0v8.793L5.354 8.146a.5.5 0 1 0-.708.708l3 3z"/>
            </svg>
            <span>导出 SRT 字幕</span>
          </button>
        </div>

        <!-- 实时进度条 -->
        <div v-if="workbenchStore.isLocked" class="sl-progress-box">
          <div class="sl-progress-header">
            <span class="sl-progress-stage">处理中 · {{ (workbenchStore.progress.pct * 100).toFixed(0) }}%</span>
            <span class="sl-progress-eta">{{ workbenchStore.progress.stage }}</span>
          </div>
          <div class="sl-progress-bar-bg">
            <div 
              class="sl-progress-bar-fill"
              :style="{ width: `${Math.max(3, workbenchStore.progress.pct * 100)}%` }"
            ></div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useSystemStore } from '../stores/system';
import { useWorkbenchStore } from '../stores/workbench';
import type { SystemEngineInfo } from '../types/api';

const systemStore = useSystemStore();
const workbenchStore = useWorkbenchStore();

const availableEngines = computed<SystemEngineInfo[]>(() => {
  if (systemStore.availableEngines.length === 0) {
    return [{ name: 'mock', available: true, detail: 'Mock' }];
  }
  return systemStore.availableEngines;
});

const isBottom30Roi = computed(() => {
  const b = workbenchStore.regionBox;
  return Math.abs(b.x - 0.0) < 0.01 && Math.abs(b.y - 0.7) < 0.05 && Math.abs(b.width - 1.0) < 0.01 && Math.abs(b.height - 0.3) < 0.05;
});

const isFullScreenRoi = computed(() => {
  const b = workbenchStore.regionBox;
  return Math.abs(b.x - 0.0) < 0.01 && Math.abs(b.y - 0.0) < 0.01 && Math.abs(b.width - 1.0) < 0.01 && Math.abs(b.height - 1.0) < 0.01;
});

const isCustomRoi = computed(() => !isBottom30Roi.value && !isFullScreenRoi.value);
</script>

<style scoped>
.sl-sidebar-controls {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.sl-card {
  background: var(--sl-surface-card);
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-md);
  box-shadow: var(--sl-shadow-sm), var(--sl-inner-highlight);
  overflow: hidden;
}

.sl-card-header {
  padding: 12px 14px;
  border-bottom: 1px solid var(--sl-border-subtle);
  background: rgba(255, 255, 255, 0.02);
}

.sl-card-title {
  font-size: var(--sl-font-size-xs);
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.04em;
  color: var(--sl-text-tertiary);
}

.sl-card-body {
  padding: 14px;
  display: flex;
  flex-direction: column;
  gap: 14px;
}

.sl-form-row {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.sl-form-row-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.sl-form-label {
  font-size: var(--sl-font-size-xs);
  color: var(--sl-text-secondary);
  font-weight: 500;
}

.sl-form-val {
  font-family: var(--sl-font-family-mono);
  font-size: var(--sl-font-size-xs);
  color: var(--sl-text-tertiary);
}

.sl-segmented-control {
  display: flex;
  background: var(--sl-surface-base);
  padding: 2px;
  border-radius: var(--sl-radius-sm);
  border: 1px solid var(--sl-border-subtle);
  gap: 2px;
}

.sl-segmented-item {
  flex: 1;
  border: none;
  background: transparent;
  color: var(--sl-text-tertiary);
  font-size: var(--sl-font-size-xs);
  font-weight: 500;
  padding: 5px 4px;
  border-radius: var(--sl-radius-xs);
  cursor: pointer;
  transition: var(--sl-transition-snappy);
  text-align: center;
  white-space: nowrap;
}

.sl-segmented-item.is-selected {
  background: var(--sl-surface-elevated);
  color: var(--sl-text-primary);
  font-weight: 600;
  box-shadow: var(--sl-shadow-sm);
}

.sl-segmented-item:hover:not(.is-selected):not(:disabled) {
  color: var(--sl-text-secondary);
}

.sl-segmented-item:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.sl-actions-group {
  display: flex;
  flex-direction: column;
  gap: 8px;
  margin-top: 4px;
}

.sl-button-primary,
.sl-button-danger,
.sl-button-export {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  height: 34px;
  border: none;
  border-radius: var(--sl-radius-sm);
  font-size: var(--sl-font-size-sm);
  font-weight: 500;
  cursor: pointer;
  transition: var(--sl-transition-snappy);
  box-shadow: var(--sl-shadow-sm);
}

.sl-button-primary {
  background: var(--sl-color-accent);
  color: #ffffff;
}

.sl-button-primary:hover:not(:disabled) {
  background: var(--sl-color-accent-hover);
  box-shadow: var(--sl-shadow-glow-accent);
}

.sl-button-primary:disabled {
  opacity: 0.35;
  cursor: not-allowed;
}

.sl-button-danger {
  background: rgba(255, 69, 58, 0.15);
  border: 1px solid rgba(255, 69, 58, 0.4);
  color: var(--sl-color-error);
}

.sl-button-danger:hover {
  background: rgba(255, 69, 58, 0.25);
}

.sl-button-export {
  background: var(--sl-surface-elevated);
  border: 1px solid var(--sl-border-standard);
  color: var(--sl-color-success);
}

.sl-button-export:hover {
  background: var(--sl-surface-card-hover);
  border-color: rgba(48, 209, 88, 0.4);
}

.sl-btn-icon {
  width: 14px;
  height: 14px;
}

.sl-progress-box {
  display: flex;
  flex-direction: column;
  gap: 6px;
  padding: 8px;
  background: var(--sl-surface-base);
  border-radius: var(--sl-radius-sm);
  border: 1px solid var(--sl-border-subtle);
}

.sl-progress-header {
  display: flex;
  justify-content: space-between;
  font-size: var(--sl-font-size-xs);
}

.sl-progress-stage {
  color: var(--sl-color-processing);
  font-weight: 500;
}

.sl-progress-eta {
  font-family: var(--sl-font-family-mono);
  color: var(--sl-text-tertiary);
}

.sl-progress-bar-bg {
  width: 100%;
  height: 4px;
  background: rgba(255, 255, 255, 0.1);
  border-radius: var(--sl-radius-pill);
  overflow: hidden;
}

.sl-progress-bar-fill {
  height: 100%;
  background: var(--sl-color-processing);
  border-radius: var(--sl-radius-pill);
  transition: width 150ms ease;
}
</style>
