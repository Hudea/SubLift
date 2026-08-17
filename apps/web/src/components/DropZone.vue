<template>
  <div class="sl-video-selector">
    <div class="sl-selector-container">
      <!-- 顶部模式说明与工作区胶囊 -->
      <div class="sl-selector-header">
        <div class="sl-header-icon-box">
          <svg class="sl-header-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor">
            <rect x="2" y="2" width="20" height="20" rx="4" stroke-width="1.8"/>
            <polygon points="10 8 16 12 10 16 10 8" fill="currentColor" stroke="none"/>
          </svg>
        </div>
        <h2 class="sl-selector-title">载入单视频进行字幕提取</h2>
        <p class="sl-selector-subtitle">
          单视频模式需指定具体物理文件：请从当前工作区选择，或直接输入本地绝对路径。
        </p>
      </div>

      <!-- 双通道选择区（等高对称双卡片） -->
      <div class="sl-selector-grid">
        <!-- 通道 1：从工作区选择 -->
        <div class="sl-channel-card">
          <div class="sl-card-header">
            <div class="sl-card-title-group">
              <span class="sl-channel-tag">通道一</span>
              <h3 class="sl-card-title">从工作区选择视频</h3>
            </div>
            <button
              type="button"
              class="sl-refresh-btn"
              :disabled="isLoadingVideos"
              title="刷新工作区视频列表"
              @click="fetchVideos"
            >
              <svg class="sl-refresh-icon" :class="{ 'is-spinning': isLoadingVideos }" viewBox="0 0 16 16" fill="currentColor">
                <path d="M8 3a5 5 0 1 0 4.546 2.914.5.5 0 0 1 .908-.417A6 6 0 1 1 8 2v1z"/>
                <path d="M8 4.466V.534a.25.25 0 0 1 .41-.192l2.36 1.966c.12.1.12.284 0 .384L8.41 4.658A.25.25 0 0 1 8 4.466z"/>
              </svg>
            </button>
          </div>

          <div class="sl-ws-indicator" :title="systemStore.currentMediaDir">
            <svg class="sl-ws-indicator-icon" viewBox="0 0 16 16" fill="currentColor">
              <path d="M.54 3.87.5 3a2 2 0 0 1 2-2h3.672a2 2 0 0 1 1.414.586l.828.828A2 2 0 0 0 9.828 3h3.982a2 2 0 0 1 1.992 2.181l-.637 7A2 2 0 0 1 13.174 14H2.826a2 2 0 0 1-1.991-1.819l-.637-7a1.99 1.99 0 0 1 .342-1.31zM2.19 4a1 1 0 0 0-.996 1.09l.637 7a1 1 0 0 0 .995.91h10.348a1 1 0 0 0 .995-.91l.637-7A1 1 0 0 0 13.81 4H2.19zm4.69-1.707A1 1 0 0 0 6.172 2H2.5a1 1 0 0 0-1 .981l.006.139C1.72 3.042 1.95 3 2.19 3h11.62c.24 0 .47.042.684.12l.006-.139A1 1 0 0 0 13.5 2H9.828a1 1 0 0 0-.707-.293l-.828-.828A1 1 0 0 0 7.586 1H6.172a1 1 0 0 0-.707.293l-.585.586z"/>
            </svg>
            <span class="sl-ws-indicator-path">{{ systemStore.currentMediaDir || '未配置工作区' }}</span>
          </div>

          <!-- 搜索过滤 -->
          <div class="sl-search-box">
            <input
              v-model="searchQuery"
              type="text"
              placeholder="搜索工作区内的视频..."
              class="sl-search-input"
            />
          </div>

          <!-- 视频列表 -->
          <div class="sl-video-list-box">
            <div v-if="isLoadingVideos" class="sl-empty-state">
              <span>正在扫描工作区视频…</span>
            </div>

            <div v-else-if="filteredVideos.length === 0" class="sl-empty-state">
              <span v-if="workspaceVideos.length === 0">当前工作区内未发现支持的视频文件</span>
              <span v-else>无匹配搜索结果</span>
            </div>

            <div v-else class="sl-video-items">
              <button
                v-for="vid in filteredVideos"
                :key="vid.path"
                type="button"
                class="sl-video-item"
                @click="selectWorkspaceVideo(vid)"
              >
                <div class="sl-video-item-icon">
                  <svg viewBox="0 0 16 16" fill="currentColor">
                    <path d="M0 2a2 2 0 0 1 2-2h12a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2V2zm11.5 5.5-5-3A.5.5 0 0 0 5.75 5v6a.5.5 0 0 0 .75.433l5-3a.5.5 0 0 0 0-.866z"/>
                  </svg>
                </div>
                <div class="sl-video-item-info">
                  <span class="sl-video-name" :title="vid.name">{{ vid.name }}</span>
                  <span class="sl-video-meta">{{ vid.relative_path }} · {{ formatSize(vid.size_bytes) }}</span>
                </div>
                <span class="sl-select-badge">选择载入</span>
              </button>
            </div>
          </div>
        </div>

        <!-- 通道 2：粘贴绝对路径 -->
        <div class="sl-channel-card sl-channel-card-manual">
          <div class="sl-card-header">
            <div class="sl-card-title-group">
              <span class="sl-channel-tag">通道二</span>
              <h3 class="sl-card-title">粘贴绝对路径视频</h3>
            </div>
          </div>

          <p class="sl-card-desc">
            若视频位于工作区之外，直接粘贴该视频在系统上的完整物理绝对路径：
          </p>

          <form class="sl-manual-form" @submit.prevent="submitManualPath">
            <div class="sl-manual-input-wrapper">
              <label class="sl-manual-label">视频绝对路径</label>
              <div class="sl-manual-input-group">
                <svg class="sl-input-icon" viewBox="0 0 16 16" fill="currentColor">
                  <path d="M14 4.5V14a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V2a2 2 0 0 1 2-2h5.5L14 4.5zm-3 0A1.5 1.5 0 0 1 9.5 3V1H4a1 1 0 0 0-1 1v12a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1V4.5h-2z"/>
                </svg>
                <input
                  v-model="manualPath"
                  type="text"
                  placeholder="例如: /Users/name/Movies/demo.mp4"
                  class="sl-manual-input"
                  autofocus
                />
              </div>
            </div>

            <div v-if="manualError" class="sl-manual-error">
              {{ manualError }}
            </div>

            <button
              type="submit"
              class="sl-manual-submit-btn"
              :disabled="!manualPath.trim()"
            >
              载入视频进入工作台
            </button>
          </form>

          <div class="sl-card-footer">
            <div class="sl-tip-line">
              <span class="sl-tip-badge">快捷操作</span>
              <span>macOS Finder 选中文件按 <code>⌥⌘C</code> 即可直接复制绝对路径并粘贴于上方。</span>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from 'vue';
