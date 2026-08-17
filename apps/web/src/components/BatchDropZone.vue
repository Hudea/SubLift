<template>
  <div 
    class="sl-batch-dropzone"
    :class="{ 'is-dragover': isDragOver }"
    @dragover.prevent="isDragOver = true"
    @dragleave.prevent="isDragOver = false"
    @drop.prevent="handleDrop"
  >
    <div class="sl-dropzone-left" @click="triggerFileInput">
      <div class="sl-icon-pill">
        <svg class="sl-upload-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor">
          <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" stroke-width="1.8" stroke-linecap="round"/>
          <polyline points="17 8 12 3 7 8" stroke-width="1.8" stroke-linecap="round"/>
          <line x1="12" y1="3" x2="12" y2="15" stroke-width="1.8" stroke-linecap="round"/>
        </svg>
      </div>
      <div>
        <div class="sl-drop-title">批量添加视频到队列</div>
        <div class="sl-drop-subtitle">点击选择多文件，或直接从工作区一键导入</div>
      </div>
      <button 
        type="button" 
        class="sl-ws-import-btn" 
        @click.stop="importAllFromWorkspace"
      >
        📁 导入工作区全部视频
      </button>
      <input 
        ref="fileInputRef" 
        type="file" 
        multiple 
        accept="video/mp4,video/quicktime,video/webm,video/x-matroska,video/x-msvideo,.mp4,.mov,.webm,.mkv,.avi"
        style="display: none"
        @change="handleFileSelect"
      />
    </div>

    <div class="sl-dropzone-divider"></div>

    <!-- 绝对路径多行批量录入内嵌框 -->
    <div class="sl-dropzone-right">
      <p v-if="webModeNotice" class="sl-batch-notice">{{ webModeNotice }}</p>
      <textarea
        v-model="batchPathText"
        placeholder="或直接粘贴本地视频绝对路径 (支持换行多路径):&#10;/path/to/video1.mp4&#10;/path/to/video2.mov"
        class="sl-batch-path-textarea"
        rows="2"
      ></textarea>
      <button 
        class="sl-batch-add-btn" 
        :disabled="!batchPathText.trim()"
        @click="submitBatchPaths"
      >
        加入队列
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { useBatchStore } from '../stores/batch';
import { SubLiftApiClient } from '../api/client';
import { computeFingerprint } from '../utils/fingerprint';

const batchStore = useBatchStore();
const isDragOver = ref(false);
const fileInputRef = ref<HTMLInputElement | null>(null);
const batchPathText = ref('');
const webModeNotice = ref('');

function triggerFileInput() {
  fileInputRef.value?.click();
}

function extractNativePaths(files: FileList | File[]): { paths: string[]; fallbackFiles: File[] } {
  const paths: string[] = [];
  const fallbackFiles: File[] = [];
  for (const f of Array.from(files)) {
    // Electron 才有 File.path；纯浏览器走指纹反查（Feature 12507）
    const nativePath = (f as File & { path?: string }).path;
    if (nativePath) {
      paths.push(nativePath);
    } else {
      fallbackFiles.push(f);
    }
  }
  return { paths, fallbackFiles };
}

function applyNativePaths(files: FileList | File[]) {
  const { paths, fallbackFiles } = extractNativePaths(files);
  if (paths.length > 0) {
    batchStore.addFiles(paths);
  }
  if (fallbackFiles.length === 0) {
    webModeNotice.value = '';
    return;
  }

  // 无原生路径的文件：逐个指纹反查，命中即以服务端路径入队（零拷贝）
  webModeNotice.value = `正在服务器上定位 ${fallbackFiles.length} 个文件…`;
  let located = 0;
  let settled = 0;
  const onSettled = () => {
    settled++;
    if (settled < fallbackFiles.length) return;
    const missed = fallbackFiles.length - located;
    webModeNotice.value =
      missed > 0
        ? `已定位 ${located}/${fallbackFiles.length} 个文件；${missed} 个未在服务器上找到，请用右侧多行路径录入。`
        : '';
  };
  for (const f of fallbackFiles) {
    computeFingerprint(f)
      .then((fp) => SubLiftApiClient.resolveVideoPath(fp))
      .then((path) => {
        if (path) {
          located++;
          batchStore.addFiles([path]);
        }
      })
      .catch(() => {
        // 定位失败按未命中处理
      })
      .then(onSettled);
  }
}

