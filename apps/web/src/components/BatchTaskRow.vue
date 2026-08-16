<template>
  <tr class="sl-task-row" :class="`is-${task.status}`">
    <!-- 1. 状态指示灯与名称 -->
    <td class="sl-cell-name">
      <div class="sl-name-wrapper">
        <span class="sl-beacon-light" :class="`sl-beacon--${task.status}`"></span>
        <div class="sl-file-meta">
          <span class="sl-file-title" :title="task.videoPath">{{ task.name }}</span>
          <span class="sl-file-subpath" :title="task.videoPath">{{ task.videoPath }}</span>
        </div>
      </div>
    </td>

    <!-- 2. 引擎与采样配置快照 -->
    <td class="sl-cell-config">
      <div class="sl-tag-group">
        <span class="sl-tag">{{ task.config.engine.toUpperCase() }}</span>
        <span class="sl-tag">{{ task.config.fps }} FPS</span>
      </div>
    </td>

    <!-- 3. 实时进度与状态 -->
    <td class="sl-cell-progress">
      <div class="sl-progress-wrapper">
        <div class="sl-progress-header">
          <span class="sl-stage-text">{{ getStageLabel(task) }}</span>
          <span class="sl-pct-text">{{ task.progressPct }}%</span>
        </div>
        <div class="sl-progress-track">
          <div 
            class="sl-progress-fill" 
            :class="`sl-fill--${task.status}`"
            :style="{ width: `${task.progressPct}%` }"
          ></div>
        </div>
      </div>
    </td>

    <!-- 4. 统计与条目 -->
    <td class="sl-cell-entries">
      <span v-if="task.entries.length > 0" class="sl-entry-badge">
        {{ task.entries.length }} 句
      </span>
      <span v-else class="sl-muted-text">—</span>
    </td>

    <!-- 5. 耗时 -->
    <td class="sl-cell-time">
      <div class="sl-time-wrapper">
        <span v-if="task.elapsedMs > 0">{{ formatDuration(task.elapsedMs) }}</span>
        <span v-else class="sl-muted-text">—</span>
      </div>
    </td>

    <!-- 6. 行级操作按钮 -->
    <td class="sl-cell-actions">
      <div class="sl-action-buttons">
        <!-- 取消 (处理中或排队中) -->
        <button
          v-if="task.status === 'running' || task.status === 'waiting'"
          class="sl-btn-icon"
          title="取消任务"
          @click="batchStore.cancelTask(task.id)"
        >
          <svg class="sl-icon" viewBox="0 0 16 16" fill="currentColor">
            <path d="M4.646 4.646a.5.5 0 0 1 .708 0L8 7.293l2.646-2.647a.5.5 0 0 1 .708.708L8.707 8l2.647 2.646a.5.5 0 0 1-.708.708L8 8.707l-2.646 2.647a.5.5 0 0 1-.708-.708L7.293 8 4.646 5.354a.5.5 0 0 1 0-.708z"/>
          </svg>
        </button>

        <!-- 重试 (失败或取消) -->
        <button
          v-if="task.status === 'failed' || task.status === 'cancelled'"
          class="sl-btn-icon"
          title="重新排队"
          @click="batchStore.retryTask(task.id)"
        >
          <svg class="sl-icon" viewBox="0 0 16 16" fill="currentColor">
            <path d="M11.534 7h3.932a.25.25 0 0 1 .192.41l-1.966 2.36a.25.25 0 0 1-.384 0l-1.966-2.36a.25.25 0 0 1 .192-.41zm-11 2h3.932a.25.25 0 0 0 .192-.41L2.692 6.23a.25.25 0 0 0-.384 0L.342 8.59A.25.25 0 0 0 .534 9z"/>
            <path d="M8 3c-1.552 0-2.94.707-3.857 1.818a.5.5 0 1 1-.771-.636A6.002 6.002 0 0 1 13.917 7H12.9A5.002 5.002 0 0 0 8 3zM3.1 9a5.002 5.002 0 0 0 8.757 2.182.5.5 0 1 1 .771.636A6.002 6.002 0 0 1 2.083 9H3.1z"/>
          </svg>
        </button>

        <!-- 单任务下载 SRT (完成态) -->
        <button
          v-if="task.status === 'completed'"
          class="sl-btn-icon sl-btn-icon--accent"
          title="下载 SRT 字幕"
          @click="batchStore.exportTaskSrt(task.id)"
        >
          <svg class="sl-icon" viewBox="0 0 16 16" fill="currentColor">
            <path d="M.5 9.9a.5.5 0 0 1 .5.5v2.5a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-2.5a.5.5 0 0 1 1 0v2.5a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2v-2.5a.5.5 0 0 1 .5-.5z"/>
            <path d="M7.646 11.854a.5.5 0 0 0 .708 0l3-3a.5.5 0 0 0-.708-.708L8.5 10.293V1.5a.5.5 0 0 0-1 0v8.793L5.354 8.146a.5.5 0 1 0-.708.708l3 3z"/>
          </svg>
        </button>

        <!-- 删除任务 -->
        <button
          class="sl-btn-icon sl-btn-icon--danger"
          title="移除任务"
          @click="batchStore.removeTask(task.id)"
        >
          <svg class="sl-icon" viewBox="0 0 16 16" fill="currentColor">
            <path d="M5.5 5.5A.5.5 0 0 1 6 6v6a.5.5 0 0 1-1 0V6a.5.5 0 0 1 .5-.5zm2.5 0a.5.5 0 0 1 .5.5v6a.5.5 0 0 1-1 0V6a.5.5 0 0 1 .5-.5zm3 .5a.5.5 0 0 0-1 0v6a.5.5 0 0 0 1 0V6z"/>
            <path fill-rule="evenodd" d="M14.5 3a1 1 0 0 1-1 1H13v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V4h-.5a1 1 0 0 1-1-1V2a1 1 0 0 1 1-1H6a1 1 0 0 1 1-1h2a1 1 0 0 1 1 1h3.5a1 1 0 0 1 1 1v1zM4.118 4 4 4.059V13a1 1 0 0 0 1 1h6a1 1 0 0 0 1-1V4.059L11.882 4H4.118zM2.5 3V2h11v1h-11z"/>
          </svg>
        </button>
      </div>
    </td>
  </tr>
