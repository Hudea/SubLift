<template>
  <div class="sl-task-center-layout" role="region" aria-label="批量任务中心">
    <!-- 1. 顶部 7 状态统计面板 -->
    <BatchStatsCards />

    <!-- 2. 批量多模态导入区 (支持文件夹/文件拖拽、工作区一键导入、路径粘贴与扫描摘要横幅) -->
    <BatchDropZone />

    <!-- 3. 操作与列表主卡片 -->
    <div class="sl-main-queue-card">
      <!-- 队列全局操作工具栏 -->
      <div class="sl-queue-toolbar" role="toolbar" aria-label="队列全局操作栏">
        <div class="sl-toolbar-left">
          <span class="sl-toolbar-title">批量任务队列</span>
          <span class="sl-badge-count" :aria-label="`当前展示 ${batchStore.filteredTasks.length} 项，共 ${batchStore.tasks.length} 项`">
            {{ batchStore.filteredTasks.length }} / {{ batchStore.tasks.length }} 项
          </span>

          <!-- 搜索框 -->
          <div class="sl-search-box">
            <svg class="sl-search-icon" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
              <path d="M11.742 10.344a6.5 6.5 0 1 0-1.397 1.398h-.001c.03.04.062.078.098.115l3.85 3.85a1 1 0 0 0 1.415-1.414l-3.85-3.85a1.007 1.007 0 0 0-.115-.1zM12 6.5a5.5 5.5 0 1 1-11 0 5.5 5.5 0 0 1 11 0z"/>
            </svg>
            <input 
              v-model="batchStore.searchText" 
              type="text" 
              placeholder="搜索视频名、路径或目录..." 
              class="sl-search-input"
              aria-label="搜索视频名、路径或目录"
            />
            <button 
              v-if="batchStore.searchText" 
              type="button" 
              class="sl-search-clear"
              aria-label="清除搜索"
              @click="batchStore.searchText = ''"
            >
              ✕
            </button>
          </div>
        </div>

        <div class="sl-toolbar-right">
          <!-- 全部开始 / 继续 -->
          <button 
            v-if="!batchStore.isQueueRunning"
            class="sl-btn sl-btn--primary"
            :disabled="!batchStore.canStartAll"
            aria-label="开始全部任务"
            @click="batchStore.startQueue()"
          >
            <svg class="sl-btn-icon-svg" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
              <path d="m11.596 8.697-6.363 3.692c-.54.313-1.233-.066-1.233-.697V4.308c0-.63.692-1.01 1.233-.696l6.363 3.692a.802.802 0 0 1 0 1.393z"/>
            </svg>
            <span>开始全部任务</span>
          </button>

          <!-- 暂停队列 -->
          <button 
            v-else
            class="sl-btn sl-btn--warning"
            aria-label="暂停队列"
            @click="batchStore.pauseQueue()"
          >
            <svg class="sl-btn-icon-svg" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
              <path d="M5.5 3.5A1.5 1.5 0 0 1 7 5v6a1.5 1.5 0 0 1-3 0V5a1.5 1.5 0 0 1 1.5-1.5zm5 0A1.5 1.5 0 0 1 12 5v6a1.5 1.5 0 0 1-3 0V5a1.5 1.5 0 0 1 1.5-1.5z"/>
            </svg>
            <span>暂停队列</span>
          </button>

          <!-- 批量下载 ZIP (浏览器) -->
          <button 
            class="sl-btn sl-btn--secondary"
            :disabled="!batchStore.canExportAll"
            title="打包全部已完成字幕为单一 ZIP 下载"
            aria-label="打包全部已完成字幕为单一 ZIP 下载"
            @click="handleDownloadZip"
          >
            <svg class="sl-btn-icon-svg" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
              <path d="M.5 9.9a.5.5 0 0 1 .5.5v2.5a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-2.5a.5.5 0 0 1 1 0v2.5a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2v-2.5a.5.5 0 0 1 .5-.5z"/>
              <path d="M7.646 11.854a.5.5 0 0 0 .708 0l3-3a.5.5 0 0 0-.708-.708L8.5 10.293V1.5a.5.5 0 0 0-1 0v8.793L5.354 8.146a.5.5 0 1 0-.708.708l3 3z"/>
            </svg>
            <span>下载 ZIP 压缩包</span>
          </button>

          <!-- 批量受控落盘保存 (服务端) -->
          <button 
            class="sl-btn sl-btn--primary"
            :disabled="!batchStore.canBatchSave"
            title="服务端原子保存到工作区目录"
            aria-label="服务端原子保存到工作区目录"
            @click="isBatchSaveModalOpen = true"
          >
            <svg class="sl-btn-icon-svg" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
              <path d="M2 1a1 1 0 0 0-1 1v12a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1V2a1 1 0 0 0-1-1H2zm12-1a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2V2a2 2 0 0 1 2-2h12z"/>
              <path d="M4 2.5a.5.5 0 0 1 .5-.5h7a.5.5 0 0 1 .5.5v2a.5.5 0 0 1-.5.5h-7a.5.5 0 0 1-.5-.5v-2zm0 6a.5.5 0 0 1 .5-.5h7a.5.5 0 0 1 .5.5v5a.5.5 0 0 1-.5.5h-7a.5.5 0 0 1-.5-.5v-5z"/>
            </svg>
            <span>批量保存到磁盘</span>
          </button>

          <!-- 清空已完成 -->
          <button 
            class="sl-btn sl-btn--ghost"
            :disabled="!batchStore.canClearCompleted"
            aria-label="清空已完成任务"
            @click="batchStore.clearCompleted()"
          >
            清空已完成
          </button>
        </div>
      </div>

      <!-- 任务数据表格 -->
      <div class="sl-table-container" role="region" aria-label="任务队列数据表格">
        <table v-if="batchStore.filteredTasks.length > 0" class="sl-task-table" role="table" aria-label="批量任务队列">
          <thead>
            <tr>
              <th scope="col">视频名称 / 路径</th>
              <th scope="col">来源位置</th>
              <th scope="col">提取配置</th>
              <th scope="col">提取进度</th>
              <th scope="col">字幕条目</th>
              <th scope="col">耗时</th>
              <th scope="col">操作</th>
            </tr>
          </thead>
          <tbody>
            <BatchTaskRow 
              v-for="task in batchStore.filteredTasks" 
              :key="task.id" 
              :task="task" 
            />
          </tbody>
        </table>

        <!-- 筛选无结果空态 (FilterEmptyState) -->
        <div v-else-if="batchStore.tasks.length > 0" class="sl-empty-state" role="status">
          <div class="sl-empty-icon-wrap" aria-hidden="true">
            <svg class="sl-empty-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor">
              <circle cx="11" cy="11" r="8" stroke-width="1.6"/>
              <line x1="21" y1="21" x2="16.65" y2="16.65" stroke-width="1.6"/>
            </svg>
          </div>
          <p class="sl-empty-title">无匹配的视频任务</p>
          <p class="sl-empty-desc">当前筛选或搜索条件下未检索到相关条目</p>
          <button 
            type="button" 
            class="sl-btn sl-btn--secondary sl-btn--sm"
            aria-label="清除筛选与搜索"
            @click="clearFilters"
          >
            清除筛选与搜索
          </button>
        </div>

        <!-- 队列完全为空的空态 -->
        <div v-else class="sl-empty-state" role="status">
          <div class="sl-empty-icon-wrap" aria-hidden="true">
            <svg class="sl-empty-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor">
              <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z" stroke-width="1.6"/>
              <polyline points="14 2 14 8 20 8" stroke-width="1.6"/>
              <line x1="16" y1="13" x2="8" y2="13" stroke-width="1.6"/>
              <line x1="16" y1="17" x2="8" y2="17" stroke-width="1.6"/>
            </svg>
          </div>
          <p class="sl-empty-title">队列为空</p>
          <p class="sl-empty-desc">拖拽视频文件、文件夹或从工作区一键导入开始批量提取</p>
        </div>
      </div>
    </div>

    <!-- 4. 单任务详情检查器抽屉 (Inspector Drawer) -->
    <transition name="sl-slide-fade">
      <BatchTaskInspector 
        v-if="batchStore.selectedTask"
        :key="batchStore.selectedTask.id"
      />
    </transition>

    <!-- 5. 批量落盘模态弹窗 (Feature 12514) -->
    <div 
      v-if="isBatchSaveModalOpen" 
      ref="batchSaveModalRef"
      class="sl-modal-overlay" 
      role="dialog"
      aria-modal="true"
      aria-labelledby="batch-save-title"
      aria-describedby="batch-save-desc"
      @click.self="isBatchSaveModalOpen = false"
    >
      <div class="sl-modal-card">
        <div class="sl-modal-header">
          <h2 id="batch-save-title" class="sl-modal-title">批量保存字幕到工作区磁盘</h2>
          <button 
            type="button"
            class="sl-modal-close" 
            aria-label="关闭批量保存弹窗"
            @click="isBatchSaveModalOpen = false"
          >
            ×
          </button>
        </div>
        <p id="batch-save-desc" class="sl-modal-desc">
          采用受控原子落盘（同目录临时文件 + fsync + 原子 rename），将已完成任务的 SRT 文件写入各自视频所在目录。
        </p>

        <div class="sl-modal-body">
          <div class="sl-form-group" role="radiogroup" aria-label="文件冲突处理策略">
            <label class="sl-form-label">文件冲突处理策略：</label>
            <div class="sl-radio-group">
              <label class="sl-radio-item">
                <input v-model="batchConflictPolicy" type="radio" value="deterministic_rename" aria-label="自动重命名 (video_1.srt)" />
                <span>自动重命名 (video_1.srt)</span>
              </label>
              <label class="sl-radio-item">
                <input v-model="batchConflictPolicy" type="radio" value="skip" aria-label="跳过已存在文件 (skip)" />
                <span>跳过已存在文件 (skip)</span>
              </label>
              <label class="sl-radio-item">
                <input v-model="batchConflictPolicy" type="radio" value="replace" aria-label="原子覆盖替换 (replace)" />
                <span>原子覆盖替换 (replace)</span>
              </label>
            </div>
          </div>

          <div class="sl-form-group">
            <label class="sl-checkbox-item">
              <input v-model="batchAllowEmpty" type="checkbox" aria-label="允许落盘 0 字节空字幕文件" />
              <span>允许落盘 0 字节空字幕文件 (默认跳过)</span>
            </label>
          </div>

          <div v-if="batchSaveResult" class="sl-modal-result-box" role="status" aria-live="polite">
            <div class="sl-result-title">落盘处理结果：</div>
            <div class="sl-result-summary">
              共处理 {{ batchSaveResult.total }} 项：
              成功落盘 <strong>{{ batchSaveResult.saved }}</strong>，
              跳过 <strong>{{ batchSaveResult.skipped }}</strong>，
              空字幕 <strong>{{ batchSaveResult.empty_results }}</strong>，
              失败 <strong>{{ batchSaveResult.failed }}</strong>
            </div>
          </div>

          <div v-if="batchSaveError" class="sl-modal-error" role="alert" aria-live="assertive">
            {{ batchSaveError }}
          </div>
        </div>

        <div class="sl-modal-footer">
          <button 
            type="button"
            class="sl-modal-btn-cancel" 
            aria-label="关闭弹窗"
            @click="isBatchSaveModalOpen = false"
          >
            关闭
          </button>
          <button
            type="button"
            class="sl-modal-btn-confirm"
            aria-label="执行批量落盘"
            :disabled="isBatchSaving"
            @click="handleExecuteBatchSave"
          >
            {{ isBatchSaving ? '正在落盘…' : '执行批量落盘' }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import BatchStatsCards from '../components/BatchStatsCards.vue';
import BatchDropZone from '../components/BatchDropZone.vue';
import BatchTaskRow from '../components/BatchTaskRow.vue';
import BatchTaskInspector from '../components/BatchTaskInspector.vue';
import { useBatchStore } from '../stores/batch';
import { useModalA11y } from '../composables/useModalA11y';
import type { JobConflictPolicy, BatchSaveResponse } from '../types/api';

const batchStore = useBatchStore();

const isBatchSaveModalOpen = ref(false);
const batchSaveModalRef = ref<HTMLElement | null>(null);
const batchConflictPolicy = ref<JobConflictPolicy>('deterministic_rename');
const batchAllowEmpty = ref(false);
const isBatchSaving = ref(false);
const batchSaveResult = ref<BatchSaveResponse | null>(null);
const batchSaveError = ref<string | null>(null);

useModalA11y(isBatchSaveModalOpen, batchSaveModalRef, {
  initialFocusSelector: '.sl-modal-btn-confirm',
  onClose: () => {
    isBatchSaveModalOpen.value = false;
  },
});

function clearFilters() {
  batchStore.searchText = '';
  batchStore.statusFilter = 'all';
}

function handleDownloadZip() {
  batchStore.downloadBatchZip();
}

async function handleExecuteBatchSave() {
  isBatchSaving.value = true;
  batchSaveError.value = null;
  batchSaveResult.value = null;
  try {
    const res = await batchStore.batchSaveToDisk(batchConflictPolicy.value, batchAllowEmpty.value);
    batchSaveResult.value = res;
  } catch (err: unknown) {
    batchSaveError.value = err instanceof Error ? err.message : String(err);
  } finally {
    isBatchSaving.value = false;
  }
}
</script>

<style scoped>
.sl-task-center-layout {
  display: flex;
  flex-direction: column;
  gap: 14px;
  max-width: 1360px;
  width: 100%;
  margin: 0 auto;
  padding: 16px 20px;
  box-sizing: border-box;
  overflow-y: auto;
  overflow-x: hidden;
  height: calc(100vh - 48px);
}

.sl-main-queue-card {
  background: var(--sl-surface-card, #1c1c1e);
  border: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.08));
  border-radius: var(--sl-radius-lg, 10px);
  overflow: hidden;
  display: flex;
  flex-direction: column;
  flex-shrink: 0;
}

