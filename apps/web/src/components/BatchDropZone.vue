<template>
  <div class="sl-batch-ingestion-wrap">
    <!-- 1. 主拖拽与导入面板 -->
    <div 
      class="sl-batch-dropzone"
      :class="{ 'is-dragover': isDragOver, 'is-processing': isScanning }"
      @dragover.prevent="isDragOver = true"
      @dragleave.prevent="isDragOver = false"
      @drop.prevent="handleDrop"
    >
      <div class="sl-dropzone-left">
        <div class="sl-icon-pill" @click="triggerFileInput">
          <svg v-if="!isScanning" class="sl-upload-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor">
            <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" stroke-width="1.8" stroke-linecap="round"/>
            <polyline points="17 8 12 3 7 8" stroke-width="1.8" stroke-linecap="round"/>
            <line x1="12" y1="3" x2="12" y2="15" stroke-width="1.8" stroke-linecap="round"/>
          </svg>
          <div v-else class="sl-scanning-spinner"></div>
        </div>

        <div class="sl-drop-text-group">
          <div class="sl-drop-title">
            {{ isScanning ? '正在递归扫描媒体文件...' : '批量添加视频 / 文件夹到队列' }}
          </div>
          <div class="sl-drop-subtitle">
            拖拽多个视频或整个文件夹，支持 MP4 / MOV / MKV / WebM / AVI
          </div>
        </div>

        <div class="sl-btn-action-group">
          <button 
            type="button" 
            class="sl-ws-action-btn" 
            :disabled="isScanning"
            @click.stop="triggerFileInput"
          >
            📄 选择文件
          </button>
          <button 
            type="button" 
            class="sl-ws-action-btn" 
            :disabled="isScanning"
            @click.stop="triggerDirInput"
          >
            📂 选择文件夹
          </button>
          <button 
            type="button" 
            class="sl-ws-import-btn" 
            :disabled="isScanning"
            @click.stop="importAllFromWorkspace"
          >
            📁 导入工作区视频
          </button>
        </div>

        <!-- 隐藏的 File / Directory Input -->
        <input 
          ref="fileInputRef" 
          type="file" 
          multiple 
          accept="video/mp4,video/quicktime,video/webm,video/x-matroska,video/x-msvideo,.mp4,.mov,.webm,.mkv,.avi,.m4v,.flv"
          style="display: none"
          @change="handleFileSelect"
        />
        <input 
          ref="dirInputRef" 
          type="file" 
          webkitdirectory 
          directory 
          multiple
          style="display: none"
          @change="handleDirSelect"
        />
      </div>

      <div class="sl-dropzone-divider"></div>

      <!-- 绝对路径多行批量录入内嵌框 -->
      <div class="sl-dropzone-right">
        <textarea
          v-model="batchPathText"
          placeholder="或直接粘贴本地视频/工作区绝对路径 (支持换行多路径):&#10;/path/to/video1.mp4&#10;/path/to/video2.mov"
          class="sl-batch-path-textarea"
          rows="2"
          :disabled="isScanning"
        ></textarea>
        <button 
          class="sl-batch-add-btn" 
          :disabled="!batchPathText.trim() || isScanning"
          @click="submitBatchPaths"
        >
          加入队列
        </button>
      </div>
    </div>

    <!-- 2. 扫描摘要横幅 (ScanSummaryBanner) -->
    <transition name="sl-slide-fade">
      <div v-if="batchStore.lastScanSummary" class="sl-scan-summary-banner">
        <div class="sl-summary-left">
          <span class="sl-summary-icon">📋</span>
          <span class="sl-summary-text">
            扫描完成：已接受 <strong>{{ batchStore.lastScanSummary.accepted.length }}</strong> 项，
            跳过 <strong>{{ batchStore.lastScanSummary.skipped }}</strong> 项隐藏/系统文件
            <span v-if="batchStore.lastScanSummary.rejected.length > 0">
              ，拒绝 <strong>{{ batchStore.lastScanSummary.rejected.length }}</strong> 项
            </span>
          </span>
        </div>

        <div class="sl-summary-right">
          <button 
            v-if="batchStore.lastScanSummary.rejected.length > 0"
            type="button" 
            class="sl-summary-btn-link"
            @click="isDetailsExpanded = !isDetailsExpanded"
          >
            {{ isDetailsExpanded ? '收起详情' : '查看被拒绝项' }}
          </button>
          <button 
            type="button" 
            class="sl-summary-close-btn"
            title="关闭提示"
            @click="batchStore.setScanSummary(null)"
          >
            ✕
          </button>
        </div>

        <!-- 折叠的拒绝明细 -->
        <div v-if="isDetailsExpanded && batchStore.lastScanSummary.rejected.length > 0" class="sl-summary-details">
          <div 
            v-for="(rej, idx) in batchStore.lastScanSummary.rejected" 
            :key="idx"
            class="sl-summary-rej-row"
          >
            <span class="sl-rej-name">• {{ rej.pathOrName }}</span>
            <span class="sl-rej-reason">({{ formatRejectionReason(rej.reason) }})</span>
          </div>
        </div>
      </div>
    </transition>
  </div>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { useBatchStore } from '../stores/batch';
