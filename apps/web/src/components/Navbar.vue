<template>
  <header class="sl-navbar">
    <!-- 左侧 Logo 组 -->
    <div class="sl-logo-group">
      <svg class="sl-logo-icon" viewBox="0 0 24 24" fill="none">
        <rect width="24" height="24" rx="5" fill="#0A84FF" />
        <path d="M5 8h14M5 12h9M5 16h12" stroke="#ffffff" stroke-width="2" stroke-linecap="round" />
      </svg>
      <span class="sl-logo-title">SubLift</span>
      <span class="sl-badge sl-badge--version">{{ systemStore.serverVersion || 'v0.1' }}</span>
    </div>

    <!-- 中间：Apple / Notion 风格 Tab 切换器 -->
    <div class="sl-nav-center">
      <div class="sl-nav-segmented-control">
        <button
          class="sl-segment-btn"
          :class="{ 'is-active': currentView === 'workbench' }"
          @click="emit('update:view', 'workbench')"
        >
          <svg class="sl-seg-icon" viewBox="0 0 16 16" fill="currentColor">
            <path d="M0 1a1 1 0 0 1 1-1h14a1 1 0 0 1 1 1v14a1 1 0 0 1-1 1H1a1 1 0 0 1-1-1V1zm4 0v6h8V1H4zm8 8H4v6h8V9zM3 1H1v14h2V1zm12 0h-2v14h2V1z"/>
          </svg>
          <span>单视频工作台</span>
        </button>

        <button
          class="sl-segment-btn"
          :class="{ 'is-active': currentView === 'batch' }"
          @click="emit('update:view', 'batch')"
        >
          <svg class="sl-seg-icon" viewBox="0 0 16 16" fill="currentColor">
            <path d="M2.5 0a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h11a2 2 0 0 0 2-2V2a2 2 0 0 0-2-2h-11zm5 2v1h5V2h-5zm-4 3h9v1h-9V5zm0 3h9v1h-9V8zm0 3h6v1h-6v-1z"/>
          </svg>
          <span>批量任务中心</span>
          <span 
            v-if="batchStore.activeTaskCount > 0"
            class="sl-tab-counter-badge"
          >
            {{ batchStore.activeTaskCount }}
          </span>
        </button>
      </div>
    </div>

    <!-- 右侧原生服务连通性与辅助操作 -->
    <div class="sl-nav-actions">
      <div class="sl-badge" :class="systemStore.isReady ? 'sl-badge--online' : 'sl-badge--offline'">
        <span class="sl-badge-dot"></span>
        <span v-if="systemStore.isReady">
          C++ Core · {{ availableEngineNames }} · FFmpeg Ready
        </span>
        <span v-else-if="systemStore.isLoading">正在连接 C++ 服务端...</span>
        <span v-else>服务离线 ({{ systemStore.error || '无法连接' }})</span>
      </div>

      <button 
        v-if="currentView === 'workbench' && workbenchStore.state !== 'Empty'"
        class="sl-button-secondary"
        :disabled="workbenchStore.isLocked"
        title="清除当前视频重新导入"
        @click="workbenchStore.reset()"
      >
        <svg class="sl-icon" viewBox="0 0 16 16" fill="currentColor">
          <path d="M8 3a5 5 0 1 0 4.546 2.914.5.5 0 0 1 .908-.417A6 6 0 1 1 8 2v1z"/>
          <path d="M8 4.466V.534a.25.25 0 0 1 .41-.192l2.36 1.966c.12.1.12.284 0 .384L8.41 4.658A.25.25 0 0 1 8 4.466z"/>
        </svg>
        <span>更换视频</span>
      </button>
    </div>
  </header>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useSystemStore } from '../stores/system';
import { useWorkbenchStore } from '../stores/workbench';
import { useBatchStore } from '../stores/batch';

defineProps<{
  currentView: 'workbench' | 'batch';
}>();