import { useWorkbenchStore } from '../stores/workbench';
import { useSystemStore } from '../stores/system';
import { SubLiftApiClient } from '../api/client';
import type { WorkspaceVideoFileDTO } from '../types/api';

const workbenchStore = useWorkbenchStore();
const systemStore = useSystemStore();

const workspaceVideos = ref<WorkspaceVideoFileDTO[]>([]);
const isLoadingVideos = ref(false);
const searchQuery = ref('');
const manualPath = ref('');
const manualError = ref<string | null>(null);

const filteredVideos = computed(() => {
  const q = searchQuery.value.trim().toLowerCase();
  if (!q) return workspaceVideos.value;
  return workspaceVideos.value.filter(
    (v) => v.name.toLowerCase().includes(q) || v.relative_path.toLowerCase().includes(q)
  );
});

async function fetchVideos() {
  if (!systemStore.isWorkspaceConfigured) return;
  isLoadingVideos.value = true;
  try {
    const list = await SubLiftApiClient.getWorkspaceVideos();
    workspaceVideos.value = list;
  } catch {
    workspaceVideos.value = [];
  } finally {
    isLoadingVideos.value = false;
  }
}

function selectWorkspaceVideo(vid: WorkspaceVideoFileDTO) {
  workbenchStore.loadVideo(vid.path, vid.name);
}

function submitManualPath() {
  const path = manualPath.value.trim();
  if (!path) return;
  manualError.value = null;
  workbenchStore.loadVideo(path);
}

