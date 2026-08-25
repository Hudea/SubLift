<template>
  <main class="sl-setup-wrapper" role="main">
    <div class="sl-setup-card" role="region" aria-labelledby="sl-setup-title">
      <div class="sl-setup-header">
        <div class="sl-setup-icon-box" aria-hidden="true">
          <svg class="sl-setup-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor">
            <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>
          </svg>
        </div>
        <h1 id="sl-setup-title" class="sl-setup-title">请指定视频所在的文件夹路径：</h1>
        <p id="sl-setup-desc" class="sl-setup-desc">
          指定媒体处理工作区后，该目录及子目录内的视频可直接原地零拷贝极速提取，并在该目录下就近创建 <code>.sublift_cache</code> 缓存。
        </p>
      </div>

      <form class="sl-setup-form" aria-describedby="sl-setup-desc" @submit.prevent="handleSetup">
        <div class="sl-input-group">
          <svg class="sl-input-icon" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
            <path d="M14 4.5V14a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V2a2 2 0 0 1 2-2h5.5L14 4.5zm-3 0A1.5 1.5 0 0 1 9.5 3V1H4a1 1 0 0 0-1 1v12a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1V4.5h-2z"/>
          </svg>
          <label for="workspace-path-input" class="sl-sr-only">视频工作区文件夹路径</label>
          <input
            id="workspace-path-input"
            v-model="inputPath"
            type="text"
            placeholder="例如: /Users/name/Movies 或 /Volumes/Data/videos"
            class="sl-setup-input"
            aria-label="视频工作区文件夹路径"
            aria-required="true"
            autofocus
            :disabled="isSubmitting"
            @keydown.enter="handleSetup"
          />
        </div>

        <!-- 快捷填入预设 -->
        <div class="sl-presets-row" role="group" aria-label="常用建议预设路径">
          <span class="sl-presets-label">常用建议：</span>
          <div class="sl-preset-chips">
            <button
              v-for="preset in presets"
              :key="preset.path"
              type="button"
              class="sl-preset-chip"
              :aria-label="`快捷填入 ${preset.label}`"
              :disabled="isSubmitting"
              @click="inputPath = preset.path"
            >
              {{ preset.label }}
            </button>
          </div>
        </div>

        <!-- 错误提示 -->
        <div v-if="errorMessage" class="sl-setup-error" role="alert" aria-live="assertive">
          <svg class="sl-error-icon" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
            <path d="M8 15A7 7 0 1 1 8 1a7 7 0 0 1 0 14zm0 1A8 8 0 1 0 8 0a8 8 0 0 0 0 16z"/>
            <path d="M7.002 11a1 1 0 1 1 2 0 1 1 0 0 1-2 0zM7.1 4.995a.905.905 0 1 1 1.8 0l-.35 3.507a.552.552 0 0 1-1.1 0L7.1 4.995z"/>
          </svg>
          <span>{{ errorMessage }}</span>
        </div>

        <!-- 提交按钮 -->
        <button
          type="button"
          class="sl-setup-submit-btn"
          aria-label="进入工作台"
          :disabled="!inputPath.trim() || isSubmitting"
          @click="handleSetup"
        >
          <span v-if="!isSubmitting">进入工作台</span>
          <span v-else>正在初始化工作区…</span>
        </button>
      </form>

      <div class="sl-setup-footer">
        <p>macOS 可在 Finder 选中文件夹后按 <code>⌥⌘C</code> 快速复制绝对路径</p>
      </div>
    </div>
  </main>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { useSystemStore } from '../stores/system';

const systemStore = useSystemStore();
const inputPath = ref('');
const isSubmitting = ref(false);
const errorMessage = ref<string | null>(null);

const presets = [
  { label: '~/Movies (影片)', path: '~/Movies' },
  { label: '~/Downloads (下载)', path: '~/Downloads' },
  { label: '~/Desktop (桌面)', path: '~/Desktop' },
  { label: '~/Documents (文稿)', path: '~/Documents' },
];

async function handleSetup() {
  const path = inputPath.value.trim();
  if (!path || isSubmitting.value) return;

  isSubmitting.value = true;
  errorMessage.value = null;

  const res = await systemStore.setWorkspace(path);
  if (!res.success) {
    errorMessage.value = res.error || '无法设置指定的媒体工作区目录';
    isSubmitting.value = false;
  }
}
</script>

<style scoped>
.sl-setup-wrapper {
  display: flex;
  align-items: center;
  justify-content: center;
  min-height: 100vh;
  width: 100vw;
  background: var(--sl-surface-ground);
  padding: 24px;
  overflow-y: auto;
  overflow-x: hidden;
}

