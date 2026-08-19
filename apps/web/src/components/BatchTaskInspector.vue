<template>
  <div v-if="inspector" class="sl-inspector-card">
    <!-- 头部：文件名与状态操作栏 -->
    <div class="sl-inspector-header">
      <div class="sl-inspector-title-wrap">
        <h3 class="sl-inspector-filename" :title="inspector.locationFullPath">
          {{ inspector.filename }}
        </h3>
        <span class="sl-status-badge" :class="`sl-status-badge--${inspector.status}`">
          <span class="sl-badge-dot"></span>
          {{ inspector.statusName }}
        </span>
      </div>

      <div class="sl-inspector-actions">
        <!-- 启动单项 -->
        <button 
          v-if="inspector.canStartSingle"
          type="button" 
          class="sl-ins-btn sl-ins-btn--primary"
          title="立即单独启动此任务"
          aria-label="立即单独启动此任务"
          :disabled="batchStore.isQueueRunning || batchStore.currentRunningId !== null"
          @click="batchStore.startSingle(inspector.id)"
        >
          ▶ 启动
        </button>

        <!-- 上移 -->
        <button 
          v-if="inspector.canReorder"
          type="button" 
          class="sl-ins-btn"
          title="上移排队顺序"
          aria-label="上移排队顺序"
          @click="batchStore.moveTaskUp(inspector.id)"
        >
          ↑
        </button>

        <!-- 下移 -->
        <button 
          v-if="inspector.canReorder"
          type="button" 
          class="sl-ins-btn"
          title="下移排队顺序"
          aria-label="下移排队顺序"
          @click="batchStore.moveTaskDown(inspector.id)"
        >
          ↓
        </button>

        <!-- 取消 -->
        <button 
          v-if="inspector.canCancel"
          type="button" 
          class="sl-ins-btn sl-ins-btn--warning"
          title="取消任务"
          aria-label="取消任务"
          @click="batchStore.cancelTask(inspector.id)"
        >
          ✕ 取消
        </button>

        <!-- 重试 -->
        <button 
          v-if="inspector.canRetry"
          type="button" 
          class="sl-ins-btn sl-ins-btn--primary"
          title="重新排队"
          aria-label="重新排队"
          @click="batchStore.retryTask(inspector.id)"
        >
          ↺ 重试
        </button>

        <!-- 导出已完成 -->
        <button 
          v-if="inspector.status === 'completed' && inspector.entryCount > 0"
          type="button" 
          class="sl-ins-btn sl-ins-btn--success"
          title="下载 SRT"
          aria-label="下载 SRT"
          @click="batchStore.exportTaskSrt(inspector.id)"
        >
          ⬇ 下载
        </button>

        <!-- 删除 -->
        <button 
          v-if="inspector.canRemove"
          type="button" 
          class="sl-ins-btn sl-ins-btn--danger"
          title="从队列中移除"
          aria-label="从队列中移除"
          @click="batchStore.removeTask(inspector.id)"
        >
          🗑 移除
        </button>
      </div>
    </div>

    <!-- 错误诊断 Callout -->
    <div v-if="inspector.failureMessage" class="sl-inspector-error-box">
      <div class="sl-error-title">⚠️ 任务执行失败</div>
      <div class="sl-error-content">{{ inspector.failureMessage }}</div>
    </div>

    <!-- 属性网格 -->
    <div class="sl-inspector-body">
      <!-- 1. 来源与位置 -->
      <div class="sl-ins-row">
        <span class="sl-ins-label">来源位置</span>
        <div class="sl-ins-value-group">
          <span class="sl-ins-tag">{{ inspector.locationDisplay }}</span>
          <span class="sl-ins-path" :title="inspector.locationFullPath">{{ inspector.locationFullPath }}</span>
        </div>
      </div>

      <!-- 2. 输出规划 -->
      <div class="sl-ins-row">
        <span class="sl-ins-label">输出目标</span>
        <div class="sl-ins-value-group">
          <span class="sl-ins-path" :title="inspector.outputFullPath">{{ inspector.outputFullPath || '—' }}</span>
          <span v-if="inspector.outputExistsWarning" class="sl-ins-warning-hint">
            ⚠️ {{ inspector.outputExistsWarning }}
          </span>
        </div>
      </div>

      <!-- 3. 提取配置（Waiting 状态可编辑，其余状态锁定） -->
      <div class="sl-ins-row">
        <span class="sl-ins-label">提取配置</span>
        <div v-if="inspector.canEditConfiguration" class="sl-ins-config-edit">
          <!-- 引擎选择 -->
          <select 
            :value="inspector.engine" 
            class="sl-ins-select"
            @change="handleEngineChange"
          >
            <option value="vision">Apple Vision (原生极速)</option>
            <option value="paddle">PaddleOCR (通用多语言)</option>
          </select>

          <!-- 采样质量选择 -->
          <select 
            :value="inspector.quality" 
            class="sl-ins-select"
            @change="handleQualityChange"
          >
            <option value="fast">⚡ 快速 (5 FPS 推荐)</option>
            <option value="balanced">⚖️ 平衡 (8 FPS)</option>
            <option value="fine">🔬 精细 (12 FPS)</option>
          </select>
        </div>
        <div v-else class="sl-ins-value-group">
          <div class="sl-ins-locked-config">
            <span class="sl-ins-tag">{{ inspector.engineDisplay }}</span>
            <span class="sl-ins-tag">{{ inspector.qualityDisplay }}</span>
            <span class="sl-ins-lock-hint">🔒 已锁定</span>
          </div>
        </div>
      </div>

      <!-- 4. 统计与元数据 -->
      <div class="sl-ins-row">
        <span class="sl-ins-label">处理结果</span>
        <div class="sl-ins-meta-chips">
          <span class="sl-meta-chip">
            字幕条目: <strong>{{ inspector.entryCount }}</strong>
          </span>
          <span v-if="inspector.runtimeIdentity" class="sl-meta-chip">
            Runtime: <strong>{{ inspector.runtimeIdentity }}</strong>
          </span>
          <span class="sl-meta-chip">
            Task ID: <code>{{ inspector.id }}</code>
          </span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useBatchStore } from '../stores/batch';
