<template>
  <div class="sl-card sl-transcript-card" role="region" aria-label="字幕工作台">
    <!-- 1. 顶部操作栏 -->
    <div class="sl-card-header" role="toolbar" aria-label="字幕工作区操作栏">
      <div class="sl-header-left">
        <span class="sl-card-title">字幕工作台 (SUBTITLE TRANSCRIPT)</span>
        <span class="sl-badge" :aria-label="`共 ${filteredEntries.length} 句字幕`">
          {{ filteredEntries.length }} / {{ workbenchStore.entries.length }} 句
        </span>
        <span 
          v-if="workbenchStore.state === 'Processing'" 
          class="sl-live-tag"
          role="status"
          aria-live="polite"
          aria-label="实时打轴中"
        >
          <span class="sl-live-dot" aria-hidden="true"></span> 实时打轴中
        </span>

        <!-- 草稿状态指示器 (Feature 12515) -->
        <span 
          v-if="workbenchStore.entries.length > 0 || workbenchStore.hasUserEdits" 
          class="sl-draft-status-badge"
          :class="`sl-draft-status--${workbenchStore.draftStatus}`"
          :title="draftTooltip"
          role="status"
          aria-live="polite"
          :aria-label="`草稿状态: ${draftStatusLabel}`"
        >
          <span class="sl-draft-dot" aria-hidden="true"></span>
          {{ draftStatusLabel }}
        </span>
      </div>

      <div class="sl-header-right">
        <!-- 撤销 / 重做 按钮 (Feature 12515) -->
        <div class="sl-history-group" role="group" aria-label="历史编辑">
          <button 
            type="button"
            class="sl-btn-header-history"
            :disabled="!workbenchStore.canUndo"
            title="撤销 (⌘Z / Ctrl+Z)"
            aria-label="撤销"
            @click="workbenchStore.undo()"
          >
            ↶ 撤销
          </button>
          <button 
            type="button"
            class="sl-btn-header-history"
            :disabled="!workbenchStore.canRedo"
            title="重做 (⌘⇧Z / Ctrl+Shift+Z / Ctrl+Y)"
            aria-label="重做"
            @click="workbenchStore.redo()"
          >
            ↷ 重做
          </button>
        </div>

        <!-- 搜索框 -->
        <div v-if="workbenchStore.entries.length > 0" class="sl-search-box">
          <svg class="sl-search-icon" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
            <path d="M11.742 10.344a6.5 6.5 0 1 0-1.397 1.398h-.001c.03.04.062.078.098.115l3.85 3.85a1 1 0 0 0 1.415-1.414l-3.85-3.85a1.007 1.007 0 0 0-.115-.1zM12 6.5a5.5 5.5 0 1 1-11 0 5.5 5.5 0 0 1 11 0z"/>
          </svg>
          <input 
            v-model="searchQuery" 
            type="text" 
            placeholder="搜索字幕内容..."
            class="sl-search-input"
            aria-label="搜索字幕内容"
          />
          <button 
            v-if="searchQuery" 
            type="button"
            class="sl-search-clear" 
            aria-label="清除搜索"
            @click="searchQuery = ''"
          >
            &times;
          </button>
        </div>

        <!-- 导出 SRT 按钮 -->
        <button 
          v-if="workbenchStore.canExport"
          class="sl-btn-header-export"
          title="导出标准 UTF-8 SRT 字幕文件"
          aria-label="导出标准 UTF-8 SRT 字幕文件"
          @click="workbenchStore.downloadSrt()"
        >
          <svg class="sl-btn-icon" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
            <path d="M.5 9.9a.5.5 0 0 1 .5.5v2.5a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-2.5a.5.5 0 0 1 1 0v2.5a2 2 0 0 1-2 2H2a2 2 0 0 1-2-2v-2.5a.5.5 0 0 1 .5-.5z"/>
            <path d="M7.646 11.854a.5.5 0 0 0 .708 0l3-3a.5.5 0 0 0-.708-.708L8.5 10.293V1.5a.5.5 0 0 0-1 0v8.793L5.354 8.146a.5.5 0 1 0-.708.708l3 3z"/>
          </svg>
          <span>导出 SRT</span>
        </button>
      </div>
    </div>

    <!-- 2. 表头导航 -->
    <div v-if="workbenchStore.entries.length > 0" class="sl-table-header" aria-hidden="true">
      <span class="col-idx">#</span>
      <span class="col-time">时间区间 (Start → End)</span>
      <span class="col-text">字幕文本内容 (双击就地校对)</span>
      <span class="col-conf">置信度</span>
      <span class="col-act">操作</span>
    </div>

    <!-- 3. 滚动内容区 -->
    <div 
      ref="scrollContainerRef" 
      class="sl-transcript-body"
      tabindex="0"
      aria-label="字幕条目滚动列表"
      @scroll="handleScroll"
      @wheel="handleUserWheel"
      @pointerdown="handleUserWheel"
    >
      <!-- 空状态 -->
      <div v-if="workbenchStore.entries.length === 0" class="sl-transcript-empty" role="status">
        <div class="sl-empty-icon-wrap" aria-hidden="true">
          <svg class="sl-empty-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor">
            <rect x="3" y="4" width="18" height="16" rx="3" stroke-width="1.8"/>
            <path d="M7 9h10M7 13h6M7 17h8" stroke-width="1.8" stroke-linecap="round"/>
          </svg>
        </div>
        <p class="sl-empty-primary">
          {{ 
            workbenchStore.state === 'Empty' ? '暂无字幕数据 · 请载入视频' :
            workbenchStore.state === 'Ready' ? '视频就绪 · 点击左侧“开始提取”实时生成字幕' :
            workbenchStore.state === 'Processing' ? '正在逐帧识别提取中，实时字幕将在此处流式展现...' :
            '未识别到字幕文本'
          }}
        </p>
        <p class="sl-empty-secondary">支持实时双向音画联动、点击跳帧定位、双击行内校对与快捷打轴</p>
      </div>

      <!-- 字幕行列表 -->
      <div v-else class="sl-transcript-list" role="list">
        <div
          v-for="(entry, idx) in filteredEntries"
          :key="entry.index"
          :ref="(el) => setRowRef(el as HTMLElement, entry.index)"
          class="sl-transcript-row"
          :class="{ 
            'is-active': workbenchStore.activeEntryIndex === entry.index,
            'has-warning': getEntryWarnings(entry.index) !== null 
          }"
          role="listitem"
          :aria-label="`字幕第 ${idx + 1} 句，时间 ${formatDisplayMs(entry.start_ms)} 至 ${formatDisplayMs(entry.end_ms)}，内容：${entry.text}`"
          @click="handleRowClick(entry.start_ms)"
        >
          <!-- 序号 & 冲突警示 -->
          <div class="col-idx sl-row-num-wrap">
            <span class="sl-row-num">{{ idx + 1 }}</span>
            <span 
              v-if="getEntryWarnings(entry.index)" 
              class="sl-timeline-warning-badge" 
              :title="getEntryWarnings(entry.index)!"
              role="alert"
              :aria-label="`时间码冲突警示: ${getEntryWarnings(entry.index)}`"
            >
              ⚠️
            </span>
          </div>

          <!-- 时间轴 (支持双击微调时码) -->
          <div class="col-time sl-time-cell" @dblclick.stop="startEditTime(entry)">
            <template v-if="editingTimeIndex === entry.index">
              <div class="sl-time-edit-wrapper">
                <input
                  v-model="draftTimeStr"
                  type="text"
                  class="sl-inline-input sl-inline-input--time"
                  :class="{ 'sl-inline-input--error': !!timecodeValidationError }"
                  placeholder="00:00.000 - 00:00.000"
                  aria-label="编辑字幕起止时间码"
                  autofocus
                  @click.stop
                  @blur="commitEditTime(entry)"
                  @keydown.enter="commitEditTime(entry)"
                  @keydown.esc="cancelEditTime"
                />
                <div v-if="timecodeValidationError" class="sl-time-error-tooltip" role="alert">
                  {{ timecodeValidationError }}
                </div>
              </div>
            </template>
            <template v-else>
              <span class="sl-time-start">{{ formatDisplayMs(entry.start_ms) }}</span>
              <span class="sl-time-arrow" aria-hidden="true">→</span>
              <span class="sl-time-end">{{ formatDisplayMs(entry.end_ms) }}</span>
            </template>
          </div>

          <!-- 字幕文本内容 (双击编辑) -->
          <div class="col-text sl-text-cell">
            <input
              v-if="editingTextIndex === entry.index"
              ref="textInputRef"
              v-model="draftText"
              type="text"
              class="sl-inline-input"
              aria-label="编辑字幕文本"
              @click.stop
              @blur="commitEditText(entry)"
              @keydown.enter="commitEditText(entry)"
              @keydown.esc="cancelEditText"
            />
            <span 
              v-else 
              class="sl-row-text"
              title="双击直接编辑文本，回车保存"
              @dblclick.stop="startEditText(entry)"
            >
              {{ entry.text }}
            </span>
          </div>

          <!-- 置信度 -->
          <div class="col-conf sl-conf-cell">
            <span 
              class="sl-conf-tag" 
              :class="getConfClass(entry.confidence)"
              :aria-label="`置信度: ${(entry.confidence * 100).toFixed(0)}%`"
            >
              {{ (entry.confidence * 100).toFixed(0) }}%
            </span>
          </div>

          <!-- 打轴操作工具栏 -->
          <div class="col-act sl-act-cell" @click.stop>
            <button 
              type="button"
              class="sl-row-btn" 
              title="定位跳转" 
              aria-label="定位跳转播放"
              @click="handleRowClick(entry.start_ms)"
            >
              <svg class="sl-icon-xs" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
                <path d="m11.596 8.697-6.363 3.692c-.54.313-1.233-.066-1.233-.697V4.308c0-.63.692-1.01 1.233-.696l6.363 3.692a.802.802 0 0 1 0 1.393z"/>
              </svg>
            </button>
            <button 
              type="button"
              class="sl-row-btn" 
              title="向后插入新句" 
              aria-label="向后插入新句"
              :disabled="workbenchStore.isLocked"
              @click="workbenchStore.insertEntryAfter(entry.index)"
            >
              +
            </button>
            <button 
              v-if="!isFiltered"
              type="button"
              class="sl-row-btn" 
              title="与下一句合并" 
              aria-label="与下一句合并"
              :disabled="workbenchStore.isLocked"
              @click="workbenchStore.mergeWithNext(entry.index)"
            >
              ↓
            </button>
            <button 
              type="button"
              class="sl-row-btn sl-row-btn--delete" 
              title="删除此行"
              aria-label="删除此行"
              :disabled="workbenchStore.isLocked"
              @click="workbenchStore.removeSubtitleEntry(entry.index)"
            >
              &times;
            </button>
          </div>
        </div>
      </div>

      <!-- 4. 脱离底部吸附时的返回悬浮胶囊 -->
      <transition name="sl-fade">
        <button 
          v-if="!autoStickToBottom && workbenchStore.state === 'Processing'" 
          type="button"
          class="sl-floating-bottom-btn"
          aria-label="回到底部实时滚动"
          @click="scrollToBottomManual"
        >
          <svg class="sl-icon-xs" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
            <path d="M8 1a.5.5 0 0 1 .5.5v11.793l3.146-3.147a.5.5 0 0 1 .708.708l-4 4a.5.5 0 0 1-.708 0l-4-4a.5.5 0 0 1 .708-.708L7.5 13.293V1.5A.5.5 0 0 1 8 1z"/>
          </svg>
          <span>实时滚动已暂停 · 点击回到底部</span>
        </button>
      </transition>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, nextTick, onMounted, onUnmounted } from 'vue';
