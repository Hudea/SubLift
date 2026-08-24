<template>
  <tr 
    class="sl-task-row" 
    :class="[`is-${task.status}`, { 'is-selected': isSelected }]"
    @click="handleRowClick"
  >
    <!-- 1. 状态指示灯与名称 -->
    <td class="sl-cell-name">
      <div class="sl-name-wrapper">
        <span class="sl-beacon-light" :class="`sl-beacon--${task.status}`"></span>
        <div class="sl-file-meta">
          <div class="sl-file-title-wrap">
            <span class="sl-file-title" :title="task.videoPath">{{ task.name }}</span>
            <span v-if="task.outputExists" class="sl-out-exists-tag" title="对应 SRT 字幕已存在，导出时将原子覆盖">SRT已存在</span>
          </div>
          <span class="sl-file-subpath" :title="task.videoPath">{{ task.videoPath }}</span>
        </div>
      </div>
    </td>

    <!-- 2. 来源位置 -->
    <td class="sl-cell-location">
      <span class="sl-location-tag" :title="task.importRootPath || task.videoPath">
        {{ locationDisplay }}
      </span>
    </td>

    <!-- 3. 引擎与采样配置快照 -->
    <td class="sl-cell-config">
      <div class="sl-tag-group">
        <span class="sl-tag sl-tag--engine">{{ formatEngine(task.config.engine) }}</span>
        <span class="sl-tag sl-tag--quality">{{ formatQuality(task.config.quality) }}</span>
      </div>
    </td>

    <!-- 4. 实时进度与状态 -->
    <td class="sl-cell-progress">
      <div class="sl-progress-wrapper">
        <div class="sl-progress-header">
          <span class="sl-stage-text">{{ getStageLabel(task) }}</span>
          <span v-if="task.progressPct > 0" class="sl-pct-text">{{ task.progressPct }}%</span>
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

    <!-- 5. 字幕条目 -->
    <td class="sl-cell-entries">
      <span v-if="task.entries.length > 0" class="sl-entry-badge">
        {{ task.entries.length }} 句
      </span>
      <span v-else class="sl-muted-text">—</span>
    </td>

    <!-- 6. 耗时 -->
    <td class="sl-cell-time">
      <div class="sl-time-wrapper">
        <span v-if="task.elapsedMs > 0">{{ formatDuration(task.elapsedMs) }}</span>
        <span v-else class="sl-muted-text">—</span>
      </div>
    </td>

    <!-- 7. 行级操作按钮 -->
    <td class="sl-cell-actions" @click.stop>
      <div class="sl-action-buttons">
        <!-- 启动单项 (仅 waiting 可用) -->
        <button
          v-if="task.status === 'waiting'"
          class="sl-btn-icon sl-btn-icon--primary"
          title="立即单独启动此任务"
          aria-label="立即单独启动此任务"
          :disabled="batchStore.isQueueRunning || batchStore.currentRunningId !== null"
          @click="batchStore.startSingle(task.id)"
        >
          ▶
        </button>

        <!-- 上移 (仅 waiting 可用) -->
        <button
          v-if="task.status === 'waiting'"
          class="sl-btn-icon"
          title="上移排队顺序"
          aria-label="上移排队顺序"
          @click="batchStore.moveTaskUp(task.id)"
        >
          ↑
        </button>

        <!-- 下移 (仅 waiting 可用) -->
        <button
          v-if="task.status === 'waiting'"
          class="sl-btn-icon"
          title="下移排队顺序"
          aria-label="下移排队顺序"
          @click="batchStore.moveTaskDown(task.id)"
        >
          ↓
        </button>

        <!-- 取消 (处理中或排队中) -->
        <button
          v-if="BatchTaskStatusGuard.canCancel(task.status)"
          class="sl-btn-icon sl-btn-icon--warning"
          title="取消任务"
          aria-label="取消任务"
          @click="batchStore.cancelTask(task.id)"
        >
          ✕
        </button>

        <!-- 重试 (失败、取消或中断) -->
        <button
          v-if="BatchTaskStatusGuard.canRetry(task.status)"
          class="sl-btn-icon sl-btn-icon--primary"
          title="重新排队"
          aria-label="重新排队"
          @click="batchStore.retryTask(task.id)"
        >
          ↺
        </button>

        <!-- 单任务下载 SRT (完成态) -->
        <button
          v-if="task.status === 'completed' && task.entries.length > 0"
          class="sl-btn-icon sl-btn-icon--accent"
          title="下载 SRT 字幕"
          aria-label="下载 SRT 字幕"
          @click="batchStore.exportTaskSrt(task.id)"
        >
          ⬇
        </button>

        <!-- 移除任务 -->
        <button
          v-if="BatchTaskStatusGuard.canRemove(task.status)"
          class="sl-btn-icon sl-btn-icon--danger"
          title="从队列中移除"
          aria-label="从队列中移除"
          @click="batchStore.removeTask(task.id)"
        >
          🗑
        </button>
      </div>
    </td>
  </tr>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useBatchStore } from '../stores/batch';
