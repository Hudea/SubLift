<template>
  <div class="sl-stats-grid" role="tablist" aria-label="任务状态分类筛选">
    <!-- 全部 -->
    <div 
      class="sl-stat-card"
      :class="{ 'is-active': batchStore.statusFilter === 'all' }"
      role="tab"
      :aria-selected="batchStore.statusFilter === 'all'"
      tabindex="0"
      :aria-label="`全部任务: ${batchStore.stats.total} 项`"
      @click="batchStore.statusFilter = 'all'"
      @keydown.enter="batchStore.statusFilter = 'all'"
      @keydown.space.prevent="batchStore.statusFilter = 'all'"
    >
      <div class="sl-stat-label">全部任务</div>
      <div class="sl-stat-value">{{ batchStore.stats.total }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <!-- 等待中 -->
    <div 
      class="sl-stat-card sl-stat-card--waiting"
      :class="{ 'is-active': batchStore.statusFilter === 'waiting' }"
      role="tab"
      :aria-selected="batchStore.statusFilter === 'waiting'"
      tabindex="0"
      :aria-label="`等待中任务: ${batchStore.stats.waiting} 项`"
      @click="batchStore.statusFilter = 'waiting'"
      @keydown.enter="batchStore.statusFilter = 'waiting'"
      @keydown.space.prevent="batchStore.statusFilter = 'waiting'"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--waiting" aria-hidden="true"></span>
        等待中
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.waiting }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <!-- 进行中 -->
    <div 
      class="sl-stat-card sl-stat-card--running"
      :class="{ 'is-active': batchStore.statusFilter === 'running' }"
      role="tab"
      :aria-selected="batchStore.statusFilter === 'running'"
      tabindex="0"
      :aria-label="`进行中任务: ${batchStore.stats.active} 项`"
      @click="batchStore.statusFilter = 'running'"
      @keydown.enter="batchStore.statusFilter = 'running'"
      @keydown.space.prevent="batchStore.statusFilter = 'running'"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--running" :class="{ 'is-pulsing': batchStore.stats.active > 0 }" aria-hidden="true"></span>
        进行中
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.active }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <!-- 已完成 -->
    <div 
      class="sl-stat-card sl-stat-card--completed"
      :class="{ 'is-active': batchStore.statusFilter === 'completed' }"
      role="tab"
      :aria-selected="batchStore.statusFilter === 'completed'"
      tabindex="0"
      :aria-label="`已完成任务: ${batchStore.stats.completed} 项`"
      @click="batchStore.statusFilter = 'completed'"
      @keydown.enter="batchStore.statusFilter = 'completed'"
      @keydown.space.prevent="batchStore.statusFilter = 'completed'"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--completed" aria-hidden="true"></span>
        已完成
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.completed }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <!-- 失败 -->
    <div 
      class="sl-stat-card sl-stat-card--failed"
      :class="{ 'is-active': batchStore.statusFilter === 'failed' }"
      role="tab"
      :aria-selected="batchStore.statusFilter === 'failed'"
      tabindex="0"
      :aria-label="`失败任务: ${batchStore.stats.failed} 项`"
      @click="batchStore.statusFilter = 'failed'"
      @keydown.enter="batchStore.statusFilter = 'failed'"
      @keydown.space.prevent="batchStore.statusFilter = 'failed'"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--failed" aria-hidden="true"></span>
        失败
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.failed }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <!-- 已取消 -->
    <div 
      class="sl-stat-card sl-stat-card--cancelled"
      :class="{ 'is-active': batchStore.statusFilter === 'cancelled' }"
      role="tab"
      :aria-selected="batchStore.statusFilter === 'cancelled'"
      tabindex="0"
      :aria-label="`已取消任务: ${batchStore.stats.cancelled} 项`"
      @click="batchStore.statusFilter = 'cancelled'"
      @keydown.enter="batchStore.statusFilter = 'cancelled'"
      @keydown.space.prevent="batchStore.statusFilter = 'cancelled'"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--cancelled" aria-hidden="true"></span>
        已取消
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.cancelled }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <!-- 已跳过 -->
    <div 
      class="sl-stat-card sl-stat-card--skipped"
      :class="{ 'is-active': batchStore.statusFilter === 'skipped' }"
      role="tab"
      :aria-selected="batchStore.statusFilter === 'skipped'"
      tabindex="0"
      :aria-label="`已跳过任务: ${batchStore.stats.skipped} 项`"
      @click="batchStore.statusFilter = 'skipped'"
      @keydown.enter="batchStore.statusFilter = 'skipped'"
      @keydown.space.prevent="batchStore.statusFilter = 'skipped'"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--skipped" aria-hidden="true"></span>
        已跳过
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.skipped }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useBatchStore } from '../stores/batch';