import { useWorkbenchStore } from '../stores/workbench';
import { formatDisplayMs, validateTimecodeString, detectTimelineConflicts } from '../utils/timecode';
import type { SubtitleEntry } from '../types/api';

const emit = defineEmits<{
  (e: 'seek', timeSec: number): void;
}>();

const workbenchStore = useWorkbenchStore();
const searchQuery = ref('');
const scrollContainerRef = ref<HTMLDivElement | null>(null);
const rowRefs = new Map<number, HTMLElement>();

// 编辑状态管理
const editingTextIndex = ref<number | null>(null);
const draftText = ref('');
const textInputRef = ref<HTMLInputElement | null>(null);

const editingTimeIndex = ref<number | null>(null);
const draftTimeStr = ref('');
const timecodeValidationError = ref<string | null>(null);

// 底部吸附与智能平滑滚动状态
const autoStickToBottom = ref(true);
let userInteractingUntil = 0;

const isFiltered = computed(() => !!searchQuery.value.trim());

const filteredEntries = computed(() => {
  if (!isFiltered.value) {
    return workbenchStore.entries;
  }
  const q = searchQuery.value.toLowerCase().trim();
  return workbenchStore.entries.filter((e) => e.text.toLowerCase().includes(q));
});

// Timeline Conflict Detection (Feature 12515)
const timelineConflicts = computed(() => detectTimelineConflicts(workbenchStore.entries));

