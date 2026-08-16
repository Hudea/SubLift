<template>
  <div 
    class="sl-dropzone" 
    :class="{ 'is-dragover': isDragOver }"
    @dragover.prevent="isDragOver = true"
    @dragleave.prevent="isDragOver = false"
    @drop.prevent="handleDrop"
  >
    <div class="sl-dropzone-inner" @click="triggerFileInput">
      <div class="sl-dropzone-icon-box">
        <svg class="sl-dropzone-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor">
          <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" />
          <polyline points="17 8 12 3 7 8" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" />
          <line x1="12" y1="3" x2="12" y2="15" stroke-width="1.8" stroke-linecap="round" />
        </svg>
      </div>
      
      <p class="sl-dropzone-title">拖拽视频到此处，或点击浏览文件</p>
      <p class="sl-dropzone-hint">支持 MP4, MOV, WebM, MKV, AVI (智能极速转封装与分片直出)</p>

      <input 
        ref="fileInputRef"
        type="file" 
        accept="video/mp4,video/quicktime,video/webm,video/x-matroska,video/x-msvideo,.mp4,.mov,.webm,.mkv,.avi" 
        style="display: none"
        @change="handleFileSelected"
      />
    </div>

    <!-- 本地绝对路径直接输入区 -->
    <div class="sl-path-input-group" @click.stop>
      <div class="sl-path-input-wrapper">
        <svg class="sl-path-icon" viewBox="0 0 16 16" fill="currentColor">
          <path d="M14 4.5V14a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V2a2 2 0 0 1 2-2h5.5L14 4.5zm-3 0A1.5 1.5 0 0 1 9.5 3V1H4a1 1 0 0 0-1 1v12a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1V4.5h-2z"/>
        </svg>
        <input 
          v-model="manualPath"
          type="text" 
          placeholder="或者直接粘贴本地视频绝对路径 (例如: /Users/name/video.mp4)"
          class="sl-path-input"
          @keydown.enter="submitManualPath"
        />
        <button 
          class="sl-path-submit" 
          :disabled="!manualPath.trim()"
          @click="submitManualPath"
        >
          载入
        </button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { useWorkbenchStore } from '../stores/workbench';

const workbenchStore = useWorkbenchStore();

const isDragOver = ref(false);
const manualPath = ref('');
const fileInputRef = ref<HTMLInputElement | null>(null);

function triggerFileInput() {
  fileInputRef.value?.click();
}

function handleFileSelected(e: Event) {
  const target = e.target as HTMLInputElement;
  const file = target.files?.[0];
  if (file) {
    // If native WebKit / Electron path is accessible
    const nativePath = (file as any).path;
    if (nativePath) {
      workbenchStore.loadVideo(nativePath, file.name);
    } else {
      // In web mode, try file name or fallback
      workbenchStore.loadVideo(file.name, file.name);
    }
  }
}

function handleDrop(e: DragEvent) {
  isDragOver.value = false;
  const file = e.dataTransfer?.files?.[0];
  if (file) {
    const nativePath = (file as any).path;
    if (nativePath) {
      workbenchStore.loadVideo(nativePath, file.name);
    } else {
      workbenchStore.loadVideo(file.name, file.name);
    }
  }
}

function submitManualPath() {
  if (manualPath.value.trim()) {
    workbenchStore.loadVideo(manualPath.value.trim());
  }
}
</script>

<style scoped>
.sl-dropzone {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 100%;
  height: 100%;
  background: var(--sl-surface-base);
  border-radius: var(--sl-radius-lg);
  border: 1.5px dashed var(--sl-border-focus);
  padding: 32px 24px;
  transition: var(--sl-transition-smooth);
  position: relative;
}

.sl-dropzone:hover,
.sl-dropzone.is-dragover {
  border-color: var(--sl-color-accent);
  background: var(--sl-color-accent-muted);
}

.sl-dropzone-inner {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  cursor: pointer;
  width: 100%;
  max-width: 480px;
  margin-bottom: 24px;
}

.sl-dropzone-icon-box {
  width: 56px;
  height: 56px;
  border-radius: var(--sl-radius-md);
  background: var(--sl-surface-card);
  border: 1px solid var(--sl-border-standard);
  display: flex;
  align-items: center;
  justify-content: center;
  margin-bottom: 16px;
  color: var(--sl-color-accent);
  box-shadow: var(--sl-shadow-sm), var(--sl-inner-highlight);
  transition: var(--sl-transition-snappy);
}

.sl-dropzone:hover .sl-dropzone-icon-box {
  transform: translateY(-2px);
  box-shadow: var(--sl-shadow-glow-accent);
}

.sl-dropzone-icon {
  width: 28px;
  height: 28px;
}

.sl-dropzone-title {
  font-size: var(--sl-font-size-md);
  font-weight: 500;
  color: var(--sl-text-primary);
  margin-bottom: 6px;
}

.sl-dropzone-hint {
  font-size: var(--sl-font-size-xs);
  color: var(--sl-text-tertiary);
  margin-bottom: 8px;
}

.sl-path-input-group {
  width: 100%;
  max-width: 560px;
}

.sl-path-input-wrapper {
  display: flex;
  align-items: center;
  background: var(--sl-surface-card);
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-md);
  padding: 4px 6px 4px 12px;
  box-shadow: var(--sl-inner-shadow);
  transition: var(--sl-transition-snappy);
}

.sl-path-input-wrapper:focus-within {
  border-color: var(--sl-color-accent);
  box-shadow: 0 0 0 2px var(--sl-color-accent-muted);
}

.sl-path-icon {
  width: 14px;
  height: 14px;
  color: var(--sl-text-tertiary);
  margin-right: 8px;
  flex-shrink: 0;
}

.sl-path-input {
  flex: 1;
  border: none;
  background: transparent;
  color: var(--sl-text-primary);
  font-size: var(--sl-font-size-sm);
  outline: none;
}

.sl-path-input::placeholder {
  color: var(--sl-text-tertiary);
}

.sl-path-submit {
  height: 28px;
  padding: 0 12px;
  border: none;
  border-radius: var(--sl-radius-xs);
  background: var(--sl-color-accent);
  color: #ffffff;
  font-size: var(--sl-font-size-xs);
  font-weight: 500;
  cursor: pointer;
  transition: var(--sl-transition-snappy);
}

.sl-path-submit:hover:not(:disabled) {
  background: var(--sl-color-accent-hover);
}

.sl-path-submit:disabled {
  opacity: 0.35;
  cursor: not-allowed;
}
</style>
