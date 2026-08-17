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

    <!-- 右侧原生服务连通性、工作区与辅助操作 -->
    <div class="sl-nav-actions">
      <!-- 工作区目录状态胶囊 (Feature 12508) -->
      <button
        class="sl-workspace-capsule"
        :title="`当前工作区：${systemStore.currentMediaDir || '未配置'}（点击切换）`"
        @click="openSwitchModal"
      >
        <svg class="sl-ws-icon" viewBox="0 0 16 16" fill="currentColor">
          <path d="M.54 3.87.5 3a2 2 0 0 1 2-2h3.672a2 2 0 0 1 1.414.586l.828.828A2 2 0 0 0 9.828 3h3.982a2 2 0 0 1 1.992 2.181l-.637 7A2 2 0 0 1 13.174 14H2.826a2 2 0 0 1-1.991-1.819l-.637-7a1.99 1.99 0 0 1 .342-1.31zM2.19 4a1 1 0 0 0-.996 1.09l.637 7a1 1 0 0 0 .995.91h10.348a1 1 0 0 0 .995-.91l.637-7A1 1 0 0 0 13.81 4H2.19zm4.69-1.707A1 1 0 0 0 6.172 2H2.5a1 1 0 0 0-1 .981l.006.139C1.72 3.042 1.95 3 2.19 3h11.62c.24 0 .47.042.684.12l.006-.139A1 1 0 0 0 13.5 2H9.828a1 1 0 0 0-.707-.293l-.828-.828A1 1 0 0 0 7.586 1H6.172a1 1 0 0 0-.707.293l-.585.586z"/>
        </svg>
        <span class="sl-ws-text">{{ shortMediaDir }}</span>
        <span v-if="systemStore.workspace.video_count > 0" class="sl-ws-count">{{ systemStore.workspace.video_count }} 视频</span>
      </button>

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

    <!-- 切换工作区模态弹窗 -->
    <div v-if="isSwitchModalOpen" class="sl-modal-overlay" @click.self="isSwitchModalOpen = false">
      <div class="sl-modal-card">
        <div class="sl-modal-header">
          <h2 class="sl-modal-title">切换视频处理工作区</h2>
          <button class="sl-modal-close" @click="isSwitchModalOpen = false">×</button>
        </div>
        <p class="sl-modal-desc">
          指定新的视频工作目录，服务端将就近管理 <code>.sublift_cache</code> 并在该目录下极速定位视频。
        </p>
        <div class="sl-modal-body">
          <input
            v-model="switchPathInput"
            type="text"
            placeholder="例如: /Volumes/Data/videos 或 ~/Movies"
            class="sl-modal-input"
            autofocus
            @keydown.enter="handleConfirmSwitch"
          />
          <div class="sl-modal-presets">
            <button
              v-for="p in ['~/Movies', '~/Downloads', '~/Desktop', '~/Documents']"
              :key="p"
              type="button"
              class="sl-modal-preset-chip"
              @click="switchPathInput = p"
            >
              {{ p }}
            </button>
          </div>
          <div v-if="switchError" class="sl-modal-error">
            {{ switchError }}
          </div>
        </div>
        <div class="sl-modal-footer">
          <button class="sl-modal-btn-cancel" @click="isSwitchModalOpen = false">取消</button>
          <button
            class="sl-modal-btn-confirm"
            :disabled="!switchPathInput.trim() || isSwitching"
            @click="handleConfirmSwitch"
          >
            {{ isSwitching ? '正在切换…' : '确认切换' }}
          </button>
        </div>
      </div>
    </div>
  </header>
</template>

<script setup lang="ts">
import { computed, ref, onMounted, onUnmounted } from 'vue';
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

const isSwitchModalOpen = ref(false);
const switchPathInput = ref('');
const isSwitching = ref(false);
const switchError = ref<string | null>(null);

const shortMediaDir = computed(() => {
  const dir = systemStore.currentMediaDir;
  if (!dir) return '未设置工作区';
  const parts = dir.split(/[/\\]/);
  return parts.length > 1 ? parts[parts.length - 1] || dir : dir;
});

function openSwitchModal() {
  switchPathInput.value = systemStore.currentMediaDir || '';
  switchError.value = null;
  isSwitchModalOpen.value = true;
}

function handleKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape' && isSwitchModalOpen.value) {
    isSwitchModalOpen.value = false;
  }
}

onMounted(() => {
  window.addEventListener('keydown', handleKeydown);
});

onUnmounted(() => {
  window.removeEventListener('keydown', handleKeydown);
});