function getEntryWarnings(entryIndex: number): string | null {
  const list = timelineConflicts.value.get(entryIndex);
  if (!list || list.length === 0) return null;
  return list.map((w) => w.message).join('\n');
}

// Draft Status Label & Tooltip
const draftStatusLabel = computed(() => {
  switch (workbenchStore.draftStatus) {
    case 'dirty':
      return '未保存更改';
    case 'saving':
      return '保存中…';
    case 'saved':
      return '草稿已存';
    case 'failed':
      return '保存失败';
    default:
      return '草稿就绪';
  }
});

const draftTooltip = computed(() => {
  if (workbenchStore.draftStatus === 'failed') {
    return `保存失败: ${workbenchStore.draftError || '未知错误'}`;
  }
  if (workbenchStore.draftSavedAt) {
    const d = new Date(workbenchStore.draftSavedAt);
    return `最近自动保存: ${d.toLocaleTimeString()}`;
  }
  return '审阅草稿自动保存中';
});

function setRowRef(el: HTMLElement | null, index: number) {
  if (el) {
    rowRefs.set(index, el);
  } else {
    rowRefs.delete(index);
  }
}

function getConfClass(conf: number): string {
  if (conf >= 0.85) return 'is-high';
  if (conf >= 0.6) return 'is-med';
  return 'is-low';
}

