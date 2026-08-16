<template>
  <div class="sl-task-center-layout">
    <!-- 1. 顶部统计面板 -->
    <BatchStatsCards 
      :active-filter="activeFilter" 
      @update:active-filter="activeFilter = $event" 
    />

    <!-- 2. 批量多选导入区 -->
    <BatchDropZone />

    <!-- 3. 操作与列表主卡片 -->
    <div class="sl-main-queue-card">
      <!-- 队列全局操作工具栏 -->
      <div class="sl-queue-toolbar">
        <div class="sl-toolbar-left">
          <span class="sl-toolbar-title">批量任务队列</span>
          <span class="sl-badge-count">{{ displayedTasks.length }} / {{ batchStore.tasks.length }} 项</span>
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
        <table v-if="displayedTasks.length > 0" class="sl-task-table">
          <thead>
            <tr>
              <th>视频名称 / 路径</th>
              <th>配置</th>
              <th>提取进度</th>
              <th>字幕条目</th>
              <th>耗时</th>
              <th>操作</th>
            </tr>
          </thead>
          <tbody>
            <BatchTaskRow 
              v-for="task in displayedTasks" 
              :key="task.id" 
              :task="task" 
            />
          </tbody>
        </table>

        <!-- 空态设计 -->
        <div v-else class="sl-empty-state">
          <div class="sl-empty-icon-wrap">
            <svg class="sl-empty-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor">
              <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z" stroke-width="1.6"/>
              <polyline points="14 2 14 8 20 8" stroke-width="1.6"/>
              <line x1="16" y1="13" x2="8" y2="13" stroke-width="1.6"/>
              <line x1="16" y1="17" x2="8" y2="17" stroke-width="1.6"/>
            </svg>
          </div>
          <p class="sl-empty-title">
            {{ activeFilter === 'all' ? '队列为空' : `无「${getFilterName(activeFilter)}」状态的任务` }}
          </p>
          <p class="sl-empty-desc">拖拽多个视频到上方区域，即可批量导入排队</p>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed } from 'vue';
import BatchStatsCards from '../components/BatchStatsCards.vue';
import BatchDropZone from '../components/BatchDropZone.vue';
import BatchTaskRow from '../components/BatchTaskRow.vue';
import { useBatchStore } from '../stores/batch';

const batchStore = useBatchStore();
const activeFilter = ref<string>('all');

const displayedTasks = computed(() => {
  if (activeFilter.value === 'all') {
    return batchStore.tasks;
  }
  return batchStore.tasks.filter((t) => t.status === activeFilter.value);
});

function getFilterName(filter: string): string {
  const map: Record<string, string> = {
    waiting: '排队中',
    running: '处理中',
    completed: '已完成',
    failed: '失败',
  };
  return map[filter] || filter;
}
</script>

<style scoped>
.sl-task-center-layout {
  display: flex;
  flex-direction: column;
  gap: 16px;
  width: 100%;
  height: calc(100vh - 48px);
  padding: 16px 20px;
  background: var(--sl-surface-ground);
  overflow-y: auto;
}

.sl-main-queue-card {
  display: flex;
  flex-direction: column;
  flex: 1;
  background: var(--sl-surface-panel);
  border: 1px solid var(--sl-border-subtle);
  border-radius: var(--sl-radius-lg);
  box-shadow: var(--sl-shadow-sm), var(--sl-inner-highlight);
  overflow: hidden;
}

.sl-queue-toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 10px 16px;
  background: var(--sl-surface-card);
  border-bottom: 1px solid var(--sl-border-subtle);
}

.sl-toolbar-left {
  display: flex;
  align-items: center;
  gap: 8px;
}

.sl-toolbar-title {
  font-size: var(--sl-font-size-base);
  font-weight: 600;
  color: var(--sl-text-primary);
}

.sl-badge-count {
  font-size: 11px;
  font-family: var(--sl-font-family-mono);
  background: var(--sl-surface-elevated);
  border: 1px solid var(--sl-border-subtle);
  padding: 1px 6px;
  border-radius: var(--sl-radius-pill);
  color: var(--sl-text-tertiary);
}

.sl-toolbar-right {
  display: flex;
  align-items: center;
  gap: 8px;
}

.sl-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  height: 30px;
  padding: 0 12px;
  border-radius: var(--sl-radius-sm);
  font-size: var(--sl-font-size-xs);
  font-weight: 500;
  cursor: pointer;
  transition: var(--sl-transition-snappy);
  border: none;
}

.sl-btn--primary {
  background: var(--sl-color-accent);
  color: #ffffff;
}
.sl-btn--primary:hover:not(:disabled) {
  background: var(--sl-color-accent-hover);
}

.sl-btn--warning {
  background: var(--sl-color-processing);
  color: #000;
  font-weight: 600;
}

.sl-btn--secondary {
  background: var(--sl-surface-elevated);
  border: 1px solid var(--sl-border-standard);
  color: var(--sl-text-secondary);
}
.sl-btn--secondary:hover:not(:disabled) {
  color: var(--sl-text-primary);
  border-color: var(--sl-border-focus);
}

.sl-btn--ghost {
  background: transparent;
  color: var(--sl-text-tertiary);
}
.sl-btn--ghost:hover:not(:disabled) {
  color: var(--sl-text-primary);
}

.sl-btn:disabled {
  opacity: 0.35;
  cursor: not-allowed;
}

.sl-btn-icon-svg {
  width: 12px;
  height: 12px;
}

.sl-table-container {
  flex: 1;
  overflow-y: auto;
}

.sl-task-table {
  width: 100%;
  border-collapse: collapse;
  text-align: left;
}

.sl-task-table th {
  padding: 10px 14px;
  font-size: 11px;
  font-weight: 500;
  color: var(--sl-text-tertiary);
  background: var(--sl-surface-base);
  border-bottom: 1px solid var(--sl-border-standard);
}

.sl-empty-state {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  padding: 60px 20px;
}

.sl-empty-icon-wrap {
  width: 48px;
  height: 48px;
  border-radius: var(--sl-radius-md);
  background: var(--sl-surface-card);
  border: 1px solid var(--sl-border-subtle);
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--sl-text-tertiary);
  margin-bottom: 12px;
}

.sl-empty-icon {
  width: 24px;
  height: 24px;
}

.sl-empty-title {
  font-size: var(--sl-font-size-md);
  font-weight: 500;
  color: var(--sl-text-secondary);
  margin-bottom: 4px;
}

.sl-empty-desc {
  font-size: var(--sl-font-size-xs);
  color: var(--sl-text-tertiary);
}
</style>