import { BatchTaskStatusGuard } from '../utils/batch_guards';
import { SAMPLING_QUALITY_MAP, type BatchTaskItem, type SamplingQuality } from '../types/batch';

const props = defineProps<{
  task: BatchTaskItem;
}>();

const batchStore = useBatchStore();

const isSelected = computed(() => batchStore.selectedTaskId === props.task.id);

const locationDisplay = computed(() => {
  if (props.task.importRootPath) return props.task.importRootPath;
  const clean = props.task.videoPath.replace(/\\/g, '/');
  const parts = clean.split('/').filter((p) => p.length > 0);
  if (parts.length > 1) {
    return `${parts[parts.length - 2]}/`;
  }
  return '根目录';
});

function handleRowClick() {
  batchStore.selectTask(props.task.id);
}

function formatEngine(engine: string): string {
  switch (engine) {
    case 'vision':
      return 'Vision';
    case 'paddle':
      return 'Paddle';
    default:
      return engine.toUpperCase();
  }
}

function formatQuality(quality: SamplingQuality): string {
  return SAMPLING_QUALITY_MAP[quality]?.label || quality;
}

function getStageLabel(task: BatchTaskItem): string {
  switch (task.status) {
    case 'waiting':
      return '等待中';
    case 'preparing':
      return '准备中...';
    case 'extracting':
      return task.stage && task.stage !== 'extracting' ? task.stage : '正在提取...';
    case 'exporting':
      return '正在导出...';
    case 'completed':
      return '提取完成';
    case 'failed':
      return task.error ? '处理失败' : '失败';
    case 'cancelled':
      return '已取消';
    case 'interrupted':
      return '已中断';
    case 'skipped':
      return '已跳过';
  }
}

function formatDuration(ms: number): string {
  const sec = Math.floor(ms / 1000);
  if (sec < 60) return `${sec}s`;
  const min = Math.floor(sec / 60);
  const remSec = sec % 60;
  return `${min}m ${remSec}s`;
}
</script>

<style scoped>
.sl-task-row {
  border-bottom: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.06));
  transition: background-color 0.15s ease, box-shadow 0.15s ease;
  cursor: pointer;
  user-select: none;
}

