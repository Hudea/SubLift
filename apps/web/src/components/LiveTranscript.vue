<template>
  <div class="sl-card sl-transcript-card">
    <!-- 头部栏 -->
    <div class="sl-card-header">
      <div class="sl-header-left">
        <span class="sl-card-title">字幕工作台 (SUBTITLE TRANSCRIPT)</span>
        <span class="sl-badge">{{ filteredEntries.length }} / {{ workbenchStore.entries.length }} 条</span>
      </div>

      <div class="sl-header-right">
        <!-- 搜索过滤条 -->
        <div v-if="workbenchStore.entries.length > 0" class="sl-search-box">
          <svg class="sl-search-icon" viewBox="0 0 16 16" fill="currentColor">
            <path d="M11.742 10.344a6.5 6.5 0 1 0-1.397 1.398h-.001c.03.04.062.078.098.115l3.85 3.85a1 1 0 0 0 1.415-1.414l-3.85-3.85a1.007 1.007 0 0 0-.115-.1zM12 6.5a5.5 5.5 0 1 1-11 0 5.5 5.5 0 0 1 11 0z"/>
          </svg>
          <input 
            v-model="searchQuery" 
            type="text" 
            placeholder="搜索字幕文本..."
            class="sl-search-input"
          />
          <button 
            v-if="searchQuery" 
            class="sl-search-clear"
            @click="searchQuery = ''"
          >
            &times;
          </button>
        </div>

        <!-- 顶部导出按钮 -->
        <button 
          v-if="workbenchStore.canExport"
          class="sl-btn-header-export"
          title="导出修改后的 SRT 字幕"
          @click="workbenchStore.downloadSrt()"
        >
          <svg class="sl-btn-icon" viewBox="0 0 16 16" fill="currentColor">
            <path d="M.5 9.9a.5.5 0 0 1 .5.5v2.5a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-2.5a.5.5 0 0 1 1 0v2.5a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2v-2.5a.5.5 0 0 1 .5-.5z"/>
            <path d="M7.646 11.854a.5.5 0 0 0 .708 0l3-3a.5.5 0 0 0-.708-.708L8.5 10.293V1.5a.5.5 0 0 0-1 0v8.793L5.354 8.146a.5.5 0 1 0-.708.708l3 3z"/>
          </svg>
          <span>导出 SRT</span>
        </button>
      </div>
    </div>

    <!-- 表头导航 -->
    <div v-if="workbenchStore.entries.length > 0" class="sl-table-header">
      <span class="col-idx">#</span>
      <span class="col-time">时间区间 (Start → End)</span>
      <span class="col-text">字幕文本内容 (双击行内校对)</span>
      <span class="col-conf">置信度</span>
      <span class="col-act">操作</span>
    </div>

    <!-- 滚动内容区 -->
    <div ref="scrollContainerRef" class="sl-transcript-body">
      <!-- 空状态 -->
      <div v-if="workbenchStore.entries.length === 0" class="sl-transcript-empty">
        <div class="sl-empty-icon-wrap">
          <svg class="sl-empty-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor">
            <rect x="3" y="4" width="18" height="16" rx="3" stroke-width="1.8"/>
            <path d="M7 9h10M7 13h6M7 17h8" stroke-width="1.8" stroke-linecap="round"/>
          </svg>
        </div>
        <p class="sl-empty-primary">
          {{ 
            workbenchStore.state === 'Empty' ? '暂无字幕数据 · 请在左侧载入视频' :
            workbenchStore.state === 'Ready' ? '视频就绪 · 点击左侧“开始提取”实时生成字幕' :
            workbenchStore.state === 'Processing' ? '正在逐帧识别提取中，实时字幕将在此处流式展现...' :
            '未识别到字幕文本'
          }}
        </p>
        <p class="sl-empty-secondary">支持实时双向音画联动、点击跳帧定位与双击原地校对</p>
      </div>

      <!-- 字幕条目大表格 -->
      <div v-else class="sl-transcript-list">
        <div
          v-for="(entry, idx) in filteredEntries"
          :key="entry.index"
          class="sl-transcript-row"
          :class="{ 'is-active': workbenchStore.activeEntryIndex === entry.index }"
          @click="handleRowClick(entry.start_ms)"
        >
          <!-- 序号 -->
          <span class="col-idx sl-row-num">{{ idx + 1 }}</span>

          <!-- 时间轴 -->
          <div class="col-time sl-time-cell">
            <span class="sl-time-start">{{ formatMs(entry.start_ms) }}</span>
            <span class="sl-time-arrow">→</span>
            <span class="sl-time-end">{{ formatMs(entry.end_ms) }}</span>
          </div>

          <!-- 字幕内容 (双击编辑) -->
          <div class="col-text sl-text-cell">
            <input
              v-if="editingIndex === entry.index"
              v-model="entry.text"
              type="text"
              class="sl-inline-input"
              autofocus
              @blur="editingIndex = null"
              @keydown.enter="editingIndex = null"
              @click.stop
            />
            <span 
              v-else 
              class="sl-row-text"
              title="双击直接编辑字幕文本"
              @dblclick.stop="editingIndex = entry.index"
            >
              {{ entry.text }}
            </span>
          </div>

          <!-- 置信度 -->
          <div class="col-conf sl-conf-cell">
            <span class="sl-conf-tag" :class="getConfClass(entry.confidence)">
              {{ (entry.confidence * 100).toFixed(0) }}%
            </span>
          </div>

          <!-- 操作 -->
          <div class="col-act sl-act-cell" @click.stop>
            <button 
              class="sl-row-btn" 
              title="点击跳转到该时码"
              @click="handleRowClick(entry.start_ms)"
            >
              <svg class="sl-icon-xs" viewBox="0 0 16 16" fill="currentColor">
                <path d="m11.596 8.697-6.363 3.692c-.54.313-1.233-.066-1.233-.697V4.308c0-.63.692-1.01 1.233-.696l6.363 3.692a.802.802 0 0 1 0 1.393z"/>
              </svg>
            </button>
            <button 
              class="sl-row-btn sl-row-btn--delete" 
              title="删除此行字幕"
              @click="workbenchStore.removeSubtitleEntry(entry.index)"
            >
              &times;
            </button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, nextTick } from 'vue';