function formatSize(bytes: number): string {
  if (!bytes || bytes <= 0) return '0 B';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

onMounted(() => {
  fetchVideos();
});
</script>

<style scoped>
.sl-video-selector {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 100%;
  padding: 40px 24px;
}

.sl-selector-container {
  width: 100%;
  max-width: 1000px;
  display: flex;
  flex-direction: column;
  gap: 28px;
}

.sl-selector-header {
  display: flex;
  flex-direction: column;
  align-items: center;
  text-align: center;
  gap: 8px;
}

.sl-header-icon-box {
  width: 52px;
  height: 52px;
  border-radius: var(--sl-radius-md);
  background: var(--sl-color-ready-bg);
  border: 1px solid rgba(10, 132, 255, 0.25);
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--sl-color-accent);
  margin-bottom: 4px;
  box-shadow: var(--sl-shadow-sm);
}

.sl-header-icon {
  width: 26px;
  height: 26px;
}

.sl-selector-title {
  font-size: 22px;
  font-weight: 600;
  color: var(--sl-text-primary);
  letter-spacing: -0.02em;
  margin: 0;
}

.sl-selector-subtitle {
  font-size: 14px;
  color: var(--sl-text-secondary);
  margin: 0;
  max-width: 600px;
  line-height: 1.5;
}

.sl-selector-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 24px;
  align-items: stretch;
}

@media (max-width: 820px) {
  .sl-selector-grid {
    grid-template-columns: 1fr;
  }
}

.sl-channel-card {
  background: var(--sl-surface-base);
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-lg);
  padding: 24px;
  display: flex;
  flex-direction: column;
  gap: 16px;
  box-shadow: var(--sl-shadow-md);
  min-height: 380px;
}

.sl-channel-card-manual {
  justify-content: space-between;
}

.sl-card-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.sl-card-title-group {
  display: flex;
  align-items: center;
  gap: 8px;
}

.sl-channel-tag {
  font-size: 11px;
  font-weight: 600;
  background: rgba(255, 255, 255, 0.08);
  padding: 2px 7px;
  border-radius: var(--sl-radius-xs);
  color: var(--sl-text-secondary);
}

.sl-card-title {
  font-size: 16px;
  font-weight: 600;
  color: var(--sl-text-primary);
  margin: 0;
}

.sl-refresh-btn {
  background: transparent;
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-xs);
  color: var(--sl-text-secondary);
  width: 30px;
  height: 30px;
  display: flex;
  align-items: center;
  justify-content: center;
  cursor: pointer;
  transition: var(--sl-transition-snappy);
}

.sl-refresh-btn:hover:not(:disabled) {
  background: rgba(255, 255, 255, 0.08);
  color: var(--sl-text-primary);
  border-color: var(--sl-color-accent);
}

.sl-refresh-icon {
  width: 14px;
  height: 14px;
}

.sl-refresh-icon.is-spinning {
  animation: sl-spin 0.8s linear infinite;
}

@keyframes sl-spin {
  to { transform: rotate(360deg); }
}

.sl-ws-indicator {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 12px;
  background: rgba(255, 255, 255, 0.03);
  border: 1px solid var(--sl-border-subtle);
  border-radius: var(--sl-radius-sm);
  font-size: 12px;
  color: var(--sl-text-secondary);
}

.sl-ws-indicator-icon {
  width: 13px;
  height: 13px;
  color: var(--sl-color-accent);
  flex-shrink: 0;
}

.sl-ws-indicator-path {
  font-family: var(--sl-font-family-mono);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.sl-search-input {
  width: 100%;
  height: 34px;
  padding: 0 12px;
  background: rgba(255, 255, 255, 0.04);
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-sm);
  color: var(--sl-text-primary);
  font-size: 12.5px;
  outline: none;
}

.sl-search-input:focus {
  border-color: var(--sl-color-accent);
  box-shadow: 0 0 0 2px rgba(10, 132, 255, 0.2);
}

.sl-video-list-box {
  height: 240px;
  overflow-y: auto;
  border: 1px solid var(--sl-border-subtle);
  border-radius: var(--sl-radius-sm);
  background: rgba(0, 0, 0, 0.25);
}