import { SubLiftApiClient } from '../api/client';
import { BrowserDirectoryScanner } from '../utils/browser_scanner';
import type { BatchInputRejectionReason } from '../types/batch';

const batchStore = useBatchStore();
const isDragOver = ref(false);
const isScanning = ref(false);
const isDetailsExpanded = ref(false);
const fileInputRef = ref<HTMLInputElement | null>(null);
const dirInputRef = ref<HTMLInputElement | null>(null);
const batchPathText = ref('');

function triggerFileInput() {
  fileInputRef.value?.click();
}

function triggerDirInput() {
  dirInputRef.value?.click();
}

function formatRejectionReason(reason: BatchInputRejectionReason): string {
  switch (reason.kind) {
    case 'unsupportedFormat':
      return `不支持的格式 .${reason.extension}`;
    case 'mkvRequiresFfmpeg':
      return 'MKV 需要系统安装 ffmpeg';
    case 'duplicate':
      return '队列中已存在或本批次重复';
    case 'emptyDirectory':
      return '文件夹为空';
    case 'unreadable':
      return reason.detail ? `读取失败: ${reason.detail}` : '无法访问或权限不足';
  }
}

async function processScanFiles(files: FileList | DataTransferItemList) {
  isScanning.value = true;
  try {
    // 检查是否具备原生桌面绝对路径 (如 Electron / 混合环境)
    const nativePaths: string[] = [];
    if ('length' in files) {
      for (let i = 0; i < files.length; i++) {
        const item = files[i];
        const p = (item as any).path;
        if (typeof p === 'string' && p) {
          nativePaths.push(p);
        }
      }
    }

    if (nativePaths.length > 0) {
      const summary = await SubLiftApiClient.scanServerPaths(nativePaths);
      const existing = new Set(batchStore.tasks.map((t) => t.videoPath));
      const finalAccepted: any[] = [];
      const extraRejected: any[] = [];

      for (const item of summary.accepted) {
        if (existing.has(item.videoPath)) {
          extraRejected.push({
            pathOrName: item.name,
            reason: { kind: 'duplicate' as const },
          });
        } else {
          existing.add(item.videoPath);
          finalAccepted.push(item);
        }
      }

      if (finalAccepted.length > 0) {
        batchStore.addBatchItems(finalAccepted);
      }

      batchStore.setScanSummary({
        accepted: finalAccepted,
        skipped: summary.skipped,
        rejected: [...summary.rejected, ...extraRejected],
      });
      return;
    }

    // 纯浏览器沙盒模式：通过 HTML5 递归与指纹反查
    const scanRes = await BrowserDirectoryScanner.scanDataTransferItems(files);
    const existingPaths = new Set(batchStore.tasks.map((t) => t.videoPath));
    const resolveRes = await BrowserDirectoryScanner.resolveFilesWithConcurrency(
      scanRes.files,
      existingPaths,
      4
    );

    const allRejected = [...scanRes.rejected, ...resolveRes.rejected];

    if (resolveRes.accepted.length > 0) {
      batchStore.addBatchItems(resolveRes.accepted);
    }

    batchStore.setScanSummary({
      accepted: resolveRes.accepted,
      skipped: scanRes.skipped,
      rejected: allRejected,
    });
  } catch (err: any) {
    console.error('Batch scan error:', err);
    batchStore.setScanSummary({
      accepted: [],
      skipped: 0,
      rejected: [{ pathOrName: '文件扫描', reason: { kind: 'unreadable', detail: err.message } }],
    });
  } finally {
    isScanning.value = false;
  }
}

async function handleDrop(e: DragEvent) {
  isDragOver.value = false;
  if (!e.dataTransfer) return;

  if (e.dataTransfer.items && e.dataTransfer.items.length > 0) {
    await processScanFiles(e.dataTransfer.items);
  } else if (e.dataTransfer.files && e.dataTransfer.files.length > 0) {
    await processScanFiles(e.dataTransfer.files);
  }
}

async function handleFileSelect(e: Event) {
  const target = e.target as HTMLInputElement;
  if (!target.files || target.files.length === 0) return;

  await processScanFiles(target.files);
  target.value = '';
}

async function handleDirSelect(e: Event) {
  const target = e.target as HTMLInputElement;
  if (!target.files || target.files.length === 0) return;

  await processScanFiles(target.files);
  target.value = '';
}

