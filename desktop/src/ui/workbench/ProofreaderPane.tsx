import { useEffect, useMemo, useRef, useState } from 'react';
import { useVirtualizer } from '@tanstack/react-virtual';
import {
  AlertTriangle,
  ArrowDown,
  CheckCircle2,
  ChevronDown,
  ChevronUp,
  Download,
  Loader2,
  Redo2,
  Search,
  Sparkles,
  Undo2,
  X,
} from 'lucide-react';
import {
  countLowConfidenceCues,
  findCueMatches,
  findQueueMatches,
  isLowConfidenceCue,
  resolveLowConfidenceThreshold,
} from '../../core/subtitle';
import type { ProofreadCue } from '../../core/types';
import {
  selectActiveCommands,
  selectActiveTask,
  useWorkbenchStore,
} from '../../store';
import { formatExtractionSubPhaseLabel } from '../components/ui/badge';
import { Button } from '../components/ui/button';
import { Checkbox } from '../components/ui/checkbox';
import { cn } from '../lib/utils';
import { ProofreadRow } from './ProofreadRow';

interface ProofreaderPaneProps {
  className?: string;
}

export function ProofreaderPane({ className }: ProofreaderPaneProps) {
  const tasks = useWorkbenchStore((s) => s.tasks);
  const activeTask = useWorkbenchStore(selectActiveTask);
  const commands = useWorkbenchStore(selectActiveCommands);
  const findReplace = useWorkbenchStore((s) => s.findReplace);
  const editingCueUid = useWorkbenchStore((s) => s.editingCueUid);
  const isDirtyInlineEdit = useWorkbenchStore((s) => s.isDirtyInlineEdit);
  const lastExporting = useWorkbenchStore((s) => s.lastExporting);

  const selectCue = useWorkbenchStore((s) => s.selectCue);
  const toggleCueSelection = useWorkbenchStore((s) => s.toggleCueSelection);
  const setDirtyInlineEdit = useWorkbenchStore((s) => s.setDirtyInlineEdit);
  const commitCueText = useWorkbenchStore((s) => s.commitCueText);
  const commitCueTimes = useWorkbenchStore((s) => s.commitCueTimes);
  const commitCueEdit = useWorkbenchStore((s) => s.commitCueEdit);
  const nudgeCueTime = useWorkbenchStore((s) => s.nudgeCueTime);
  const mergeCueWithNext = useWorkbenchStore((s) => s.mergeCueWithNext);
  const splitCue = useWorkbenchStore((s) => s.splitCue);
  const deleteCue = useWorkbenchStore((s) => s.deleteCue);
  const undo = useWorkbenchStore((s) => s.undo);
  const redo = useWorkbenchStore((s) => s.redo);

  const openFind = useWorkbenchStore((s) => s.openFind);
  const closeFind = useWorkbenchStore((s) => s.closeFind);
  const setFindState = useWorkbenchStore((s) => s.setFindState);
  const navigateFindMatch = useWorkbenchStore((s) => s.navigateFindMatch);
  const replaceCurrentMatch = useWorkbenchStore((s) => s.replaceCurrentMatch);
  const replaceAllMatches = useWorkbenchStore((s) => s.replaceAllMatches);

  const reRecognizeCues = useWorkbenchStore((s) => s.reRecognizeCues);
  const reRecognizeAllLowConf = useWorkbenchStore(
    (s) => s.reRecognizeAllLowConf
  );
  const exportActiveSrt = useWorkbenchStore((s) => s.exportActiveSrt);

  const [onlyLowConf, setOnlyLowConf] = useState(false);
  const [autoFollow, setAutoFollow] = useState(true);
  const scrollParentRef = useRef<HTMLDivElement | null>(null);
  const findInputRef = useRef<HTMLInputElement | null>(null);

  const isExtracting = activeTask?.status === 'extracting';
  const isReRecognizing = Boolean(activeTask?.isReRecognizing);
  const lowConfThreshold = resolveLowConfidenceThreshold(
    activeTask?.activeRunConfig?.engine ?? activeTask?.engine
  );

  const baseCues: ProofreadCue[] = useMemo(() => {
    if (!activeTask) {
      return [];
    }
    if (isExtracting) {
      return activeTask.streamingPreviewCues.map((e, i) => ({
        ...e,
        local_uid: `stream_${e.id}_${i}`,
      }));
    }
    return activeTask.cues;
  }, [activeTask, isExtracting]);

  const lowConfCount = useMemo(
    () => countLowConfidenceCues(baseCues, lowConfThreshold),
    [baseCues, lowConfThreshold]
  );

  // 解决最后一条存疑条目或切换至 0 条存疑的剧集时自动复位，避免被困在空过滤列表
  useEffect(() => {
    if (onlyLowConf && lowConfCount === 0) {
      setOnlyLowConf(false);
    }
  }, [onlyLowConf, lowConfCount]);

  const displayCues = useMemo(() => {
    if (!onlyLowConf) {
      return baseCues;
    }
    return baseCues.filter((c) => isLowConfidenceCue(c, lowConfThreshold));
  }, [baseCues, onlyLowConf, lowConfThreshold]);

  const matches = useMemo(() => {
    if (!findReplace.isOpen || !findReplace.query) {
      return [];
    }
    if (findReplace.scope === 'queue') {
      const readyItems = tasks
        .filter((t) => t.status === 'ready' || t.status === 'exported')
        .map((t) => ({ taskId: t.id, cues: t.cues }));
      return findQueueMatches(
        readyItems,
        findReplace.query,
        findReplace.caseSensitive
      );
    }
    if (!activeTask) {
      return [];
    }
    return findCueMatches(
      activeTask.id,
      baseCues,
      findReplace.query,
      findReplace.caseSensitive
    );
  }, [
    findReplace.isOpen,
    findReplace.query,
    findReplace.scope,
    findReplace.caseSensitive,
    tasks,
    activeTask,
    baseCues,
  ]);

  const activeMatch =
    matches.length > 0
      ? matches[Math.min(findReplace.activeMatchIndex, matches.length - 1)]
      : null;

  const activeMatchReRecognizing = activeMatch
    ? Boolean(tasks.find((t) => t.id === activeMatch.taskId)?.isReRecognizing)
    : false;
  const hasReplaceableQueueMatch =
    findReplace.scope === 'queue'
      ? matches.some(
          (m) => !tasks.find((t) => t.id === m.taskId)?.isReRecognizing
        )
      : !isReRecognizing;

  const rowVirtualizer = useVirtualizer({
    count: displayCues.length,
    getScrollElement: () => scrollParentRef.current,
    estimateSize: () => 56,
    overscan: 10,
  });

  useEffect(() => {
    if (findReplace.isOpen) {
      requestAnimationFrame(() => {
        findInputRef.current?.focus();
        findInputRef.current?.select();
      });
    }
  }, [findReplace.isOpen, findReplace.focusRequestId]);

  // 提取流式推送自动跟滚（用户手动上滑即暂停，出现「回到最新」）
  useEffect(() => {
    if (isExtracting && autoFollow && displayCues.length > 0) {
      rowVirtualizer.scrollToIndex(displayCues.length - 1, { align: 'end' });
    }
  }, [isExtracting, autoFollow, displayCues.length, rowVirtualizer]);

  const handleScroll = () => {
    const el = scrollParentRef.current;
    if (!el || !isExtracting) {
      return;
    }
    const distanceToBottom = el.scrollHeight - el.scrollTop - el.clientHeight;
    if (distanceToBottom > 48 && autoFollow) {
      setAutoFollow(false);
    } else if (distanceToBottom <= 16 && !autoFollow) {
      setAutoFollow(true);
    }
  };

  const selectedCount = activeTask?.selectedCueIds.length ?? 0;
  const activeEngine = activeTask?.activeRunConfig?.engine ?? activeTask?.engine ?? null;
  const activeProgress = activeTask?.progress ?? null;
  const effectiveSampleFps =
    (activeTask?.activeRunConfig?.sampleFps ?? activeTask?.sampleFps ?? 5) > 0
      ? (activeTask?.activeRunConfig?.sampleFps ?? activeTask?.sampleFps ?? 5)
      : 5;
  const isVlmSheetPhase =
    activeTask?.subPhase === 'vlm_sheets' &&
    (activeProgress?.totalSheets ?? 0) > 0;
  const extractPct =
    isVlmSheetPhase && activeProgress
      ? Math.min(
          100,
          Math.round(
            (activeProgress.completedSheets / activeProgress.totalSheets) * 100
          )
        )
      : activeProgress && activeProgress.totalMs > 0
        ? Math.min(
            100,
            Math.round((activeProgress.currentMs / activeProgress.totalMs) * 100)
          )
        : 0;
  const extractFps = activeProgress?.fps ?? 0;
  const extractMult =
    extractFps > 0 ? (extractFps / effectiveSampleFps).toFixed(1) : null;
  const metrics = activeTask?.metrics ?? null;
  const metricsSpeedStr =
    metrics && metrics.effective_fps > 0
      ? ` · 平均 ${Math.round(metrics.effective_fps)} fps (${(
          metrics.effective_fps / effectiveSampleFps
        ).toFixed(1)}×)`
      : '';

  return (
    <aside
      aria-label="字幕校对器"
      className={cn(
        'flex h-full flex-col border-l border-[var(--border-subtle)] bg-[var(--bg-panel)]',
        className
      )}
    >
      {/* 顶部标题与工具栏 */}
      <div className="flex items-center justify-between gap-2 border-b border-[var(--border-subtle)] px-3 py-2">
        <div className="flex items-center gap-2">
          <span className="text-[11px] font-semibold tracking-wider text-[var(--text-secondary)]">
            字幕校对 ({baseCues.length})
          </span>
          {lowConfCount > 0 && (
            <button
              type="button"
              aria-pressed={onlyLowConf}
              onClick={() => setOnlyLowConf((v) => !v)}
              className={cn(
                'inline-flex items-center gap-1 rounded-[var(--radius-sm)] px-1.5 py-0.5 text-[10px] font-medium transition-colors cursor-pointer',
                onlyLowConf
                  ? 'bg-[var(--status-warning)] text-black'
                  : 'bg-[var(--tint-warning)] text-[var(--status-warning)] hover:bg-[var(--status-warning)]/25'
              )}
            >
              <AlertTriangle className="h-2.5 w-2.5" />
              <span>仅看存疑 ({lowConfCount})</span>
            </button>
          )}
        </div>

        <div className="flex items-center gap-1">
          <Button
            size="icon"
            variant="ghost"
            aria-label="撤销"
            title={commands.undo.reason ?? '撤销 (⌘Z)'}
            disabled={!commands.undo.enabled}
            onClick={() => undo()}
          >
            <Undo2 className="h-3.5 w-3.5" strokeWidth={1.75} />
          </Button>
          <Button
            size="icon"
            variant="ghost"
            aria-label="重做"
            title={commands.redo.reason ?? '重做 (⇧⌘Z)'}
            disabled={!commands.redo.enabled}
            onClick={() => redo()}
          >
            <Redo2 className="h-3.5 w-3.5" strokeWidth={1.75} />
          </Button>
          <Button
            size="icon"
            variant="ghost"
            aria-label="查找替换"
            title="查找与替换 (⌘F)"
            onClick={() =>
              findReplace.isOpen ? closeFind() : openFind(false)
            }
          >
            <Search className="h-3.5 w-3.5" strokeWidth={1.75} />
          </Button>
        </div>
      </div>

      {/* 主界面常驻提取进度与完成统计 */}
      {isExtracting && activeTask && (
        <div
          data-testid="active-extraction-progress"
          className="space-y-1.5 border-b border-[var(--border-subtle)] bg-[var(--bg-elevated)] px-3 py-2 text-[11px]"
        >
          <div className="flex items-center justify-between gap-2">
            <span className="inline-flex items-center gap-1.5 font-medium text-[var(--text-primary)]">
              <Loader2
                className="h-3 w-3 shrink-0 animate-spin text-[var(--status-extracting)]"
                strokeWidth={1.75}
              />
              <span>
                {formatExtractionSubPhaseLabel(
                  activeTask.subPhase,
                  activeTask.progress,
                  {
                    engine: activeEngine,
                    isAutoDetecting: activeTask.isAutoDetecting,
                  }
                )}
              </span>
            </span>
            <span className="tabular-nums-mono truncate text-[10px] text-[var(--text-secondary)]">
              {extractPct}%
              {extractFps > 0 &&
                ` · ${Math.round(extractFps)} fps${extractMult ? ` · ${extractMult}×` : ''}`}
              {` · 已出 ${baseCues.length} 条`}
              {activeProgress && activeProgress.totalSheets > 0
                ? ` · 图表 ${activeProgress.completedSheets}/${activeProgress.totalSheets}${
                    activeProgress.retriedSplits > 0
                      ? ` (重试 ${activeProgress.retriedSplits})`
                      : ''
                  }`
                : ''}
            </span>
          </div>
          <div className="h-1 w-full overflow-hidden rounded-full bg-[var(--border-default)]">
            <div
              className="h-full bg-[var(--status-extracting)] transition-all"
              style={{ width: `${extractPct}%` }}
            />
          </div>
        </div>
      )}

      {!isExtracting &&
        activeTask &&
        (activeTask.status === 'ready' || activeTask.status === 'exported') &&
        metrics && (
          <div
            data-testid="extraction-metrics-summary"
            className="flex items-center gap-1.5 border-b border-[var(--border-subtle)] bg-[var(--bg-elevated)]/60 px-3 py-1.5 text-[11px] text-[var(--text-secondary)]"
          >
            <CheckCircle2
              className="h-3 w-3 shrink-0 text-[var(--status-exported)]"
              strokeWidth={1.75}
            />
            <span className="tabular-nums-mono truncate">
              {`提取完成 · 耗时 ${(metrics.wall_time_ms / 1000).toFixed(1)}s${metricsSpeedStr} · 有效字幕 ${activeTask.cues.length} 条`}
            </span>
          </div>
        )}

      {/* 查找与替换条 */}
      {findReplace.isOpen && (
        <div className="space-y-1.5 border-b border-[var(--border-subtle)] bg-[var(--bg-elevated)] px-3 py-2 text-xs">
          <div className="flex items-center gap-1.5">
            <input
              ref={findInputRef}
              aria-label="查找关键词"
              type="text"
              value={findReplace.query}
              onChange={(e) => setFindState({ query: e.target.value })}
              placeholder="查找字幕文本…"
              className="h-6 min-w-0 flex-1 rounded-[var(--radius-sm)] border border-[var(--border-input)] bg-[var(--bg-panel)] px-2 text-xs text-[var(--text-primary)] focus:border-[var(--border-focus)] focus:outline-none"
            />
            <span className="tabular-nums-mono text-[10px] text-[var(--text-secondary)]">
              {matches.length > 0
                ? `${Math.min(findReplace.activeMatchIndex + 1, matches.length)}/${matches.length}`
                : '0/0'}
            </span>
            <Button
              size="icon"
              variant="ghost"
              aria-label="上一个匹配"
              disabled={matches.length === 0}
              onClick={() => navigateFindMatch('prev')}
            >
              <ChevronUp className="h-3.5 w-3.5" />
            </Button>
            <Button
              size="icon"
              variant="ghost"
              aria-label="下一个匹配"
              disabled={matches.length === 0}
              onClick={() => navigateFindMatch('next')}
            >
              <ChevronDown className="h-3.5 w-3.5" />
            </Button>
            <Button
              size="icon"
              variant="ghost"
              aria-label="关闭查找"
              onClick={closeFind}
            >
              <X className="h-3.5 w-3.5" />
            </Button>
          </div>

          {findReplace.showReplace && (
            <div className="flex items-center gap-1.5">
              <input
                aria-label="替换为"
                type="text"
                value={findReplace.replacement}
                onChange={(e) => setFindState({ replacement: e.target.value })}
                placeholder="替换为…"
                className="h-6 min-w-0 flex-1 rounded-[var(--radius-sm)] border border-[var(--border-input)] bg-[var(--bg-panel)] px-2 text-xs text-[var(--text-primary)] focus:border-[var(--border-focus)] focus:outline-none"
              />
              <Button
                size="xs"
                variant="outline"
                disabled={
                  matches.length === 0 ||
                  isExtracting ||
                  isDirtyInlineEdit ||
                  isReRecognizing ||
                  activeMatchReRecognizing
                }
                onClick={replaceCurrentMatch}
              >
                替换
              </Button>
              <Button
                size="xs"
                variant="outline"
                disabled={
                  matches.length === 0 ||
                  isExtracting ||
                  isDirtyInlineEdit ||
                  isReRecognizing ||
                  !hasReplaceableQueueMatch
                }
                onClick={() => replaceAllMatches()}
              >
                全部替换
              </Button>
            </div>
          )}

          <div className="flex items-center justify-between text-[11px] text-[var(--text-secondary)]">
            <div className="flex items-center gap-3">
              <label className="flex cursor-pointer items-center gap-1">
                <Checkbox
                  checked={findReplace.caseSensitive}
                  onCheckedChange={(c) =>
                    setFindState({ caseSensitive: c === true })
                  }
                />
                <span>区分大小写</span>
              </label>

              <button
                type="button"
                onClick={() =>
                  setFindState({ showReplace: !findReplace.showReplace })
                }
                className="underline-offset-2 hover:underline"
              >
                {findReplace.showReplace ? '隐藏替换' : '展开替换'}
              </button>
            </div>

            <div className="flex items-center gap-1">
              <button
                type="button"
                onClick={() => setFindState({ scope: 'current' })}
                className={cn(
                  'rounded px-1.5 py-0.5 text-[10px]',
                  findReplace.scope === 'current'
                    ? 'bg-[var(--bg-active)] text-[var(--text-primary)]'
                    : 'text-[var(--text-muted)]'
                )}
              >
                当前视频
              </button>
              <button
                type="button"
                onClick={() => setFindState({ scope: 'queue' })}
                className={cn(
                  'rounded px-1.5 py-0.5 text-[10px]',
                  findReplace.scope === 'queue'
                    ? 'bg-[var(--bg-active)] text-[var(--text-primary)]'
                    : 'text-[var(--text-muted)]'
                )}
              >
                全部已就绪队列
              </button>
            </div>
          </div>
        </div>
      )}

      {/* 虚拟滚动字幕列表 */}
      <div
        ref={scrollParentRef}
        tabIndex={-1}
        onScroll={handleScroll}
        data-testid="proofreader-virtual-list"
        className="relative flex-1 overflow-y-auto focus:outline-none"
      >
        {!activeTask ? (
          <div className="flex h-full items-center justify-center p-4 text-center text-xs text-[var(--text-muted)]">
            加载视频并提取后，字幕条目将在此呈现
          </div>
        ) : displayCues.length === 0 ? (
          <div className="flex h-full items-center justify-center p-4 text-center text-xs text-[var(--text-muted)]">
            {isExtracting
              ? '正在流式识别字幕，请稍候…'
              : onlyLowConf
                ? '没有符合条件的存疑条目'
                : '暂无字幕条目'}
          </div>
        ) : (
          <div
            style={{
              height: `${rowVirtualizer.getTotalSize()}px`,
              width: '100%',
              position: 'relative',
            }}
          >
            {rowVirtualizer.getVirtualItems().map((virtualRow) => {
              const cue = displayCues[virtualRow.index];
              if (!cue) {
                return null;
              }
              const isHit =
                activeMatch !== null &&
                activeMatch.taskId === activeTask.id &&
                activeMatch.localUid === cue.local_uid;
              return (
                <div
                  key={cue.local_uid}
                  data-index={virtualRow.index}
                  ref={rowVirtualizer.measureElement}
                  style={{
                    position: 'absolute',
                    top: 0,
                    left: 0,
                    width: '100%',
                    transform: `translateY(${virtualRow.start}px)`,
                  }}
                >
                  <ProofreadRow
                    index={virtualRow.index}
                    cue={cue}
                    lowConfThreshold={lowConfThreshold}
                    readOnly={isExtracting || activeTask.isReRecognizing}
                    editingLockedByOther={
                      editingCueUid !== null && editingCueUid !== cue.local_uid
                    }
                    isActive={activeTask.activeCueId === cue.local_uid}
                    isSelected={activeTask.selectedCueIds.includes(cue.local_uid)}
                    isActiveSearchHit={isHit}
                    searchQuery={findReplace.isOpen ? findReplace.query : ''}
                    caseSensitive={findReplace.caseSensitive}
                    canMergeNext={!onlyLowConf && virtualRow.index + 1 < displayCues.length}
                    canReRecognize={commands.reRecognizeLowConf.enabled}
                    isReRecognizing={activeTask.isReRecognizing}
                    onSelectCue={(uid) => selectCue(uid, { seek: true })}
                    onToggleSelect={toggleCueSelection}
                    onDirtyChange={setDirtyInlineEdit}
                    onCommitText={commitCueText}
                    onCommitTimes={commitCueTimes}
                    onCommitEdit={commitCueEdit}
                    onNudgeTime={nudgeCueTime}
                    onMergeNext={mergeCueWithNext}
                    onSplitCue={splitCue}
                    onDeleteCue={deleteCue}
                    onReRecognizeCue={(uid) => void reRecognizeCues([uid])}
                  />
                </div>
              );
            })}
          </div>
        )}

        {/* 提取期间“回到最新”按钮 */}
        {isExtracting && !autoFollow && (
          <div className="sticky bottom-3 flex justify-center">
            <Button
              size="xs"
              variant="primary"
              onClick={() => {
                setAutoFollow(true);
                if (displayCues.length > 0) {
                  rowVirtualizer.scrollToIndex(displayCues.length - 1, {
                    align: 'end',
                  });
                }
              }}
            >
              <ArrowDown className="h-3 w-3" />
              <span>回到最新</span>
            </Button>
          </div>
        )}
      </div>

      {/* 底部批量工具与导出栏 */}
      <div className="flex flex-wrap items-center justify-between gap-2 border-t border-[var(--border-subtle)] bg-[var(--bg-panel)] px-3 py-2">
        <div className="flex items-center gap-1.5">
          {selectedCount > 0 && (
            <Button
              size="xs"
              variant="outline"
              disabled={!commands.reRecognizeLowConf.enabled}
              title={commands.reRecognizeLowConf.reason ?? '使用 VLM 重识所选字幕'}
              onClick={() =>
                activeTask && void reRecognizeCues(activeTask.selectedCueIds)
              }
            >
              <Sparkles className="h-3 w-3" />
              <span>VLM 重识选中 ({selectedCount})</span>
            </Button>
          )}

          <Button
            size="xs"
            variant="outline"
            disabled={!commands.reRecognizeLowConf.enabled || lowConfCount === 0}
            title={
              commands.reRecognizeLowConf.reason ??
              '使用 VLM 批量复核全部低置信度/待补录条目'
            }
            onClick={() => void reRecognizeAllLowConf()}
          >
            <Sparkles className="h-3 w-3 text-[var(--status-warning)]" />
            <span>
              {activeTask?.isReRecognizing
                ? 'VLM 重识中…'
                : `复核全部低置信度 (${lowConfCount})`}
            </span>
          </Button>
        </div>

        <Button
          size="xs"
          variant="primary"
          disabled={!commands.exportActiveSrt.enabled || lastExporting}
          title={commands.exportActiveSrt.reason ?? '导出规范 SRT (⌘S)'}
          onClick={() => void exportActiveSrt()}
        >
          <Download className="h-3 w-3" />
          <span>导出 SRT</span>
        </Button>
      </div>
    </aside>
  );
}