async function importAllFromWorkspace() {
  try {
    const list = await SubLiftApiClient.getWorkspaceVideos();
    if (list && list.length > 0) {
      const paths = list.map((v) => v.path);
      batchStore.addFiles(paths);
      webModeNotice.value = `已成功从工作区导入 ${paths.length} 个视频到队列`;
    } else {
      webModeNotice.value = '当前工作区内暂无可用视频';
    }
  } catch (err: any) {
    webModeNotice.value = err.message || '获取工作区视频失败';
  }
}

function handleFileSelect(e: Event) {
  const target = e.target as HTMLInputElement;
  if (!target.files || target.files.length === 0) return;

  applyNativePaths(target.files);
  target.value = '';
}

function handleDrop(e: DragEvent) {
  isDragOver.value = false;
  if (!e.dataTransfer?.files || e.dataTransfer.files.length === 0) return;

  applyNativePaths(e.dataTransfer.files);
}

function submitBatchPaths() {
  const lines = batchPathText.value
    .split('\n')
    .map((l) => l.trim())
    .filter((l) => l.length > 0);

  if (lines.length === 0) return;

  webModeNotice.value = '';
  batchStore.addFiles(lines);
  batchPathText.value = '';
}
</script>

<style scoped>
.sl-batch-notice {
  font-size: var(--sl-font-size-xs);
  color: var(--sl-color-warning, #d4a72c);
  background: var(--sl-surface-card);
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-sm);
  padding: 6px 10px;
  margin: 0 0 6px;
}

.sl-batch-dropzone {
  display: grid;
  grid-template-columns: 1fr auto 1fr;
  gap: 16px;
  align-items: center;
  background: var(--sl-surface-panel);
  border: 1.5px dashed var(--sl-border-focus);
  border-radius: var(--sl-radius-lg);
  padding: 14px 20px;
  transition: var(--sl-transition-smooth);
}

.sl-batch-dropzone.is-dragover {
  border-color: var(--sl-color-accent);
  background: var(--sl-color-accent-muted);
}

.sl-dropzone-left {
  display: flex;
  align-items: center;
  gap: 14px;
  cursor: pointer;
}

.sl-icon-pill {
  width: 42px;
  height: 42px;
  border-radius: var(--sl-radius-md);
  background: var(--sl-surface-card);
  border: 1px solid var(--sl-border-standard);
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--sl-color-accent);
  flex-shrink: 0;
  box-shadow: var(--sl-shadow-sm), var(--sl-inner-highlight);
}

.sl-upload-icon {
  width: 20px;
  height: 20px;
}

.sl-drop-title {
  font-size: var(--sl-font-size-base);
  font-weight: 600;
  color: var(--sl-text-primary);
}

.sl-drop-subtitle {
  font-size: var(--sl-font-size-xs);
  color: var(--sl-text-tertiary);
  margin-top: 2px;
}

.sl-ws-import-btn {
  background: var(--sl-color-ready-bg);
  border: 1px solid rgba(10, 132, 255, 0.3);
  color: var(--sl-color-accent);
  border-radius: var(--sl-radius-sm);
  padding: 6px 12px;
  font-size: 12px;
  font-weight: 500;
  cursor: pointer;
  white-space: nowrap;
  transition: var(--sl-transition-snappy);
}

.sl-ws-import-btn:hover {
  background: rgba(10, 132, 255, 0.2);
  border-color: var(--sl-color-accent);
}

.sl-dropzone-divider {
  width: 1px;
  height: 40px;
  background: var(--sl-border-subtle);
}

.sl-dropzone-right {
  display: flex;
  align-items: center;
  gap: 10px;
}

.sl-batch-path-textarea {
  flex: 1;
  background: var(--sl-surface-card);
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-sm);
  color: var(--sl-text-primary);
  font-size: var(--sl-font-size-xs);
  font-family: var(--sl-font-family-mono);
  padding: 6px 8px;
  resize: none;
  outline: none;
  box-shadow: var(--sl-inner-shadow);
}

.sl-batch-path-textarea:focus {
  border-color: var(--sl-color-accent);
}

.sl-batch-add-btn {
  height: 32px;
  padding: 0 12px;
  background: var(--sl-color-accent);
  color: #fff;
  border: none;
  border-radius: var(--sl-radius-sm);
  font-size: var(--sl-font-size-xs);
  font-weight: 500;
  cursor: pointer;
  white-space: nowrap;
  transition: var(--sl-transition-snappy);
}

.sl-batch-add-btn:hover:not(:disabled) {
  background: var(--sl-color-accent-hover);
}

.sl-batch-add-btn:disabled {
  opacity: 0.35;
  cursor: not-allowed;
}
</style>