const batchStore = useBatchStore();
</script>

<style scoped>
.sl-stats-grid {
  display: grid;
  grid-template-columns: repeat(7, minmax(0, 1fr));
  gap: 8px;
  width: 100%;
}

@media (max-width: 1100px) {
  .sl-stats-grid {
    grid-template-columns: repeat(4, minmax(0, 1fr));
  }
}

@media (max-width: 768px) {
  .sl-stats-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}

.sl-stat-card {
  position: relative;
  background: var(--sl-surface-card, #1c1c1e);
  border: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.08));
  border-radius: var(--sl-radius-md, 8px);
  padding: 10px 12px;
  cursor: pointer;
  transition: all 0.15s ease;
  overflow: hidden;
  user-select: none;
  outline: none;
}

.sl-stat-card:hover {
  background: var(--sl-surface-elevated, #242426);
  border-color: var(--sl-border-standard, rgba(255, 255, 255, 0.15));
  transform: translateY(-1px);
}

.sl-stat-card:focus-visible {
  outline: 2px solid var(--sl-color-accent, #0a84ff);
  outline-offset: 2px;
}

.sl-stat-card.is-active {
  background: var(--sl-surface-elevated, #2c2c2e);
  border-color: var(--sl-color-primary, #0a84ff);
  box-shadow: 0 0 0 1px var(--sl-color-primary, #0a84ff), 0 4px 12px rgba(0, 0, 0, 0.25);
}

.sl-stat-label {
  font-size: var(--sl-font-size-xs, 12px);
  font-weight: 500;
  color: var(--sl-text-secondary, #8e8e93);
  display: flex;
  align-items: center;
  gap: 5px;
  margin-bottom: 4px;
}

.sl-stat-value {
  font-size: var(--sl-font-size-lg, 18px);
  font-weight: 700;
  color: var(--sl-text-primary, #ffffff);
  line-height: 1.1;
  font-variant-numeric: tabular-nums;
}

.sl-stat-bar-accent {
  position: absolute;
  bottom: 0;
  left: 0;
  right: 0;
  height: 2px;
  background: transparent;
  transition: background 0.2s ease;
}

.sl-stat-card.is-active .sl-stat-bar-accent {
  background: var(--sl-color-primary, #0a84ff);
}

/* Dots */
.sl-dot {
  display: inline-block;
  width: 6px;
  height: 6px;
  border-radius: 50%;
}

.sl-dot--waiting { background: #8e8e93; }
.sl-dot--running { background: #ff9f0a; }
.sl-dot--completed { background: #30d158; }
.sl-dot--failed { background: #ff453a; }
.sl-dot--cancelled { background: #636366; }
.sl-dot--skipped { background: #bf5af2; }

.sl-dot.is-pulsing {
  animation: pulse-dot 1.5s infinite ease-in-out;
}

@keyframes pulse-dot {
  0%, 100% { opacity: 1; transform: scale(1); }
  50% { opacity: 0.4; transform: scale(1.3); }
}
</style>