async function handleConfirmSwitch() {
  const path = switchPathInput.value.trim();
  if (!path || isSwitching.value) return;

  isSwitching.value = true;
  switchError.value = null;
  const res = await systemStore.setWorkspace(path);
  if (res.success) {
    isSwitchModalOpen.value = false;
  } else {
    switchError.value = res.error || '切换工作区失败';
  }
  isSwitching.value = false;
}

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

/* 工作区胶囊样式 (Feature 12508) */
.sl-workspace-capsule {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  height: 28px;
  padding: 0 10px;
  background: rgba(10, 132, 255, 0.08);
  border: 1px solid rgba(10, 132, 255, 0.25);
  border-radius: var(--sl-radius-sm);
  color: var(--sl-text-primary);
  font-size: var(--sl-font-size-xs);
  cursor: pointer;
  transition: var(--sl-transition-snappy);
}

.sl-workspace-capsule:hover {
  background: rgba(10, 132, 255, 0.16);
  border-color: var(--sl-primary);
  box-shadow: 0 0 8px rgba(10, 132, 255, 0.2);
}

.sl-ws-icon {
  width: 13px;
  height: 13px;
  color: var(--sl-primary);
}

.sl-ws-text {
  font-weight: 500;
  max-width: 140px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.sl-ws-count {
  font-size: 10.5px;
  background: rgba(255, 255, 255, 0.1);
  padding: 1px 5px;
  border-radius: 10px;
  color: var(--sl-text-secondary);
}

/* 切换工作区模态弹窗 */
.sl-modal-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.65);
  backdrop-filter: blur(8px);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
}

.sl-modal-card {
  width: 100%;
  max-width: 520px;
  background: var(--sl-surface-base);
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-lg);
  padding: 24px;
  box-shadow: 0 20px 40px rgba(0, 0, 0, 0.5);
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.sl-modal-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.sl-modal-title {
  font-size: 16px;
  font-weight: 600;
  color: var(--sl-text-primary);
  margin: 0;
}

.sl-modal-close {
  background: transparent;
  border: none;
  color: var(--sl-text-tertiary);
  font-size: 20px;
  cursor: pointer;
  padding: 0 4px;
}

.sl-modal-close:hover {
  color: var(--sl-text-primary);
}

.sl-modal-desc {
  font-size: 13px;
  color: var(--sl-text-secondary);
  margin: 0;
  line-height: 1.5;
}

.sl-modal-desc code {
  background: rgba(255, 255, 255, 0.08);
  padding: 1px 5px;
  border-radius: 3px;
  color: var(--sl-text-primary);
}

.sl-modal-body {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.sl-modal-input {
  width: 100%;
  height: 40px;
  padding: 0 12px;
  background: rgba(255, 255, 255, 0.04);
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-sm);
  color: var(--sl-text-primary);
  font-size: 13.5px;
  font-family: var(--sl-font-family-mono);
  outline: none;
}

.sl-modal-input:focus {
  border-color: var(--sl-color-accent);
  box-shadow: 0 0 0 2px rgba(10, 132, 255, 0.25);
}

.sl-modal-presets {
  display: flex;
  gap: 6px;
  flex-wrap: wrap;
}

.sl-modal-preset-chip {
  background: rgba(255, 255, 255, 0.05);
  border: 1px solid rgba(255, 255, 255, 0.08);
  color: var(--sl-text-secondary);
  border-radius: 12px;
  padding: 3px 8px;
  font-size: 11px;
  cursor: pointer;
}

.sl-modal-preset-chip:hover {
  background: rgba(255, 255, 255, 0.1);
  color: var(--sl-text-primary);
}

.sl-modal-error {
  font-size: 12px;
  color: var(--sl-color-error);
  padding: 6px 10px;
  background: var(--sl-color-error-bg);
  border-radius: var(--sl-radius-xs);
}

.sl-modal-footer {
  display: flex;
  justify-content: flex-end;
  gap: 10px;
  padding-top: 8px;
}

.sl-modal-btn-cancel {
  padding: 6px 14px;
  background: transparent;
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-sm);
  color: var(--sl-text-secondary);
  font-size: 13px;
  cursor: pointer;
}

.sl-modal-btn-cancel:hover {
  background: rgba(255, 255, 255, 0.05);
  color: var(--sl-text-primary);
}

.sl-modal-btn-confirm {
  padding: 6px 16px;
  background: var(--sl-color-accent);
  border: none;
  border-radius: var(--sl-radius-sm);
  color: #ffffff;
  font-size: 13px;
  font-weight: 500;
  cursor: pointer;
}

.sl-modal-btn-confirm:hover:not(:disabled) {
  background: var(--sl-color-accent-hover);
}

.sl-modal-btn-confirm:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}
</style>