const emit = defineEmits<{
  (e: 'update:view', view: 'workbench' | 'batch'): void;
}>();

const systemStore = useSystemStore();
const workbenchStore = useWorkbenchStore();
const batchStore = useBatchStore();

const availableEngineNames = computed(() => {
  if (systemStore.availableEngines.length === 0) return 'Mock';
  return systemStore.availableEngines.map((e) => e.name).join('/');
});
</script>

<style scoped>
.sl-navbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  height: 48px;
  padding: 0 16px;
  background: var(--sl-glass-nav);
  backdrop-filter: var(--sl-glass-blur);
  -webkit-backdrop-filter: var(--sl-glass-blur);
  border-bottom: 1px solid var(--sl-border-subtle);
  box-shadow: var(--sl-inner-highlight);
  position: relative;
  z-index: 100;
}

.sl-logo-group, .sl-nav-actions {
  display: flex;
  align-items: center;
  gap: 10px;
}

.sl-logo-icon {
  width: 24px;
  height: 24px;
  border-radius: var(--sl-radius-xs);
}

.sl-logo-title {
  font-size: var(--sl-font-size-md);
  font-weight: 600;
  letter-spacing: -0.02em;
  color: var(--sl-text-primary);
}

.sl-nav-center {
  display: flex;
  align-items: center;
}

.sl-nav-segmented-control {
  display: flex;
  align-items: center;
  background: var(--sl-surface-base);
  border: 1px solid var(--sl-border-subtle);
  padding: 2px;
  border-radius: var(--sl-radius-md);
  box-shadow: var(--sl-inner-shadow);
}

.sl-segment-btn {
  display: flex;
  align-items: center;
  gap: 6px;
  height: 28px;
  padding: 0 12px;
  border: none;
  border-radius: var(--sl-radius-sm);
  background: transparent;
  color: var(--sl-text-secondary);
  font-size: var(--sl-font-size-xs);
  font-weight: 500;
  cursor: pointer;
  transition: var(--sl-transition-snappy);
}

.sl-segment-btn:hover {
  color: var(--sl-text-primary);
}

.sl-segment-btn.is-active {
  background: var(--sl-surface-card);
  color: var(--sl-text-primary);
  box-shadow: var(--sl-shadow-sm), var(--sl-inner-highlight);
}

.sl-seg-icon {
  width: 13px;
  height: 13px;
  opacity: 0.8;
}

.sl-tab-counter-badge {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-width: 16px;
  height: 16px;
  padding: 0 4px;
  border-radius: var(--sl-radius-pill);
  background: var(--sl-color-accent);
  color: #fff;
  font-size: 10px;
  font-weight: 600;
  font-family: var(--sl-font-family-mono);
}

.sl-badge {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 3px 10px;
  border-radius: var(--sl-radius-pill);
  font-family: var(--sl-font-family-mono);
  font-size: var(--sl-font-size-xs);
  background: var(--sl-surface-card);
  border: 1px solid var(--sl-border-subtle);
  color: var(--sl-text-secondary);
}

.sl-badge-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
}

.sl-badge--online .sl-badge-dot {
  background-color: var(--sl-color-success);
  box-shadow: 0 0 6px var(--sl-color-success);
}

.sl-badge--offline .sl-badge-dot {
  background-color: var(--sl-color-error);
}

.sl-button-secondary {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  height: 28px;
  padding: 0 10px;
  background: var(--sl-surface-card);
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-sm);
  color: var(--sl-text-secondary);
  font-size: var(--sl-font-size-xs);
  font-weight: 500;
  cursor: pointer;
  transition: var(--sl-transition-snappy);
}

.sl-button-secondary:hover:not(:disabled) {
  background: var(--sl-surface-card-hover);
  color: var(--sl-text-primary);
  border-color: var(--sl-border-focus);
}

.sl-button-secondary:disabled {
  opacity: 0.35;
  cursor: not-allowed;
}

.sl-icon {
  width: 12px;
  height: 12px;
}
</style>