/* Toolbar */
.sl-queue-toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 10px 14px;
  border-bottom: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.08));
  background: var(--sl-surface-elevated, #242426);
  flex-wrap: wrap;
}

.sl-toolbar-left {
  display: flex;
  align-items: center;
  gap: 10px;
  flex: 1;
  min-width: 260px;
}

.sl-toolbar-title {
  font-size: var(--sl-font-size-md, 14px);
  font-weight: 600;
  color: var(--sl-text-primary, #ffffff);
}

.sl-badge-count {
  font-size: var(--sl-font-size-xs, 12px);
  padding: 2px 8px;
  background: var(--sl-surface-base, #161618);
  border-radius: var(--sl-radius-pill, 999px);
  color: var(--sl-text-secondary, #8e8e93);
  font-variant-numeric: tabular-nums;
}

.sl-search-box {
  position: relative;
  display: flex;
  align-items: center;
  flex: 1;
  max-width: 280px;
}

.sl-search-icon {
  position: absolute;
  left: 9px;
  width: 13px;
  height: 13px;
  color: var(--sl-text-tertiary, #636366);
  pointer-events: none;
}

.sl-search-input {
  width: 100%;
  padding: 5px 28px 5px 28px;
  font-size: var(--sl-font-size-xs, 12px);
  background: var(--sl-surface-base, #161618);
  border: 1px solid var(--sl-border-standard, rgba(255, 255, 255, 0.15));
  border-radius: var(--sl-radius-sm, 6px);
  color: var(--sl-text-primary, #ffffff);
  outline: none;
  transition: border-color 0.15s ease;
}

.sl-search-input:focus {
  border-color: var(--sl-color-accent, #0a84ff);
}

.sl-search-clear {
  position: absolute;
  right: 6px;
  background: none;
  border: none;
  color: var(--sl-text-tertiary, #636366);
  font-size: 11px;
  cursor: pointer;
  padding: 2px 4px;
}

.sl-search-clear:hover {
  color: var(--sl-text-primary, #ffffff);
}

.sl-toolbar-right {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
  flex-wrap: wrap;
}

/* Buttons */
.sl-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  font-size: var(--sl-font-size-xs, 12px);
  font-weight: 500;
  padding: 5px 10px;
  border-radius: var(--sl-radius-sm, 6px);
  border: 1px solid transparent;
  cursor: pointer;
  transition: all 0.15s ease;
  white-space: nowrap;
}

.sl-btn--sm {
  padding: 4px 10px;
  font-size: 11px;
}

.sl-btn-icon-svg {
  width: 13px;
  height: 13px;
}

.sl-btn--primary {
  background: var(--sl-color-accent, #0a84ff);
  color: #ffffff;
}

.sl-btn--primary:hover:not(:disabled) {
  background: #0071e3;
}

.sl-btn--warning {
  background: #ff9f0a;
  color: #000000;
  font-weight: 600;
}

.sl-btn--secondary {
  background: var(--sl-surface-elevated, #2c2c2e);
  border-color: var(--sl-border-standard, rgba(255, 255, 255, 0.15));
  color: var(--sl-text-primary, #ffffff);
}

.sl-btn--secondary:hover:not(:disabled) {
  background: var(--sl-surface-active, #3a3a3c);
}

.sl-btn--ghost {
  background: transparent;
  color: var(--sl-text-secondary, #8e8e93);
}

.sl-btn--ghost:hover:not(:disabled) {
  background: var(--sl-surface-elevated, #2c2c2e);
  color: var(--sl-text-primary, #ffffff);
}

.sl-btn:disabled {
  opacity: 0.35;
  cursor: not-allowed;
}

/* Table */
.sl-table-container {
  overflow-x: auto;
  min-height: 180px;
}

.sl-task-table {
  width: 100%;
  border-collapse: collapse;
  text-align: left;
}

.sl-task-table th {
  padding: 8px 12px;
  font-size: var(--sl-font-size-xs, 12px);
  font-weight: 600;
  color: var(--sl-text-secondary, #8e8e93);
  border-bottom: 1px solid var(--sl-border-standard, rgba(255, 255, 255, 0.12));
  background: var(--sl-surface-base, #161618);
  white-space: nowrap;
}

/* Empty State */
.sl-empty-state {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  padding: 36px 20px;
  text-align: center;
  gap: 6px;
}

.sl-empty-icon-wrap {
  width: 44px;
  height: 44px;
  border-radius: var(--sl-radius-lg, 10px);
  background: var(--sl-surface-elevated, #2c2c2e);
  display: flex;
  align-items: center;
  justify-content: center;
  margin-bottom: 4px;
}

.sl-empty-icon {
  width: 22px;
  height: 22px;
  color: var(--sl-text-tertiary, #636366);
}

.sl-empty-title {
  font-size: var(--sl-font-size-sm, 14px);
  font-weight: 600;
  color: var(--sl-text-primary, #ffffff);
  margin: 0;
}

.sl-empty-desc {
  font-size: var(--sl-font-size-xs, 12px);
  color: var(--sl-text-secondary, #8e8e93);
  margin: 0 0 6px 0;
  max-width: 360px;
}

/* Animations */
.sl-slide-fade-enter-active,
.sl-slide-fade-leave-active {
  transition: all 0.2s cubic-bezier(0.16, 1, 0.3, 1);
}

.sl-slide-fade-enter-from,
.sl-slide-fade-leave-to {
  transform: translateY(8px);
  opacity: 0;
}

/* Modal Styles */
.sl-modal-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.7);
  backdrop-filter: blur(8px);
  -webkit-backdrop-filter: blur(8px);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
  padding: 20px;
}

.sl-modal-card {
  background: var(--sl-surface-card, #1c1c1e);
  border: 1px solid var(--sl-border-standard, rgba(255, 255, 255, 0.15));
  border-radius: var(--sl-radius-lg, 12px);
  width: 100%;
  max-width: 520px;
  padding: 24px;
  display: flex;
  flex-direction: column;
  gap: 16px;
  box-shadow: 0 16px 40px rgba(0, 0, 0, 0.5);
}

.sl-modal-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.sl-modal-title {
  font-size: 16px;
  font-weight: 600;
  color: var(--sl-text-primary, #ffffff);
  margin: 0;
}

.sl-modal-close {
  background: transparent;
  border: none;
  font-size: 20px;
  color: var(--sl-text-tertiary, #636366);
  cursor: pointer;
  padding: 4px;
}

.sl-modal-close:hover {
  color: var(--sl-text-primary, #ffffff);
}

.sl-modal-desc {
  font-size: 13px;
  color: var(--sl-text-secondary, #8e8e93);
  margin: 0;
  line-height: 1.5;
}

.sl-modal-body {
  display: flex;
  flex-direction: column;
  gap: 14px;
}

.sl-form-group {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.sl-form-label {
  font-size: 12px;
  font-weight: 600;
  color: var(--sl-text-secondary, #8e8e93);
}

.sl-radio-group {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.sl-radio-item,
.sl-checkbox-item {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
  color: var(--sl-text-primary, #ffffff);
  cursor: pointer;
}

.sl-modal-result-box {
  background: rgba(48, 209, 88, 0.1);
  border: 1px solid rgba(48, 209, 88, 0.25);
  border-radius: var(--sl-radius-md, 8px);
  padding: 10px 12px;
}

.sl-result-title {
  font-size: 12px;
  font-weight: 600;
  color: #30d158;
  margin-bottom: 4px;
}

.sl-result-summary {
  font-size: 12px;
  color: var(--sl-text-primary, #ffffff);
}

.sl-modal-error {
  background: rgba(255, 69, 58, 0.1);
  border: 1px solid rgba(255, 69, 58, 0.25);
  border-radius: var(--sl-radius-md, 8px);
  padding: 10px 14px;
  font-size: 12px;
  color: #ff453a;
}

.sl-modal-footer {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 10px;
  margin-top: 6px;
}

.sl-modal-btn-cancel {
  padding: 7px 14px;
  background: var(--sl-surface-elevated, #2c2c2e);
  border: 1px solid var(--sl-border-standard, rgba(255, 255, 255, 0.15));
  border-radius: var(--sl-radius-sm, 6px);
  color: var(--sl-text-primary, #ffffff);
  font-size: 12.5px;
  cursor: pointer;
}

.sl-modal-btn-confirm {
  padding: 7px 14px;
  background: var(--sl-color-accent, #0a84ff);
  border: none;
  border-radius: var(--sl-radius-sm, 6px);
  color: #ffffff;
  font-size: 12.5px;
  font-weight: 600;
  cursor: pointer;
}

.sl-modal-btn-confirm:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

@media (max-width: 960px) {
  .sl-task-center-layout {
    padding: 12px 14px;
  }
}
</style>