function handleRowClick(startMs: number) {
  emit('seek', startMs / 1000);
}

// 文本编辑
function startEditText(entry: SubtitleEntry) {
  if (workbenchStore.isLocked) return;
  editingTimeIndex.value = null;
  timecodeValidationError.value = null;
  editingTextIndex.value = entry.index;
  draftText.value = entry.text;
  nextTick(() => {
    textInputRef.value?.focus();
    textInputRef.value?.select();
  });
}

function commitEditText(entry: SubtitleEntry) {
  if (editingTextIndex.value === entry.index) {
    const trimmed = draftText.value.trim();
    if (trimmed !== entry.text) {
      workbenchStore.updateSubtitleEntry(entry.index, { text: trimmed });
    }
    editingTextIndex.value = null;
  }
}

function cancelEditText() {
  editingTextIndex.value = null;
}

// 时码编辑 (Pre-Commit Timecode Validation)
function startEditTime(entry: SubtitleEntry) {
  if (workbenchStore.isLocked) return;
  editingTextIndex.value = null;
  timecodeValidationError.value = null;
  editingTimeIndex.value = entry.index;
  draftTimeStr.value = `${formatDisplayMs(entry.start_ms)} - ${formatDisplayMs(entry.end_ms)}`;
}

function commitEditTime(entry: SubtitleEntry) {
  if (editingTimeIndex.value === entry.index) {
    const validation = validateTimecodeString(draftTimeStr.value);
    if (!validation.valid) {
      timecodeValidationError.value = validation.error || '时间码格式错误';
      return;
    }

    if (validation.start_ms !== undefined && validation.end_ms !== undefined) {
      workbenchStore.updateSubtitleEntry(entry.index, {
        start_ms: validation.start_ms,
        end_ms: validation.end_ms,
      });
    }
    timecodeValidationError.value = null;
    editingTimeIndex.value = null;
  }
}

function cancelEditTime() {
  editingTimeIndex.value = null;
  timecodeValidationError.value = null;
}

// 键盘快捷键监听
function handleGlobalKeydown(e: KeyboardEvent) {
  const isMac = typeof navigator !== 'undefined' && navigator.platform.toUpperCase().indexOf('MAC') >= 0;
  const modKey = isMac ? e.metaKey : e.ctrlKey;

  if (!modKey) return;

  const key = e.key.toLowerCase();

  // Undo: modKey + z (without shift)
  if (key === 'z' && !e.shiftKey) {
    if (document.activeElement?.tagName === 'INPUT' && editingTextIndex.value !== null) {
      return;
    }
    if (workbenchStore.canUndo) {
      e.preventDefault();
      workbenchStore.undo();
    }
  }

  // Redo: modKey + shift + z OR ctrl + y
  if ((key === 'z' && e.shiftKey) || (e.ctrlKey && key === 'y')) {
    if (document.activeElement?.tagName === 'INPUT' && editingTextIndex.value !== null) {
      return;
    }
    if (workbenchStore.canRedo) {
      e.preventDefault();
      workbenchStore.redo();
    }
  }
}