import { useWorkbenchStore } from '../stores/workbench';

const emit = defineEmits<{
  (e: 'seek', timeSec: number): void;
}>();

const workbenchStore = useWorkbenchStore();
const searchQuery = ref('');
const editingIndex = ref<number | null>(null);
const scrollContainerRef = ref<HTMLDivElement | null>(null);

const filteredEntries = computed(() => {
  if (!searchQuery.value.trim()) {
    return workbenchStore.entries;
  }
  const q = searchQuery.value.toLowerCase().trim();
  return workbenchStore.entries.filter((e) => e.text.toLowerCase().includes(q));
});

function formatMs(ms: number): string {
  const totalSec = Math.floor(ms / 1000);
  const s = totalSec % 60;
  const m = Math.floor(totalSec / 60) % 60;
  const millis = Math.floor((ms % 1000) / 100);
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${pad(m)}:${pad(s)}.${millis}`;
}

function getConfClass(conf: number): string {
  if (conf >= 0.85) return 'is-high';
  if (conf >= 0.6) return 'is-med';
  return 'is-low';
}

function handleRowClick(startMs: number) {
  emit('seek', startMs / 1000);
}

// Auto-scroll when new entry arrives during processing
watch(
  () => workbenchStore.entries.length,
  () => {
    if (workbenchStore.state === 'Processing' && scrollContainerRef.value) {
      nextTick(() => {
        if (scrollContainerRef.value) {
          scrollContainerRef.value.scrollTop = scrollContainerRef.value.scrollHeight;
        }
      });
    }
  }
);
</script>

<style scoped>
.sl-transcript-card {
  display: flex;
  flex-direction: column;
  height: 100%;
  background: var(--sl-surface-card);
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-md);
  box-shadow: var(--sl-shadow-sm), var(--sl-inner-highlight);
  overflow: hidden;
}

.sl-card-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 10px 16px;
  border-bottom: 1px solid var(--sl-border-subtle);
  background: rgba(255, 255, 255, 0.02);
  gap: 12px;
  flex-shrink: 0;
}

.sl-header-left {
  display: flex;
  align-items: center;
  gap: 8px;
}

.sl-card-title {
  font-size: var(--sl-font-size-xs);
  font-weight: 600;
  letter-spacing: 0.04em;
  color: var(--sl-text-tertiary);
}

.sl-badge {
  font-family: var(--sl-font-family-mono);
  font-size: 11px;
  padding: 2px 7px;
  background: var(--sl-surface-elevated);
  border-radius: var(--sl-radius-pill);
  color: var(--sl-text-secondary);
}

.sl-header-right {
  display: flex;
  align-items: center;
  gap: 10px;
}

.sl-search-box {
  display: flex;
  align-items: center;
  background: var(--sl-surface-base);
  border: 1px solid var(--sl-border-subtle);
  border-radius: var(--sl-radius-sm);
  padding: 2px 8px;
  gap: 6px;
  width: 120px;
  transition: var(--sl-transition-snappy);
}

.sl-search-box:focus-within {
  border-color: var(--sl-color-accent);
  width: 150px;
}

.sl-search-icon {
  width: 12px;
  height: 12px;
  color: var(--sl-text-tertiary);
  flex-shrink: 0;
}

.sl-search-input {
  border: none;
  background: transparent;
  color: #ffffff;
  font-size: var(--sl-font-size-xs);
  width: 100%;
  outline: none;
}

.sl-search-clear {
  border: none;
  background: transparent;
  color: var(--sl-text-tertiary);
  font-size: 13px;
  cursor: pointer;
}

.sl-btn-header-export {
  display: flex;
  align-items: center;
  gap: 5px;
  height: 26px;
  padding: 0 10px;
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-sm);
  background: var(--sl-surface-elevated);
  color: var(--sl-color-success);
  font-size: var(--sl-font-size-xs);
  font-weight: 500;
  cursor: pointer;
  transition: var(--sl-transition-snappy);
}

.sl-btn-header-export:hover {
  background: var(--sl-surface-card-hover);
  border-color: rgba(48, 209, 88, 0.4);
}

.sl-btn-icon {
  width: 12px;
  height: 12px;
}

/* 表头栏 */
.sl-table-header {
  display: grid;
  grid-template-columns: 28px 125px 1fr 52px 36px;
  gap: 8px;
  padding: 6px 12px;
  background: var(--sl-surface-base);
  border-bottom: 1px solid var(--sl-border-subtle);
  font-size: 11px;
  font-weight: 600;
  color: var(--sl-text-tertiary);
  text-transform: uppercase;
  letter-spacing: 0.02em;
  flex-shrink: 0;
}

/* 滚动条目 */
.sl-transcript-body {
  flex: 1;
  padding: 6px 8px;
  overflow-y: auto;
  min-height: 0;
}

.sl-transcript-empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  height: 100%;
  min-height: 240px;
  text-align: center;
  padding: 32px;
}

.sl-empty-icon-wrap {
  width: 48px;
  height: 48px;
  border-radius: var(--sl-radius-md);
  background: var(--sl-surface-base);
  border: 1px solid var(--sl-border-subtle);
  display: flex;
  align-items: center;
  justify-content: center;
  margin-bottom: 14px;
  color: var(--sl-text-tertiary);
}

.sl-empty-icon {
  width: 24px;
  height: 24px;
}

.sl-empty-primary {
  font-size: var(--sl-font-size-sm);
  font-weight: 500;
  color: var(--sl-text-secondary);
  margin-bottom: 6px;
}

.sl-empty-secondary {
  font-size: var(--sl-font-size-xs);
  color: var(--sl-text-tertiary);
}

.sl-transcript-list {
  display: flex;
  flex-direction: column;
  gap: 3px;
}

.sl-transcript-row {
  display: grid;
  grid-template-columns: 28px 125px 1fr 52px 36px;
  gap: 8px;
  align-items: center;
  padding: 6px 10px;
  border-radius: var(--sl-radius-sm);
  background: var(--sl-surface-base);
  border: 1px solid transparent;
  cursor: pointer;
  transition: var(--sl-transition-snappy);
}

.sl-transcript-row:hover {
  background: var(--sl-surface-elevated);
  border-color: var(--sl-border-subtle);
}

.sl-transcript-row.is-active {
  background: var(--sl-color-accent-muted);
  border-color: var(--sl-border-accent);
}

.col-idx {
  text-align: center;
}

.sl-row-num {
  font-family: var(--sl-font-family-mono);
  font-size: 11px;
  color: var(--sl-text-tertiary);
}

.sl-time-cell {
  display: flex;
  align-items: center;
  gap: 4px;
  font-family: var(--sl-font-family-mono);
  font-size: 11px;
  font-variant-numeric: tabular-nums;
}

.sl-time-start {
  color: var(--sl-color-accent-hover);
  background: rgba(10, 132, 255, 0.1);
  padding: 1px 4px;
  border-radius: 3px;
}

.sl-time-arrow {
  color: var(--sl-text-tertiary);
  font-size: 10px;
}

.sl-time-end {
  color: var(--sl-text-tertiary);
}

.sl-text-cell {
  min-width: 0;
}

.sl-row-text {
  font-size: var(--sl-font-size-sm);
  color: var(--sl-text-primary);
  line-height: 1.4;
  word-break: break-word;
}

.sl-inline-input {
  width: 100%;
  background: var(--sl-surface-card);
  border: 1px solid var(--sl-color-accent);
  border-radius: var(--sl-radius-xs);
  padding: 3px 6px;
  color: #ffffff;
  font-size: var(--sl-font-size-sm);
  outline: none;
}

.sl-conf-cell {
  text-align: center;
}

.sl-conf-tag {
  font-family: var(--sl-font-family-mono);
  font-size: 10px;
  padding: 1px 5px;
  border-radius: var(--sl-radius-xs);
}

.sl-conf-tag.is-high {
  color: var(--sl-color-success);
  background: var(--sl-color-success-bg);
}

.sl-conf-tag.is-med {
  color: var(--sl-color-processing);
  background: var(--sl-color-processing-bg);
}

.sl-conf-tag.is-low {
  color: var(--sl-text-tertiary);
  background: rgba(255, 255, 255, 0.05);
}

.sl-act-cell {
  display: flex;
  align-items: center;
  gap: 4px;
  opacity: 0.3;
  transition: opacity 120ms ease;
}

.sl-transcript-row:hover .sl-act-cell {
  opacity: 1;
}

.sl-row-btn {
  border: none;
  background: transparent;
  color: var(--sl-text-secondary);
  cursor: pointer;
  padding: 2px 4px;
  border-radius: 3px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}

.sl-row-btn:hover {
  background: var(--sl-surface-card-hover);
  color: #ffffff;
}

.sl-row-btn--delete:hover {
  color: var(--sl-color-error);
}

.sl-icon-xs {
  width: 12px;
  height: 12px;
}
</style>