import type { OcrEngineName } from '../types/api';
import type { SamplingQuality } from '../types/batch';

const batchStore = useBatchStore();
const inspector = computed(() => batchStore.inspectorModel);

function handleEngineChange(e: Event) {
  if (!inspector.value) return;
  const target = e.target as HTMLSelectElement;
  batchStore.updateTaskConfig(inspector.value.id, {
    engine: target.value as OcrEngineName,
  });
}

function handleQualityChange(e: Event) {
  if (!inspector.value) return;
  const target = e.target as HTMLSelectElement;
  batchStore.updateTaskConfig(inspector.value.id, {
    quality: target.value as SamplingQuality,
  });
}
</script>

<style scoped>
.sl-inspector-card {
  background: var(--sl-surface-card, #1c1c1e);
  border: 1px solid var(--sl-border-standard, rgba(255, 255, 255, 0.15));
  border-radius: var(--sl-radius-lg, 10px);
  padding: 16px 20px;
  display: flex;
  flex-direction: column;
  gap: 14px;
}

.sl-inspector-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  flex-wrap: wrap;
  padding-bottom: 12px;
  border-bottom: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.08));
}

.sl-inspector-title-wrap {
  display: flex;
  align-items: center;
  gap: 12px;
  min-width: 0;
  flex: 1;
}