.sl-task-row:hover {
  background-color: var(--sl-surface-elevated, #242426);
}

.sl-task-row.is-selected {
  background-color: rgba(10, 132, 255, 0.08);
  box-shadow: inset 2px 0 0 var(--sl-color-primary, #0a84ff);
}

td {
  padding: 10px 14px;
  vertical-align: middle;
  font-size: var(--sl-font-size-xs, 12px);
}

/* 1. Name */
.sl-name-wrapper {
  display: flex;
  align-items: center;
  gap: 10px;
  min-width: 0;
}

.sl-beacon-light {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  flex-shrink: 0;
}

.sl-beacon--waiting { background: #8e8e93; }
.sl-beacon--preparing,
.sl-beacon--extracting,
.sl-beacon--exporting { 
  background: #ff9f0a; 
  box-shadow: 0 0 6px rgba(255, 159, 10, 0.5);
  animation: pulse-dot 1.5s infinite ease-in-out;
}
.sl-beacon--completed { background: #30d158; }
.sl-beacon--failed { background: #ff453a; }
.sl-beacon--cancelled { background: #636366; }
.sl-beacon--interrupted { background: #ff453a; }
.sl-beacon--skipped { background: #bf5af2; }

@keyframes pulse-dot {
  0%, 100% { opacity: 1; transform: scale(1); }
  50% { opacity: 0.5; transform: scale(1.3); }
}

.sl-file-meta {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}

.sl-file-title-wrap {
  display: flex;
  align-items: center;
  gap: 6px;
  min-width: 0;
}

.sl-file-title {
  font-weight: 500;
  color: var(--sl-text-primary, #ffffff);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  max-width: 240px;
}

.sl-out-exists-tag {
  font-size: 10px;
  font-weight: 500;
  padding: 1px 4px;
  border-radius: 3px;
  background: rgba(255, 159, 10, 0.15);
  border: 1px solid rgba(255, 159, 10, 0.3);
  color: #ff9f0a;
  flex-shrink: 0;
  white-space: nowrap;
}

.sl-file-subpath {
  font-size: 11px;
  color: var(--sl-text-tertiary, #636366);
  font-family: var(--sl-font-mono, monospace);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  max-width: 260px;
}

/* 2. Location */
.sl-location-tag {
  display: inline-block;
  font-size: 11px;
  padding: 2px 6px;
  border-radius: 4px;
  background: var(--sl-surface-base, #161618);
  border: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.08));
  color: var(--sl-text-secondary, #8e8e93);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  max-width: 140px;
}

/* 3. Config */
.sl-tag-group {
  display: flex;
  align-items: center;
  gap: 4px;
}

.sl-tag {
  font-size: 11px;
  padding: 2px 6px;
  border-radius: 4px;
  background: var(--sl-surface-elevated, #2c2c2e);
  border: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.1));
  color: var(--sl-text-secondary, #8e8e93);
}

.sl-tag--engine {
  color: #5ac8fa;
}

.sl-tag--quality {
  color: #ffd60a;
}

/* 4. Progress */
.sl-progress-wrapper {
  display: flex;
  flex-direction: column;
  gap: 4px;
  width: 130px;
}

.sl-progress-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: 11px;
}

.sl-stage-text {
  color: var(--sl-text-secondary, #8e8e93);
}

.sl-pct-text {
  font-weight: 600;
  color: var(--sl-text-primary, #ffffff);
  font-variant-numeric: tabular-nums;
}

.sl-progress-track {
  width: 100%;
  height: 4px;
  background: var(--sl-surface-base, #161618);
  border-radius: 2px;
  overflow: hidden;
}

.sl-progress-fill {
  height: 100%;
  border-radius: 2px;
  transition: width 0.2s ease;
}

.sl-fill--waiting { background: #636366; }
.sl-fill--preparing,
.sl-fill--extracting,
.sl-fill--exporting { background: #ff9f0a; }
.sl-fill--completed { background: #30d158; }
.sl-fill--failed { background: #ff453a; }
.sl-fill--cancelled { background: #48484a; }

/* 5. Entries */
.sl-entry-badge {
  font-size: 11px;
  font-weight: 500;
  color: #30d158;
}

.sl-muted-text {
  color: var(--sl-text-tertiary, #636366);
}

/* 6. Time */
.sl-time-wrapper {
  color: var(--sl-text-secondary, #8e8e93);
  font-variant-numeric: tabular-nums;
}

/* 7. Actions */
.sl-action-buttons {
  display: flex;
  align-items: center;
  gap: 4px;
}

.sl-btn-icon {
  width: 24px;
  height: 24px;
  border-radius: 4px;
  border: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.1));
  background: var(--sl-surface-elevated, #2c2c2e);
  color: var(--sl-text-secondary, #8e8e93);
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 11px;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.15s ease;
}

.sl-btn-icon:hover {
  background: var(--sl-surface-active, #3a3a3c);
  color: var(--sl-text-primary, #ffffff);
}

.sl-btn-icon--primary {
  color: var(--sl-color-primary, #0a84ff);
}

.sl-btn-icon--primary:hover {
  background: rgba(10, 132, 255, 0.15);
}

.sl-btn-icon--warning {
  color: #ff9f0a;
}

.sl-btn-icon--accent {
  color: #30d158;
}

.sl-btn-icon--danger:hover {
  color: #ff453a;
  background: rgba(255, 69, 58, 0.15);
}

.sl-btn-icon:disabled {
  opacity: 0.3;
  cursor: not-allowed;
}
</style>