onMounted(() => {
  window.addEventListener('keydown', handleGlobalKeydown);
});

onUnmounted(() => {
  window.removeEventListener('keydown', handleGlobalKeydown);
});

function handleUserWheel() {
  userInteractingUntil = performance.now() + 2500;
}

function handleScroll() {
  if (!scrollContainerRef.value) return;
  const { scrollTop, scrollHeight, clientHeight } = scrollContainerRef.value;
  const dist = scrollHeight - scrollTop - clientHeight;
  autoStickToBottom.value = dist <= 36;
}

function scrollToBottomManual() {
  autoStickToBottom.value = true;
  if (scrollContainerRef.value) {
    const isReduced = typeof window !== 'undefined' && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    scrollContainerRef.value.scrollTo({
      top: scrollContainerRef.value.scrollHeight,
      behavior: isReduced ? 'auto' : 'smooth',
    });
  }
}

// 监听新条目入表 (流式打轴吸附)
watch(
  () => workbenchStore.entries.length,
  () => {
    if (workbenchStore.state === 'Processing' && autoStickToBottom.value && scrollContainerRef.value) {
      nextTick(() => {
        if (scrollContainerRef.value) {
          scrollContainerRef.value.scrollTop = scrollContainerRef.value.scrollHeight;
        }
      });
    }
  }
);

// 监听 10Hz 时钟更新下的 activeEntryIndex -> 智能平滑滚动
watch(
  () => workbenchStore.activeEntryIndex,
  (newIdx) => {
    if (newIdx === null || performance.now() < userInteractingUntil) return;
    const targetEl = rowRefs.get(newIdx);
    const container = scrollContainerRef.value;
    if (targetEl && container) {
      const containerRect = container.getBoundingClientRect();
      const elRect = targetEl.getBoundingClientRect();

      const isOutside = elRect.top < containerRect.top + 30 || elRect.bottom > containerRect.bottom - 30;
      if (isOutside) {
        const isReduced = typeof window !== 'undefined' && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
        targetEl.scrollIntoView({ block: 'nearest', behavior: isReduced ? 'auto' : 'smooth' });
      }
    }
  }
);
</script>

<style scoped>
.sl-transcript-card {
  position: relative;
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
  padding: 8px 12px;
  border-bottom: 1px solid var(--sl-border-subtle);
  background: rgba(255, 255, 255, 0.02);
  gap: 8px;
  flex-shrink: 0;
  flex-wrap: wrap;
}

.sl-header-left {
  display: flex;
  align-items: center;
  gap: 6px;
  flex-wrap: wrap;
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
  padding: 2px 6px;
  background: var(--sl-surface-elevated);
  border-radius: var(--sl-radius-pill);
  color: var(--sl-text-secondary);
}

.sl-live-tag {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  font-size: 11px;
  font-weight: 600;
  color: var(--sl-color-processing);
  background: var(--sl-color-processing-bg);
  padding: 2px 7px;
  border-radius: var(--sl-radius-pill);
}

.sl-live-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: var(--sl-color-processing);
  animation: sl-pulse 1.2s infinite ease-in-out;
}

@keyframes sl-pulse {
  0%, 100% { transform: scale(0.9); opacity: 0.5; }
  50% { transform: scale(1.3); opacity: 1; }
}

/* 草稿状态指示器 (Feature 12515) */
.sl-draft-status-badge {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  font-size: 11px;
  font-weight: 500;
  padding: 2px 6px;
  border-radius: var(--sl-radius-pill);
  background: var(--sl-surface-elevated);
  color: var(--sl-text-secondary);
  border: 1px solid var(--sl-border-subtle);
  transition: all 0.2s ease;
}

.sl-draft-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: currentColor;
}