.sl-empty-state {
  display: flex;
  align-items: center;
  justify-content: center;
  height: 100%;
  font-size: 13px;
  color: var(--sl-text-tertiary);
  padding: 20px;
  text-align: center;
}

.sl-video-items {
  display: flex;
  flex-direction: column;
}

.sl-video-item {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px;
  background: transparent;
  border: none;
  border-bottom: 1px solid var(--sl-border-subtle);
  text-align: left;
  cursor: pointer;
  transition: var(--sl-transition-snappy);
}

.sl-video-item:last-child {
  border-bottom: none;
}

.sl-video-item:hover {
  background: rgba(255, 255, 255, 0.06);
}

.sl-video-item-icon {
  width: 28px;
  height: 28px;
  border-radius: var(--sl-radius-xs);
  background: rgba(10, 132, 255, 0.12);
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--sl-color-accent);
  flex-shrink: 0;
}

.sl-video-item-icon svg {
  width: 14px;
  height: 14px;
}

.sl-video-item-info {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.sl-video-name {
  font-size: 13.5px;
  font-weight: 500;
  color: var(--sl-text-primary);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.sl-video-meta {
  font-size: 11px;
  color: var(--sl-text-tertiary);
  font-family: var(--sl-font-family-mono);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.sl-select-badge {
  font-size: 11px;
  font-weight: 500;
  color: var(--sl-color-accent);
  background: var(--sl-color-ready-bg);
  padding: 4px 10px;
  border-radius: var(--sl-radius-xs);
  flex-shrink: 0;
}

.sl-card-desc {
  font-size: 13.5px;
  color: var(--sl-text-secondary);
  line-height: 1.5;
  margin: 0;
}

.sl-manual-form {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.sl-manual-input-wrapper {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.sl-manual-label {
  font-size: 12px;
  font-weight: 500;
  color: var(--sl-text-secondary);
}

.sl-manual-input-group {
  position: relative;
  display: flex;
  align-items: center;
  width: 100%;
}

.sl-input-icon {
  position: absolute;
  left: 14px;
  width: 16px;
  height: 16px;
  color: var(--sl-text-tertiary);
  pointer-events: none;
}

.sl-manual-input {
  width: 100%;
  height: 44px;
  padding: 0 14px 0 40px;
  background: rgba(255, 255, 255, 0.04);
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-md);
  color: var(--sl-text-primary);
  font-size: 13px;
  font-family: var(--sl-font-family-mono);
  outline: none;
  transition: var(--sl-transition-snappy);
}

.sl-manual-input:focus {
  border-color: var(--sl-color-accent);
  box-shadow: 0 0 0 2px rgba(10, 132, 255, 0.2);
}

.sl-manual-error {
  font-size: 12px;
  color: var(--sl-color-error);
  padding: 8px 12px;
  background: var(--sl-color-error-bg);
  border-radius: var(--sl-radius-xs);
}

.sl-manual-submit-btn {
  height: 44px;
  background: var(--sl-color-accent);
  color: #ffffff;
  border: none;
  border-radius: var(--sl-radius-md);
  font-size: 14px;
  font-weight: 600;
  cursor: pointer;
  transition: var(--sl-transition-smooth);
}

.sl-manual-submit-btn:hover:not(:disabled) {
  background: var(--sl-color-accent-hover);
}

.sl-manual-submit-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.sl-card-footer {
  border-top: 1px solid var(--sl-border-subtle);
  padding-top: 14px;
}

.sl-tip-line {
  display: flex;
  align-items: flex-start;
  gap: 8px;
  font-size: 12px;
  color: var(--sl-text-tertiary);
  line-height: 1.45;
}

.sl-tip-badge {
  font-size: 10px;
  font-weight: 600;
  background: rgba(255, 255, 255, 0.08);
  padding: 1px 5px;
  border-radius: 3px;
  color: var(--sl-text-secondary);
  flex-shrink: 0;
}

.sl-tip-line code {
  background: rgba(255, 255, 255, 0.08);
  padding: 1px 5px;
  border-radius: 3px;
  color: var(--sl-text-primary);
}
</style>