.sl-inspector-filename {
  font-size: var(--sl-font-size-md, 15px);
  font-weight: 600;
  color: var(--sl-text-primary, #ffffff);
  margin: 0;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.sl-status-badge {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  font-size: 11px;
  font-weight: 600;
  padding: 3px 8px;
  border-radius: var(--sl-radius-full, 999px);
  background: var(--sl-surface-elevated, #2c2c2e);
  color: var(--sl-text-secondary, #8e8e93);
  flex-shrink: 0;
}

.sl-badge-dot {
  width: 5px;
  height: 5px;
  border-radius: 50%;
  background: currentColor;
}

.sl-status-badge--waiting { color: #8e8e93; }
.sl-status-badge--preparing,
.sl-status-badge--extracting,
.sl-status-badge--exporting { color: #ff9f0a; background: rgba(255, 159, 10, 0.12); }
.sl-status-badge--completed { color: #30d158; background: rgba(48, 209, 88, 0.12); }
.sl-status-badge--failed { color: #ff453a; background: rgba(255, 69, 58, 0.12); }
.sl-status-badge--cancelled { color: #636366; }
.sl-status-badge--skipped { color: #bf5af2; }

.sl-inspector-actions {
  display: flex;
  align-items: center;
  gap: 6px;
  flex-shrink: 0;
}

.sl-ins-btn {
  font-size: var(--sl-font-size-xs, 12px);
  font-weight: 500;
  padding: 5px 10px;
  border-radius: var(--sl-radius-sm, 6px);
  border: 1px solid var(--sl-border-standard, rgba(255, 255, 255, 0.15));
  background: var(--sl-surface-elevated, #2c2c2e);
  color: var(--sl-text-primary, #ffffff);
  cursor: pointer;
  transition: all 0.15s ease;
}

.sl-ins-btn:hover {
  background: var(--sl-surface-active, #3a3a3c);
}

.sl-ins-btn--primary {
  background: var(--sl-color-primary, #0a84ff);
  border-color: var(--sl-color-primary, #0a84ff);
  color: #ffffff;
}

.sl-ins-btn--warning {
  color: #ff9f0a;
}

.sl-ins-btn--danger {
  color: #ff453a;
}

.sl-ins-btn--success {
  background: rgba(48, 209, 88, 0.15);
  border-color: rgba(48, 209, 88, 0.3);
  color: #30d158;
}

.sl-ins-btn:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

/* Error Box */
.sl-inspector-error-box {
  background: rgba(255, 69, 58, 0.1);
  border: 1px solid rgba(255, 69, 58, 0.25);
  border-radius: var(--sl-radius-md, 8px);
  padding: 10px 14px;
}

.sl-error-title {
  font-size: 12px;
  font-weight: 600;
  color: #ff453a;
  margin-bottom: 4px;
}

.sl-error-content {
  font-size: 12px;
  color: var(--sl-text-secondary, #8e8e93);
  word-break: break-all;
  font-family: var(--sl-font-mono, monospace);
}

/* Body */
.sl-inspector-body {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.sl-ins-row {
  display: flex;
  align-items: flex-start;
  gap: 16px;
}

.sl-ins-label {
  font-size: var(--sl-font-size-xs, 12px);
  font-weight: 500;
  color: var(--sl-text-tertiary, #636366);
  width: 72px;
  flex-shrink: 0;
  padding-top: 4px;
}

.sl-ins-value-group {
  display: flex;
  flex-direction: column;
  gap: 4px;
  flex: 1;
  min-width: 0;
}

.sl-ins-tag {
  display: inline-block;
  font-size: 11px;
  font-weight: 500;
  padding: 2px 6px;
  border-radius: 4px;
  background: var(--sl-surface-elevated, #2c2c2e);
  color: var(--sl-text-secondary, #8e8e93);
  border: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.08));
  width: fit-content;
}

.sl-ins-path {
  font-size: var(--sl-font-size-xs, 12px);
  color: var(--sl-text-secondary, #8e8e93);
  font-family: var(--sl-font-mono, monospace);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.sl-ins-warning-hint {
  font-size: 11px;
  color: var(--sl-color-warning, #ff9f0a);
}

.sl-ins-config-edit {
  display: flex;
  align-items: center;
  gap: 8px;
}

.sl-ins-select {
  background: var(--sl-surface-elevated, #2c2c2e);
  border: 1px solid var(--sl-border-standard, rgba(255, 255, 255, 0.15));
  border-radius: var(--sl-radius-sm, 6px);
  padding: 5px 10px;
  font-size: var(--sl-font-size-xs, 12px);
  color: var(--sl-text-primary, #ffffff);
  cursor: pointer;
  outline: none;
}

.sl-ins-select:focus {
  border-color: var(--sl-color-primary, #0a84ff);
}

.sl-ins-locked-config {
  display: flex;
  align-items: center;
  gap: 6px;
}

.sl-ins-lock-hint {
  font-size: 11px;
  color: var(--sl-text-tertiary, #636366);
}

.sl-ins-meta-chips {
  display: flex;
  align-items: center;
  gap: 12px;
  flex-wrap: wrap;
}

.sl-meta-chip {
  font-size: var(--sl-font-size-xs, 12px);
  color: var(--sl-text-secondary, #8e8e93);
}

.sl-meta-chip strong {
  color: var(--sl-text-primary, #ffffff);
}

.sl-meta-chip code {
  font-family: var(--sl-font-mono, monospace);
  font-size: 11px;
  color: var(--sl-text-secondary, #8e8e93);
}
</style>