.sl-draft-status--saved {
  color: var(--sl-color-success, #30d158);
  background: rgba(48, 209, 88, 0.1);
  border-color: rgba(48, 209, 88, 0.25);
}

.sl-draft-status--dirty {
  color: #ffd60a;
  background: rgba(255, 214, 10, 0.12);
  border-color: rgba(255, 214, 10, 0.3);
}

.sl-draft-status--saving {
  color: var(--sl-color-processing, #0a84ff);
  background: rgba(10, 132, 255, 0.12);
  border-color: rgba(10, 132, 255, 0.3);
}

.sl-draft-status--failed {
  color: var(--sl-color-error, #ff453a);
  background: rgba(255, 69, 58, 0.15);
  border-color: rgba(255, 69, 58, 0.35);
}

/* 历史撤销重做按钮组 */
.sl-history-group {
  display: flex;
  align-items: center;
  gap: 4px;
}

.sl-btn-header-history {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  height: 26px;
  padding: 0 7px;
  background: var(--sl-surface-base);
  border: 1px solid var(--sl-border-standard);
  border-radius: var(--sl-radius-xs);
  color: var(--sl-text-secondary);
  font-size: 11px;
  font-weight: 500;
  cursor: pointer;
  transition: var(--sl-transition-snappy);
}

.sl-btn-header-history:hover:not(:disabled) {
  background: var(--sl-surface-elevated);
  color: #ffffff;
  border-color: var(--sl-color-accent);
}

.sl-btn-header-history:disabled {
  opacity: 0.35;
  cursor: not-allowed;
}

.sl-header-right {
  display: flex;
  align-items: center;
  gap: 8px;
}

.sl-search-box {
  display: flex;
  align-items: center;
  background: var(--sl-surface-base);
  border: 1px solid var(--sl-border-subtle);
  border-radius: var(--sl-radius-sm);
  padding: 2px 6px;
  gap: 4px;
  width: 100px;
  transition: var(--sl-transition-snappy);
}

.sl-search-box:focus-within {
  border-color: var(--sl-color-accent);
  width: 140px;
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
  border: 1px solid rgba(48, 209, 88, 0.4);
  border-radius: var(--sl-radius-sm);
  background: var(--sl-color-success-bg);
  color: var(--sl-color-success);
  font-size: var(--sl-font-size-xs);
  font-weight: 600;
  cursor: pointer;
  transition: var(--sl-transition-snappy);
  white-space: nowrap;
}

.sl-btn-header-export:hover {
  background: rgba(48, 209, 88, 0.22);
  border-color: var(--sl-color-success);
}

.sl-btn-icon {
  width: 12px;
  height: 12px;
}

.sl-table-header {
  display: grid;
  grid-template-columns: 24px 105px minmax(100px, 1fr) 45px 78px;
  gap: 6px;
  padding: 6px 10px;
  background: var(--sl-surface-base);
  border-bottom: 1px solid var(--sl-border-subtle);
  font-size: 11px;
  font-weight: 600;
  color: var(--sl-text-tertiary);
  text-transform: uppercase;
  letter-spacing: 0.02em;
  flex-shrink: 0;
}

.sl-transcript-body {
  position: relative;
  flex: 1;
  padding: 6px 8px;
  overflow-y: auto;
  min-height: 0;
  outline: none;
}

.sl-transcript-empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  height: 100%;
  min-height: 200px;
  text-align: center;
  padding: 24px;
}

.sl-empty-icon-wrap {
  width: 44px;
  height: 44px;
  border-radius: var(--sl-radius-md);
  background: var(--sl-surface-base);
  border: 1px solid var(--sl-border-subtle);
  display: flex;
  align-items: center;
  justify-content: center;
  margin-bottom: 12px;
  color: var(--sl-text-tertiary);
}

.sl-empty-icon {
  width: 22px;
  height: 22px;
}

.sl-empty-primary {
  font-size: var(--sl-font-size-sm);
  font-weight: 500;
  color: var(--sl-text-secondary);
  margin-bottom: 4px;
}

.sl-empty-secondary {
  font-size: var(--sl-font-size-xs);
  color: var(--sl-text-tertiary);
}

.sl-transcript-list {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.sl-transcript-row {
  position: relative;
  display: grid;
  grid-template-columns: 24px 105px minmax(100px, 1fr) 45px 78px;
  gap: 6px;
  align-items: center;
  padding: 5px 8px;
  border-radius: var(--sl-radius-sm);
  background: var(--sl-surface-base);
  border: 1px solid transparent;
  cursor: pointer;
  transition: background 120ms ease, border-color 120ms ease;
}

.sl-transcript-row:hover {
  background: var(--sl-surface-elevated);
  border-color: var(--sl-border-subtle);
}

/* 呼吸高亮态 */
.sl-transcript-row.is-active {
  background: rgba(10, 132, 255, 0.16);
  border-color: var(--sl-color-accent);
  box-shadow: 0 0 12px rgba(10, 132, 255, 0.22);
}

.sl-transcript-row.is-active::before {
  content: '';
  position: absolute;
  left: 0;
  top: 4px;
  bottom: 4px;
  width: 3px;
  background: var(--sl-color-accent);
  border-radius: 0 2px 2px 0;
}

.sl-transcript-row.has-warning {
  border-color: rgba(255, 159, 10, 0.25);
}

.col-idx {
  text-align: center;
}

.sl-row-num-wrap {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 2px;
}

.sl-row-num {
  font-family: var(--sl-font-family-mono);
  font-size: 11px;
  color: var(--sl-text-tertiary);
}

.sl-timeline-warning-badge {
  font-size: 10px;
  cursor: help;
  filter: drop-shadow(0 0 2px rgba(255, 159, 10, 0.5));
}

.sl-time-cell {
  display: flex;
  align-items: center;
  gap: 3px;
  font-family: var(--sl-font-family-mono);
  font-size: 10.5px;
  font-variant-numeric: tabular-nums;
  user-select: text;
  -webkit-user-select: text;
}

.sl-time-edit-wrapper {
  position: relative;
  display: flex;
  flex-direction: column;
  width: 100%;
}

.sl-time-start {
  color: var(--sl-color-accent-hover);
  background: rgba(10, 132, 255, 0.1);
  padding: 1px 3px;
  border-radius: 3px;
}

.sl-time-arrow {
  color: var(--sl-text-tertiary);
  font-size: 9px;
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
  user-select: text;
  -webkit-user-select: text;
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

.sl-inline-input--time {
  font-family: var(--sl-font-family-mono);
  font-size: 10px;
  padding: 2px 4px;
}

.sl-inline-input--error {
  border-color: var(--sl-color-error, #ff453a) !important;
  background: rgba(255, 69, 58, 0.1) !important;
}

.sl-time-error-tooltip {
  position: absolute;
  top: 100%;
  left: 0;
  z-index: 20;
  background: #2c0b0e;
  border: 1px solid #ff453a;
  color: #ff9999;
  font-size: 10px;
  font-family: var(--sl-font-family-mono);
  padding: 3px 6px;
  border-radius: 4px;
  margin-top: 2px;
  white-space: nowrap;
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.5);
  pointer-events: none;
}

.sl-conf-cell {
  text-align: center;
}

.sl-conf-tag {
  font-family: var(--sl-font-family-mono);
  font-size: 10px;
  padding: 1px 4px;
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
  gap: 2px;
  opacity: 0.5;
  transition: opacity 120ms ease;
}

.sl-transcript-row:hover .sl-act-cell,
.sl-transcript-row:focus-within .sl-act-cell {
  opacity: 1;
}

.sl-row-btn {
  border: none;
  background: var(--sl-surface-card);
  color: var(--sl-text-secondary);
  cursor: pointer;
  padding: 2px 4px;
  font-size: 11px;
  border-radius: 3px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  transition: var(--sl-transition-snappy);
}

.sl-row-btn:hover:not(:disabled) {
  background: var(--sl-surface-card-hover);
  color: #ffffff;
}

.sl-row-btn:disabled {
  opacity: 0.35;
  cursor: not-allowed;
}

.sl-row-btn--delete:hover:not(:disabled) {
  background: rgba(255, 69, 58, 0.2);
  color: var(--sl-color-error);
}

.sl-icon-xs {
  width: 11px;
  height: 11px;
}

.sl-floating-bottom-btn {
  position: sticky;
  bottom: 12px;
  left: 50%;
  transform: translateX(-50%);
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 5px 12px;
  background: var(--sl-color-accent);
  color: #ffffff;
  border: none;
  border-radius: var(--sl-radius-pill);
  font-size: 11px;
  font-weight: 600;
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.4);
  cursor: pointer;
  z-index: 10;
}

.sl-fade-enter-active,
.sl-fade-leave-active {
  transition: opacity 200ms ease, transform 200ms ease;
}

.sl-fade-enter-from,
.sl-fade-leave-to {
  opacity: 0;
  transform: translate(-50%, 8px);
}
</style>