</template>

<script setup lang="ts">
import type { BatchTaskItem } from '../types/batch';
import { useBatchStore } from '../stores/batch';

defineProps<{
  task: BatchTaskItem;
}>();

const batchStore = useBatchStore();

function getStageLabel(task: BatchTaskItem): string {
  if (task.status === 'waiting') return '排队中';
  if (task.status === 'completed') return '已完成';
  if (task.status === 'failed') return task.error ? `失败 (${task.error})` : '失败';
  if (task.status === 'cancelled') return '已取消';
  return task.stage || '处理中';
}

function formatDuration(ms: number): string {
  const totalSec = Math.floor(ms / 1000);
  const m = Math.floor(totalSec / 60);
  const s = totalSec % 60;
  return `${m}分${s < 10 ? '0' : ''}${s}秒`;
}
</script>

<style scoped>
.sl-task-row {
  border-bottom: 1px solid var(--sl-border-subtle);
  transition: var(--sl-transition-snappy);
}

.sl-task-row:hover {
  background: var(--sl-surface-card-hover);
}

td {
  padding: 10px 14px;
  vertical-align: middle;
}

.sl-cell-name {
  max-width: 280px;
}

.sl-name-wrapper {
  display: flex;
  align-items: center;
  gap: 10px;
}

.sl-beacon-light {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  flex-shrink: 0;
}

.sl-beacon--waiting { background: var(--sl-color-ready); }
.sl-beacon--running {
  background: var(--sl-color-processing);
  box-shadow: 0 0 8px var(--sl-color-processing);
  animation: sl-pulse-fast 1.2s infinite ease-in-out;
}
.sl-beacon--completed { background: var(--sl-color-success); }
.sl-beacon--failed { background: var(--sl-color-error); }
.sl-beacon--cancelled { background: var(--sl-text-disabled); }

.sl-file-meta {
  display: flex;
  flex-direction: column;
  min-width: 0;
}

.sl-file-title {
  font-size: var(--sl-font-size-base);
  font-weight: 500;
  color: var(--sl-text-primary);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.sl-file-subpath {
  font-size: 11px;
  color: var(--sl-text-tertiary);
  font-family: var(--sl-font-family-mono);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.sl-tag-group {
  display: flex;
  gap: 4px;
}

.sl-tag {
  font-size: 10px;
  font-family: var(--sl-font-family-mono);
  background: var(--sl-surface-elevated);
  border: 1px solid var(--sl-border-subtle);
  border-radius: var(--sl-radius-xs);
  padding: 2px 6px;
  color: var(--sl-text-secondary);
}

.sl-progress-wrapper {
  display: flex;
  flex-direction: column;
  gap: 4px;
  width: 130px;
}

.sl-progress-header {
  display: flex;
  justify-content: space-between;
  font-size: 11px;
}

.sl-stage-text { 
  color: var(--sl-text-secondary); 
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.sl-pct-text { 
  font-family: var(--sl-font-family-mono); 
  color: var(--sl-text-tertiary); 
}

.sl-progress-track {
  width: 100%;
  height: 4px;
  background: var(--sl-surface-base);
  border-radius: var(--sl-radius-pill);
  overflow: hidden;
}

.sl-progress-fill {
  height: 100%;
  transition: width 200ms ease;
}

.sl-fill--running { background: var(--sl-color-processing); }
.sl-fill--completed { background: var(--sl-color-success); }
.sl-fill--failed { background: var(--sl-color-error); }
.sl-fill--waiting, .sl-fill--cancelled { background: var(--sl-color-ready); }

.sl-entry-badge {
  font-size: 11px;
  color: var(--sl-color-accent);
  background: var(--sl-color-accent-muted);
  padding: 2px 6px;
  border-radius: var(--sl-radius-xs);
  font-family: var(--sl-font-family-mono);
}

.sl-time-wrapper {
  display: flex;
  flex-direction: column;
  font-size: 11px;
  font-family: var(--sl-font-family-mono);
  color: var(--sl-text-secondary);
}

.sl-muted-text {
  color: var(--sl-text-disabled);
}

.sl-action-buttons {
  display: flex;
  gap: 4px;
}

.sl-btn-icon {
  width: 26px;
  height: 26px;
  display: flex;
  align-items: center;
  justify-content: center;
  background: var(--sl-surface-card);
  border: 1px solid var(--sl-border-subtle);
  border-radius: var(--sl-radius-xs);
  color: var(--sl-text-secondary);
  cursor: pointer;
  transition: var(--sl-transition-snappy);
}

.sl-btn-icon:hover {
  background: var(--sl-surface-elevated);
  color: var(--sl-text-primary);
  border-color: var(--sl-border-standard);
}

.sl-btn-icon--accent:hover {
  background: var(--sl-color-accent-muted);
  color: var(--sl-color-accent);
  border-color: var(--sl-color-accent);
}

.sl-btn-icon--danger:hover {
  background: var(--sl-color-error-bg);
  color: var(--sl-color-error);
  border-color: var(--sl-color-error);
}

.sl-icon {
  width: 12px;
  height: 12px;
}

@keyframes sl-pulse-fast {
  0%, 100% { opacity: 1; transform: scale(1); }
  50% { opacity: 0.3; transform: scale(1.3); }
}
</style>
