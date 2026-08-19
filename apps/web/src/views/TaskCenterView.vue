<template>
  <div class="sl-task-center-layout">
    <!-- 1. 顶部 7 状态统计面板 -->
    <BatchStatsCards />

    <!-- 2. 批量多模态导入区 (支持文件夹/文件拖拽、工作区一键导入、路径粘贴与扫描摘要横幅) -->
    <BatchDropZone />

    <!-- 3. 操作与列表主卡片 -->
    <div class="sl-main-queue-card">
      <!-- 队列全局操作工具栏 -->
      <div class="sl-queue-toolbar">
        <div class="sl-toolbar-left">
          <span class="sl-toolbar-title">批量任务队列</span>
          <span class="sl-badge-count">
            {{ batchStore.filteredTasks.length }} / {{ batchStore.tasks.length }} 项
          </span>

          <!-- 搜索框 -->
          <div class="sl-search-box">
            <svg class="sl-search-icon" viewBox="0 0 16 16" fill="currentColor">
              <path d="M11.742 10.344a6.5 6.5 0 1 0-1.397 1.398h-.001c.03.04.062.078.098.115l3.85 3.85a1 1 0 0 0 1.415-1.414l-3.85-3.85a1.007 1.007 0 0 0-.115-.1zM12 6.5a5.5 5.5 0 1 1-11 0 5.5 5.5 0 0 1 11 0z"/>
            </svg>
            <input 
              v-model="batchStore.searchText" 
              type="text" 
              placeholder="搜索视频名、路径或目录..." 
              class="sl-search-input"
            />
            <button 
              v-if="batchStore.searchText" 
              type="button" 
              class="sl-search-clear"
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
            @click="batchStore.startQueue()"
          >
            <svg class="sl-btn-icon-svg" viewBox="0 0 16 16" fill="currentColor">
              <path d="m11.596 8.697-6.363 3.692c-.54.313-1.233-.066-1.233-.697V4.308c0-.63.692-1.01 1.233-.696l6.363 3.692a.802.802 0 0 1 0 1.393z"/>
            </svg>
            <span>开始全部任务</span>
          </button>

          <!-- 暂停队列 -->
          <button 
            v-else
            class="sl-btn sl-btn--warning"
            @click="batchStore.pauseQueue()"
          >
            <svg class="sl-btn-icon-svg" viewBox="0 0 16 16" fill="currentColor">
              <path d="M5.5 3.5A1.5 1.5 0 0 1 7 5v6a1.5 1.5 0 0 1-3 0V5a1.5 1.5 0 0 1 1.5-1.5zm5 0A1.5 1.5 0 0 1 12 5v6a1.5 1.5 0 0 1-3 0V5a1.5 1.5 0 0 1 1.5-1.5z"/>
            </svg>
            <span>暂停队列</span>
          </button>

          <!-- 一键导出所有已完成 SRT -->
          <button 
            class="sl-btn sl-btn--secondary"
            :disabled="!batchStore.canExportAll"
            @click="batchStore.exportAllCompleted()"
          >
            <svg class="sl-btn-icon-svg" viewBox="0 0 16 16" fill="currentColor">
              <path d="M.5 9.9a.5.5 0 0 1 .5.5v2.5a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-2.5a.5.5 0 0 1 1 0v2.5a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2v-2.5a.5.5 0 0 1 .5-.5z"/>
              <path d="M7.646 11.854a.5.5 0 0 0 .708 0l3-3a.5.5 0 0 0-.708-.708L8.5 10.293V1.5a.5.5 0 0 0-1 0v8.793L5.354 8.146a.5.5 0 1 0-.708.708l3 3z"/>
            </svg>
            <span>批量导出 SRT</span>
          </button>

          <!-- 清空已完成 -->
          <button 
            class="sl-btn sl-btn--ghost"
            :disabled="!batchStore.canClearCompleted"
            @click="batchStore.clearCompleted()"
          >
            清空已完成
          </button>
        </div>
      </div>

      <!-- 任务数据表格 -->
      <div class="sl-table-container">
        <table v-if="batchStore.filteredTasks.length > 0" class="sl-task-table">
          <thead>
            <tr>
              <th>视频名称 / 路径</th>
              <th>来源位置</th>
              <th>提取配置</th>
              <th>提取进度</th>
              <th>字幕条目</th>
              <th>耗时</th>
              <th>操作</th>
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
        <div v-else-if="batchStore.tasks.length > 0" class="sl-empty-state">
          <div class="sl-empty-icon-wrap">
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
            @click="clearFilters"
          >
            清除筛选与搜索
          </button>
        </div>

        <!-- 队列完全为空的空态 -->
        <div v-else class="sl-empty-state">
          <div class="sl-empty-icon-wrap">
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
  </div>
</template>

<script setup lang="ts">
import BatchStatsCards from '../components/BatchStatsCards.vue';
import BatchDropZone from '../components/BatchDropZone.vue';
import BatchTaskRow from '../components/BatchTaskRow.vue';
import BatchTaskInspector from '../components/BatchTaskInspector.vue';
import { useBatchStore } from '../stores/batch';

const batchStore = useBatchStore();

function clearFilters() {
  batchStore.searchText = '';
  batchStore.statusFilter = 'all';
}
</script>

<style scoped>
.sl-task-center-layout {
  display: flex;
  flex-direction: column;
  gap: 16px;
  max-width: 1360px;
  margin: 0 auto;
  padding: 20px 24px;
}

.sl-main-queue-card {
  background: var(--sl-surface-card, #1c1c1e);
  border: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.08));
  border-radius: var(--sl-radius-lg, 10px);
  overflow: hidden;
  display: flex;
  flex-direction: column;
}

/* Toolbar */
.sl-queue-toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  padding: 12px 18px;
  border-bottom: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.08));
  background: var(--sl-surface-elevated, #242426);
  flex-wrap: wrap;
}

.sl-toolbar-left {
  display: flex;
  align-items: center;
  gap: 12px;
  flex: 1;
  min-width: 280px;
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
  border-radius: var(--sl-radius-full, 999px);
  color: var(--sl-text-secondary, #8e8e93);
  font-variant-numeric: tabular-nums;
}

.sl-search-box {
  position: relative;
  display: flex;
  align-items: center;
  flex: 1;
  max-width: 320px;
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
  border-color: var(--sl-color-primary, #0a84ff);
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
}

/* Buttons */
.sl-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  font-size: var(--sl-font-size-xs, 12px);
  font-weight: 500;
  padding: 6px 12px;
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
  width: 14px;
  height: 14px;
}

.sl-btn--primary {
  background: var(--sl-color-primary, #0a84ff);
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
  min-height: 200px;
}

.sl-task-table {
  width: 100%;
  border-collapse: collapse;
  text-align: left;
}

.sl-task-table th {
  padding: 10px 14px;
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
  padding: 48px 24px;
  text-align: center;
  gap: 8px;
}

.sl-empty-icon-wrap {
  width: 48px;
  height: 48px;
  border-radius: var(--sl-radius-lg, 10px);
  background: var(--sl-surface-elevated, #2c2c2e);
  display: flex;
  align-items: center;
  justify-content: center;
  margin-bottom: 4px;
}

.sl-empty-icon {
  width: 24px;
  height: 24px;
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
  margin: 0 0 8px 0;
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
</style>