async function importAllFromWorkspace() {
  isScanning.value = true;
  try {
    const list = await SubLiftApiClient.getWorkspaceVideos();
    if (list && list.length > 0) {
      const existing = new Set(batchStore.tasks.map((t) => t.videoPath));
      const accepted: any[] = [];
      const rejected: any[] = [];

      for (const item of list) {
        if (existing.has(item.path)) {
          rejected.push({
            pathOrName: item.name,
            reason: { kind: 'duplicate' },
          });
        } else {
          existing.add(item.path);
          accepted.push({
            videoPath: item.path,
            name: item.name,
            sizeBytes: item.size_bytes,
            importRootPath: item.relative_path.includes('/') ? item.relative_path.split('/').slice(0, -1).join('/') + '/' : undefined,
          });
        }
      }

      if (accepted.length > 0) {
        batchStore.addBatchItems(accepted);
      }

      batchStore.setScanSummary({
        accepted,
        skipped: 0,
        rejected,
      });
    } else {
      batchStore.setScanSummary({
        accepted: [],
        skipped: 0,
        rejected: [{ pathOrName: '工作区', reason: { kind: 'emptyDirectory' } }],
      });
    }
  } catch (err: any) {
    batchStore.setScanSummary({
      accepted: [],
      skipped: 0,
      rejected: [{ pathOrName: '工作区扫描', reason: { kind: 'unreadable', detail: err.message } }],
    });
  } finally {
    isScanning.value = false;
  }
}

