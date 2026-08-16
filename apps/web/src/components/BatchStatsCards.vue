<template>
  <div class="sl-stats-grid">
    <div 
      class="sl-stat-card"
      :class="{ 'is-active': activeFilter === 'all' }"
      @click="setFilter('all')"
    >
      <div class="sl-stat-label">全部任务</div>
      <div class="sl-stat-value">{{ batchStore.stats.total }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <div 
      class="sl-stat-card sl-stat-card--waiting"
      :class="{ 'is-active': activeFilter === 'waiting' }"
      @click="setFilter('waiting')"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--waiting"></span>
        排队中
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.waiting }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <div 
      class="sl-stat-card sl-stat-card--running"
      :class="{ 'is-active': activeFilter === 'running' }"
      @click="setFilter('running')"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--running"></span>
        处理中
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.running }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <div 
      class="sl-stat-card sl-stat-card--completed"
      :class="{ 'is-active': activeFilter === 'completed' }"
      @click="setFilter('completed')"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--completed"></span>
        已完成
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.completed }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>

    <div 
      class="sl-stat-card sl-stat-card--failed"
      :class="{ 'is-active': activeFilter === 'failed' }"
      @click="setFilter('failed')"
    >
      <div class="sl-stat-label">
        <span class="sl-dot sl-dot--failed"></span>
        失败 / 异常
      </div>
      <div class="sl-stat-value">{{ batchStore.stats.failed }}</div>
      <div class="sl-stat-bar-accent"></div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useBatchStore } from '../stores/batch';

defineProps<{
  activeFilter: string;
}>();

const emit = defineEmits<{
  (e: 'update:activeFilter', filter: string): void;
}>();

const batchStore = useBatchStore();

function setFilter(filter: string) {
  emit('update:activeFilter', filter);
}
</script>

<style scoped>
.sl-stats-grid {
  display: grid;
  grid-template-columns: repeat(5, 1fr);
  gap: 12px;
  width: 100%;
}

@media (max-width: 860px) {
  .sl-stats-grid {
    grid-template-columns: repeat(2, 1fr);
  }
}

.sl-stat-card {
  position: relative;
  background: var(--sl-surface-card);
  border: 1px solid var(--sl-border-subtle);
  border-radius: var(--sl-radius-md);
  padding: 14px 16px;
  cursor: pointer;
  transition: var(--sl-transition-smooth);
  box-shadow: var(--sl-shadow-sm), var(--sl-inner-highlight);
  overflow: hidden;
}

.sl-stat-card:hover {
  background: var(--sl-surface-card-hover);
  border-color: var(--sl-border-standard);
  transform: translateY(-1px);
}

.sl-stat-card.is-active {
  border-color: var(--sl-color-accent);
  background: var(--sl-surface-elevated);
}

.sl-stat-label {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: var(--sl-font-size-xs);
  color: var(--sl-text-tertiary);
  font-weight: 500;
  margin-bottom: 6px;
}

.sl-stat-value {
  font-size: 22px;
  font-family: var(--sl-font-family-mono);
  font-weight: 700;
  color: var(--sl-text-primary);
  letter-spacing: -0.03em;
}

.sl-stat-bar-accent {
  position: absolute;
  bottom: 0;
  left: 0;
  right: 0;
  height: 2px;
  background: transparent;
  transition: var(--sl-transition-snappy);
}

.sl-stat-card.is-active .sl-stat-bar-accent {
  background: var(--sl-color-accent);
}

.sl-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
}
.sl-dot--waiting { background: var(--sl-color-ready); }
.sl-dot--running {
  background: var(--sl-color-processing);
  box-shadow: 0 0 6px var(--sl-color-processing);
  animation: sl-pulse-fast 1.5s infinite ease-in-out;
}
.sl-dot--completed { background: var(--sl-color-success); }
.sl-dot--failed { background: var(--sl-color-error); }

@keyframes sl-pulse-fast {
  0%, 100% { opacity: 1; transform: scale(1); }
  50% { opacity: 0.4; transform: scale(1.3); }
}
</style>