.sl-setup-card {
  width: 100%;
  max-width: 640px;
  background: var(--sl-surface-base);
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-lg);
  padding: 40px 36px;
  box-shadow: 0 24px 48px rgba(0, 0, 0, 0.4), 0 0 0 1px rgba(255, 255, 255, 0.05);
  backdrop-filter: blur(20px);
  -webkit-backdrop-filter: blur(20px);
  display: flex;
  flex-direction: column;
  gap: 28px;
}

.sl-setup-header {
  display: flex;
  flex-direction: column;
  align-items: center;
  text-align: center;
  gap: 12px;
}

.sl-setup-icon-box {
  width: 56px;
  height: 56px;
  border-radius: 16px;
  background: var(--sl-color-ready-bg);
  border: 1px solid rgba(10, 132, 255, 0.25);
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--sl-color-accent);
  margin-bottom: 4px;
}

.sl-setup-icon {
  width: 28px;
  height: 28px;
}

.sl-setup-title {
  font-size: 22px;
  font-weight: 600;
  color: var(--sl-text-primary);
  letter-spacing: -0.02em;
  margin: 0;
}

.sl-setup-desc {
  font-size: 13.5px;
  color: var(--sl-text-secondary);
  line-height: 1.6;
  margin: 0;
}

.sl-setup-desc code {
  background: rgba(255, 255, 255, 0.08);
  padding: 2px 6px;
  border-radius: 4px;
  font-family: var(--sl-font-family-mono);
  color: var(--sl-text-primary);
}

.sl-setup-form {
  display: flex;
  flex-direction: column;
  gap: 18px;
}

.sl-input-group {
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

.sl-setup-input {
  width: 100%;
  height: 48px;
  padding: 0 16px 0 42px;
  background: rgba(255, 255, 255, 0.04);
  border: 1.5px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-md);
  color: var(--sl-text-primary);
  font-size: 14px;
  font-family: var(--sl-font-family-mono);
  outline: none;
  transition: var(--sl-transition-smooth);
}

.sl-setup-input:focus {
  border-color: var(--sl-color-accent);
  background: rgba(255, 255, 255, 0.07);
  box-shadow: 0 0 0 3px rgba(10, 132, 255, 0.2);
}

.sl-presets-row {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.sl-presets-label {
  font-size: 12px;
  color: var(--sl-text-tertiary);
}

.sl-preset-chips {
  display: flex;
  gap: 6px;
  flex-wrap: wrap;
}

.sl-preset-chip {
  background: rgba(255, 255, 255, 0.05);
  border: 1px solid rgba(255, 255, 255, 0.08);
  color: var(--sl-text-secondary);
  border-radius: 20px;
  padding: 4px 10px;
  font-size: 11.5px;
  cursor: pointer;
  transition: var(--sl-transition-smooth);
}

.sl-preset-chip:hover {
  background: rgba(255, 255, 255, 0.1);
  color: var(--sl-text-primary);
  border-color: rgba(255, 255, 255, 0.18);
}

.sl-setup-error {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 10px 14px;
  background: var(--sl-color-error-bg);
  border: 1px solid rgba(255, 69, 58, 0.3);
  border-radius: var(--sl-radius-md);
  color: var(--sl-color-error);
  font-size: 13px;
}

.sl-error-icon {
  width: 16px;
  height: 16px;
  flex-shrink: 0;
}

.sl-setup-submit-btn {
  width: 100%;
  height: 46px;
  background: var(--sl-color-accent);
  color: #ffffff;
  border: none;
  border-radius: var(--sl-radius-md);
  font-size: 15px;
  font-weight: 600;
  cursor: pointer;
  transition: var(--sl-transition-smooth);
  display: flex;
  align-items: center;
  justify-content: center;
}

.sl-setup-submit-btn:hover:not(:disabled) {
  background: var(--sl-color-accent-hover);
  box-shadow: 0 4px 12px rgba(10, 132, 255, 0.35);
}

.sl-setup-submit-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.sl-setup-footer {
  text-align: center;
  font-size: 12px;
  color: var(--sl-text-tertiary);
  border-top: 1px solid var(--sl-border-subtle);
  padding-top: 18px;
  margin-top: -6px;
}

.sl-setup-footer code {
  background: rgba(255, 255, 255, 0.08);
  padding: 1px 5px;
  border-radius: 3px;
  color: var(--sl-text-secondary);
}

.sl-sr-only {
  position: absolute;
  width: 1px;
  height: 1px;
  padding: 0;
  margin: -1px;
  overflow: hidden;
  clip: rect(0, 0, 0, 0);
  white-space: nowrap;
  border: 0;
}

@media (max-width: 600px) {
  .sl-setup-card {
    padding: 24px 20px;
    gap: 20px;
  }
}
</style>