async function submitBatchPaths() {
  const lines = batchPathText.value
    .split('\n')
    .map((l) => l.trim().replace(/^["']|["']$/g, ''))
    .filter((l) => l.length > 0);

  if (lines.length === 0) return;

  isScanning.value = true;
  try {
    const summary = await SubLiftApiClient.scanServerPaths(lines);
    const existing = new Set(batchStore.tasks.map((t) => t.videoPath));
    const finalAccepted: any[] = [];
    const extraRejected: any[] = [];

    for (const item of summary.accepted) {
      if (existing.has(item.videoPath)) {
        extraRejected.push({
          pathOrName: item.name,
          reason: { kind: 'duplicate' as const },
        });
      } else {
        existing.add(item.videoPath);
        finalAccepted.push(item);
      }
    }

    if (finalAccepted.length > 0) {
      batchStore.addBatchItems(finalAccepted);
    }

    batchStore.setScanSummary({
      accepted: finalAccepted,
      skipped: summary.skipped,
      rejected: [...summary.rejected, ...extraRejected],
    });

    batchPathText.value = '';
  } catch (err: any) {
    batchStore.setScanSummary({
      accepted: [],
      skipped: 0,
      rejected: [{ pathOrName: '路径扫描', reason: { kind: 'unreadable', detail: err.message } }],
    });
  } finally {
    isScanning.value = false;
  }
}
</script>

<style scoped>
.sl-batch-ingestion-wrap {
  display: flex;
  flex-direction: column;
  gap: 8px;
  width: 100%;
}

.sl-batch-dropzone {
  display: flex;
  align-items: center;
  gap: 20px;
  background: var(--sl-surface-card, #1c1c1e);
  border: 1px dashed var(--sl-border-standard, rgba(255, 255, 255, 0.15));
  border-radius: var(--sl-radius-lg, 10px);
  padding: 14px 20px;
  transition: all 0.2s cubic-bezier(0.16, 1, 0.3, 1);
}

.sl-batch-dropzone:hover {
  border-color: var(--sl-color-primary, #0a84ff);
  background: var(--sl-surface-elevated, #242426);
}

.sl-batch-dropzone.is-dragover {
  border-color: var(--sl-color-primary, #0a84ff);
  border-style: solid;
  background: rgba(10, 132, 255, 0.08);
  box-shadow: 0 0 0 2px rgba(10, 132, 255, 0.2);
}

.sl-batch-dropzone.is-processing {
  opacity: 0.8;
  pointer-events: none;
}

.sl-dropzone-left {
  display: flex;
  align-items: center;
  gap: 16px;
  flex: 1;
  min-width: 0;
}

.sl-icon-pill {
  width: 42px;
  height: 42px;
  border-radius: var(--sl-radius-md, 8px);
  background: var(--sl-surface-elevated, #2c2c2e);
  border: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.1));
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--sl-color-primary, #0a84ff);
  flex-shrink: 0;
  cursor: pointer;
  transition: transform 0.15s ease;
}

.sl-icon-pill:hover {
  transform: scale(1.05);
}

.sl-upload-icon {
  width: 20px;
  height: 20px;
}

.sl-scanning-spinner {
  width: 20px;
  height: 20px;
  border: 2px solid rgba(10, 132, 255, 0.2);
  border-top-color: var(--sl-color-primary, #0a84ff);
  border-radius: 50%;
  animation: spin 0.8s linear infinite;
}

@keyframes spin {
  to { transform: rotate(360deg); }
}

.sl-drop-text-group {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}

.sl-drop-title {
  font-size: var(--sl-font-size-sm, 13px);
  font-weight: 600;
  color: var(--sl-text-primary, #ffffff);
}

.sl-drop-subtitle {
  font-size: var(--sl-font-size-xs, 12px);
  color: var(--sl-text-secondary, #8e8e93);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.sl-btn-action-group {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-left: auto;
  flex-shrink: 0;
}

.sl-ws-action-btn,
.sl-ws-import-btn {
  font-size: var(--sl-font-size-xs, 12px);
  font-weight: 500;
  padding: 6px 12px;
  border-radius: var(--sl-radius-sm, 6px);
  border: 1px solid var(--sl-border-standard, rgba(255, 255, 255, 0.15));
  background: var(--sl-surface-elevated, #2c2c2e);
  color: var(--sl-text-primary, #ffffff);
  cursor: pointer;
  transition: all 0.15s ease;
  white-space: nowrap;
}

.sl-ws-action-btn:hover,
.sl-ws-import-btn:hover {
  background: var(--sl-surface-active, #3a3a3c);
  border-color: var(--sl-color-primary, #0a84ff);
}

.sl-ws-import-btn {
  background: rgba(10, 132, 255, 0.12);
  border-color: rgba(10, 132, 255, 0.3);
  color: #5ac8fa;
}

.sl-ws-import-btn:hover {
  background: rgba(10, 132, 255, 0.2);
}

.sl-dropzone-divider {
  width: 1px;
  height: 48px;
  background: var(--sl-border-subtle, rgba(255, 255, 255, 0.1));
  flex-shrink: 0;
}

.sl-dropzone-right {
  display: flex;
  align-items: center;
  gap: 10px;
  width: 380px;
  flex-shrink: 0;
}

.sl-batch-path-textarea {
  flex: 1;
  background: var(--sl-surface-base, #121214);
  border: 1px solid var(--sl-border-standard, rgba(255, 255, 255, 0.15));
  border-radius: var(--sl-radius-sm, 6px);
  padding: 6px 10px;
  font-size: var(--sl-font-size-xs, 12px);
  font-family: var(--sl-font-mono, monospace);
  color: var(--sl-text-primary, #ffffff);
  resize: none;
  line-height: 1.4;
}

.sl-batch-path-textarea:focus {
  outline: none;
  border-color: var(--sl-color-primary, #0a84ff);
}

.sl-batch-add-btn {
  font-size: var(--sl-font-size-xs, 12px);
  font-weight: 500;
  padding: 8px 14px;
  background: var(--sl-color-primary, #0a84ff);
  color: #ffffff;
  border: none;
  border-radius: var(--sl-radius-sm, 6px);
  cursor: pointer;
  transition: opacity 0.15s ease;
  white-space: nowrap;
}

.sl-batch-add-btn:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

/* Scan Summary Banner */
.sl-scan-summary-banner {
  background: var(--sl-surface-elevated, #242426);
  border: 1px solid var(--sl-border-standard, rgba(255, 255, 255, 0.15));
  border-radius: var(--sl-radius-md, 8px);
  padding: 8px 14px;
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  font-size: var(--sl-font-size-xs, 12px);
}

.sl-summary-left {
  display: flex;
  align-items: center;
  gap: 8px;
  color: var(--sl-text-primary, #ffffff);
}

.sl-summary-right {
  display: flex;
  align-items: center;
  gap: 12px;
}

.sl-summary-btn-link {
  background: none;
  border: none;
  color: var(--sl-color-primary, #0a84ff);
  font-size: var(--sl-font-size-xs, 12px);
  cursor: pointer;
  text-decoration: underline;
  padding: 0;
}

.sl-summary-close-btn {
  background: none;
  border: none;
  color: var(--sl-text-secondary, #8e8e93);
  font-size: var(--sl-font-size-xs, 12px);
  cursor: pointer;
  padding: 2px 6px;
  border-radius: 4px;
}

.sl-summary-close-btn:hover {
  background: var(--sl-surface-active, #3a3a3c);
  color: var(--sl-text-primary, #ffffff);
}

.sl-summary-details {
  width: 100%;
  padding-top: 8px;
  border-top: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.08));
  display: flex;
  flex-direction: column;
  gap: 4px;
  max-height: 120px;
  overflow-y: auto;
}

.sl-summary-rej-row {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 11px;
  color: var(--sl-text-secondary, #8e8e93);
}

.sl-rej-reason {
  color: var(--sl-color-warning, #ff9f0a);
}

/* Animations */
.sl-slide-fade-enter-active,
.sl-slide-fade-leave-active {
  transition: all 0.2s ease;
}

.sl-slide-fade-enter-from,
.sl-slide-fade-leave-to {
  transform: translateY(-6px);
  opacity: 0;
}
</style>
