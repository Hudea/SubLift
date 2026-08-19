<template>
  <div class="sl-stats-grid">
    <!-- 全部 -->
    <div 
      class="sl-stat-card"
      :class="{ 'is-active': batchStore.statusFilter === 'all' }"
      @click="batchStore.statusFilter = 'all'"
    >
      <div class="sl-stat-label">全部任务</div>
      <div class="sl-stat-value">{{ batchStore.stats.total }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <!-- 等待中 -->
    <div 
      class="sl-stat-card sl-stat-card--waiting"
      :class="{ 'is-active': batchStore.statusFilter === 'waiting' }"
      @click="batchStore.statusFilter = 'waiting'"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--waiting"></span>
        等待中
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.waiting }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <!-- 进行中 -->
    <div 
      class="sl-stat-card sl-stat-card--running"
      :class="{ 'is-active': batchStore.statusFilter === 'running' }"
      @click="batchStore.statusFilter = 'running'"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--running" :class="{ 'is-pulsing': batchStore.stats.active > 0 }"></span>
        进行中
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.active }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <!-- 已完成 -->
    <div 
      class="sl-stat-card sl-stat-card--completed"
      :class="{ 'is-active': batchStore.statusFilter === 'completed' }"
      @click="batchStore.statusFilter = 'completed'"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--completed"></span>
        已完成
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.completed }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <!-- 失败 -->
    <div 
      class="sl-stat-card sl-stat-card--failed"
      :class="{ 'is-active': batchStore.statusFilter === 'failed' }"
      @click="batchStore.statusFilter = 'failed'"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--failed"></span>
        失败
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.failed }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <!-- 已取消 -->
    <div 
      class="sl-stat-card sl-stat-card--cancelled"
      :class="{ 'is-active': batchStore.statusFilter === 'cancelled' }"
      @click="batchStore.statusFilter = 'cancelled'"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--cancelled"></span>
        已取消
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.cancelled }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <!-- 已跳过 -->
    <div 
      class="sl-stat-card sl-stat-card--skipped"
      :class="{ 'is-active': batchStore.statusFilter === 'skipped' }"
      @click="batchStore.statusFilter = 'skipped'"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--skipped"></span>
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
  grid-template-columns: repeat(7, 1fr);
  gap: 10px;
  width: 100%;
}

@media (max-width: 1200px) {
  .sl-stats-grid {
    grid-template-columns: repeat(4, 1fr);
  }
}

@media (max-width: 768px) {
  .sl-stats-grid {
    grid-template-columns: repeat(2, 1fr);
  }
}

.sl-stat-card {
  position: relative;
  background: var(--sl-surface-card, #1c1c1e);
  border: 1px solid var(--sl-border-subtle, rgba(255, 255, 255, 0.08));
  border-radius: var(--sl-radius-md, 8px);
  padding: 12px 14px;
  cursor: pointer;
  transition: all 0.2s cubic-bezier(0.16, 1, 0.3, 1);
  overflow: hidden;
  user-select: none;
}

.sl-stat-card:hover {
  background: var(--sl-surface-elevated, #242426);
  border-color: var(--sl-border-standard, rgba(255, 255, 255, 0.15));
  transform: translateY(-1px);
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
  gap: 6px;
  margin-bottom: 6px;
}

.sl-stat-value {
  font-size: var(--sl-font-size-xl, 22px);
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
