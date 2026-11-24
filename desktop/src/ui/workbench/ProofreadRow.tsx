import { memo, useEffect, useRef, useState } from 'react';
import {
  AlertTriangle,
  ArrowDownToLine,
  Scissors,
  Sparkles,
  Trash2,
} from 'lucide-react';
import {
  DEFAULT_LOW_CONF_THRESHOLD,
  findLiteralMatches,
  isLowConfidenceCue,
} from '../../core/subtitle';
import {
  formatSrtTimecode,
  parseSrtTimecode,
  validateTimeRange,
} from '../../core/timecode';
import type { ProofreadCue } from '../../core/types';
import { useImeGuard } from '../hooks/useImeGuard';
import { Checkbox } from '../components/ui/checkbox';
import { cn } from '../lib/utils';

function renderHighlightedText(
  text: string,
  query: string,
  caseSensitive: boolean
): React.ReactNode {
  if (!query) {
    return text;
  }
  const spans = findLiteralMatches(text, query, caseSensitive);
  if (spans.length === 0) {
    return text;
  }
  const parts: React.ReactNode[] = [];
  let cursor = 0;
  let matchIdx = 0;

  for (const span of spans) {
    if (span.startIndex > cursor) {
      parts.push(text.slice(cursor, span.startIndex));
    }
    parts.push(
      <mark
        key={`m_${matchIdx++}`}
        className="rounded-[2px] bg-[var(--tint-search-hit)] px-0.5 text-[var(--text-primary)]"
      >
        {text.slice(span.startIndex, span.startIndex + span.length)}
      </mark>
    );
    cursor = span.startIndex + span.length;
  }

  if (cursor < text.length) {
    parts.push(text.slice(cursor));
  }
  return parts.length > 0 ? parts : text;
}

export interface ProofreadRowProps {
  index: number;
  cue: ProofreadCue;
  lowConfThreshold?: number;
  readOnly: boolean;
  editingLockedByOther?: boolean;
  isActive: boolean;
  isSelected: boolean;
  isActiveSearchHit: boolean;
  searchQuery: string;
  caseSensitive: boolean;
  canMergeNext: boolean;
  canReRecognize: boolean;
  isReRecognizing: boolean;
  onSelectCue: (localUid: string) => void;
  onToggleSelect: (localUid: string) => void;
  onDirtyChange: (
    dirty: boolean,
    focused?: boolean,
    cueUid?: string | null
  ) => void;
  onCommitText: (localUid: string, text: string) => void;
  onCommitTimes: (
    localUid: string,
    startMs: number,
    endMs: number
  ) => { ok: boolean; reason?: string };
  onCommitEdit?: (
    localUid: string,
    patch: { text?: string; startMs?: number; endMs?: number }
  ) => { ok: boolean; reason?: string };
  onNudgeTime?: (
    localUid: string,
    field: 'start_ms' | 'end_ms',
    deltaMs: number
  ) => void;
  onMergeNext: (localUid: string) => void;
  onSplitCue: (
    localUid: string,
    cursorOffset: number,
    pendingText?: string
  ) => { ok: boolean; reason?: string; splitLocalUid?: string };
  onDeleteCue: (localUid: string) => void;
  onReRecognizeCue: (localUid: string) => void;
}

export const ProofreadRow = memo(function ProofreadRow({
  index,
  cue,
  lowConfThreshold = DEFAULT_LOW_CONF_THRESHOLD,
  readOnly,
  editingLockedByOther = false,
  isActive,
  isSelected,
  isActiveSearchHit,
  searchQuery,
  caseSensitive,
  canMergeNext,
  canReRecognize,
  isReRecognizing,
  onSelectCue,
  onToggleSelect,
  onDirtyChange,
  onCommitText,
  onCommitTimes,
  onCommitEdit,
  onMergeNext,
  onSplitCue,
  onDeleteCue,
  onReRecognizeCue,
}: ProofreadRowProps) {
  const rowRef = useRef<HTMLDivElement | null>(null);
  const textareaRef = useRef<HTMLTextAreaElement | null>(null);
  const editingRef = useRef(false);
  const cueUidRef = useRef(cue.local_uid);
  cueUidRef.current = cue.local_uid;
  const onDirtyChangeRef = useRef(onDirtyChange);
  onDirtyChangeRef.current = onDirtyChange;
  const ime = useImeGuard();

  const [editing, setEditing] = useState(false);
  const [draftText, setDraftText] = useState(cue.text);
  const [draftStartText, setDraftStartText] = useState(
    formatSrtTimecode(cue.start_ms)
  );
  const [draftEndText, setDraftEndText] = useState(
    formatSrtTimecode(cue.end_ms)
  );
  const [timeError, setTimeError] = useState<string | null>(null);
  const [splitError, setSplitError] = useState<string | null>(null);

  useEffect(() => {
    return () => {
      if (editingRef.current) {
        editingRef.current = false;
        onDirtyChangeRef.current(false, false, cueUidRef.current);
      }
    };
  }, []);

  useEffect(() => {
    if (!editing) {
      setDraftText(cue.text);
      setDraftStartText(formatSrtTimecode(cue.start_ms));
      setDraftEndText(formatSrtTimecode(cue.end_ms));
      setTimeError(null);
      setSplitError(null);
    }
  }, [cue.text, cue.start_ms, cue.end_ms, editing]);

  useEffect(() => {
    if (readOnly && editingRef.current) {
      editingRef.current = false;
      setEditing(false);
      setTimeError(null);
      setSplitError(null);
      onDirtyChangeRef.current(false, false, cueUidRef.current);
    }
  }, [readOnly]);

  const lowConf = isLowConfidenceCue(cue, lowConfThreshold);
  const confPct = Math.round(cue.confidence * 100);

  const validateDraftTimes = (startStr: string, endStr: string) => {
    const startMs = parseSrtTimecode(startStr);
    const endMs = parseSrtTimecode(endStr);
    if (startMs === null || endMs === null) {
      return {
        valid: false as const,
        reason: '时码格式应为 HH:MM:SS,mmm',
      };
    }
    const range = validateTimeRange(startMs, endMs);
    if (!range.valid) {
      return {
        valid: false as const,
        reason: range.reason ?? '起止时间非法',
      };
    }
    return { valid: true as const, startMs, endMs };
  };

  const nudgeDraftTime = (field: 'start' | 'end', deltaMs: number) => {
    const currentStr = field === 'start' ? draftStartText : draftEndText;
    const baseMs =
      parseSrtTimecode(currentStr) ??
      (field === 'start' ? cue.start_ms : cue.end_ms);
    const nextMs = Math.max(0, baseMs + deltaMs);
    const nextStr = formatSrtTimecode(nextMs);
    const nextStartStr = field === 'start' ? nextStr : draftStartText;
    const nextEndStr = field === 'end' ? nextStr : draftEndText;
    if (field === 'start') {
      setDraftStartText(nextStr);
    } else {
      setDraftEndText(nextStr);
    }
    onDirtyChange(true, true, cue.local_uid);
    const check = validateDraftTimes(nextStartStr, nextEndStr);
    setTimeError(check.valid ? null : check.reason);
  };

  const enterEdit = () => {
    if (readOnly || editingLockedByOther) {
      return;
    }
    setDraftText(cue.text);
    setDraftStartText(formatSrtTimecode(cue.start_ms));
    setDraftEndText(formatSrtTimecode(cue.end_ms));
    setTimeError(null);
    setSplitError(null);
    editingRef.current = true;
    setEditing(true);
    onDirtyChange(true, true, cue.local_uid);
    requestAnimationFrame(() => {
      textareaRef.current?.focus();
    });
  };

  const cancelEdit = () => {
    editingRef.current = false;
    setEditing(false);
    setTimeError(null);
    setSplitError(null);
    onDirtyChange(false, false, cue.local_uid);
    requestAnimationFrame(() => {
      rowRef.current?.focus();
    });
  };

  const commitEdit = () => {
    if (readOnly) {
      return;
    }
    const timeCheck = validateDraftTimes(draftStartText, draftEndText);
    if (!timeCheck.valid) {
      setTimeError(timeCheck.reason);
      return;
    }
    if (onCommitEdit) {
      const res = onCommitEdit(cue.local_uid, {
        text: draftText,
        startMs: timeCheck.startMs,
        endMs: timeCheck.endMs,
      });
      if (!res.ok) {
        setTimeError(res.reason ?? '起止时间非法');
        return;
      }
    } else {
      if (
        timeCheck.startMs !== cue.start_ms ||
        timeCheck.endMs !== cue.end_ms
      ) {
        const res = onCommitTimes(
          cue.local_uid,
          timeCheck.startMs,
          timeCheck.endMs
        );
        if (!res.ok) {
          setTimeError(res.reason ?? '起止时间非法');
          return;
        }
      }
      onCommitText(cue.local_uid, draftText);
    }
    editingRef.current = false;
    setEditing(false);
    setTimeError(null);
    setSplitError(null);
    onDirtyChange(false, false, cue.local_uid);
    requestAnimationFrame(() => {
      rowRef.current?.focus();
    });
  };

  const performSplit = (offset: number) => {
    const res = onSplitCue(cue.local_uid, offset, draftText);
    if (res.ok) {
      editingRef.current = false;
      setEditing(false);
      setTimeError(null);
      setSplitError(null);
      requestAnimationFrame(() => {
        const splitSelector = res.splitLocalUid
          ? `[data-testid="proofread-row-${CSS.escape(res.splitLocalUid)}"]`
          : null;
        const targetRow = splitSelector
          ? document.querySelector<HTMLElement>(splitSelector)
          : null;
        const container =
          rowRef.current?.closest<HTMLElement>(
            '[data-testid="proofreader-virtual-list"]'
          ) ?? null;
        (targetRow ?? rowRef.current ?? container)?.focus();
      });
    } else {
      setSplitError(res.reason ?? '无法拆分字幕');
    }
  };

  const handleTextareaKeyDown = (e: React.KeyboardEvent<HTMLTextAreaElement>) => {
    if (ime.isImeActive(e)) {
      return;
    }
    const mod = e.metaKey || e.ctrlKey;
    if (mod && (e.key === 'Enter' || e.key === '\\')) {
      e.preventDefault();
      e.stopPropagation();
      const offset = e.currentTarget.selectionStart ?? draftText.length;
      performSplit(offset);
      return;
    }
    if (e.key === 'Enter' && !e.shiftKey && !mod) {
      e.preventDefault();
      e.stopPropagation();
      commitEdit();
      return;
    }
    if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      cancelEdit();
    }
  };

  const handleTimeInputKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (ime.isImeActive(e)) {
      return;
    }
    if (e.key === 'Enter') {
      e.preventDefault();
      e.stopPropagation();
      commitEdit();
    } else if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      cancelEdit();
    }
  };

  return (
    <div
      ref={rowRef}
      tabIndex={0}
      data-testid={`proofread-row-${cue.local_uid}`}
      onClick={() => onSelectCue(cue.local_uid)}
      className={cn(
        'group relative border-b border-[var(--border-subtle)] px-3 py-2 text-xs transition-colors focus-visible:outline-none focus-visible:ring-1 focus-visible:ring-[var(--border-focus)]',
        isActive && 'border-l-2 border-l-[var(--sl-highlight)] bg-[var(--bg-active)]/50',
        isSelected && 'bg-[var(--tint-selected)]',
        lowConf && !isActive && 'border-l-2 border-l-[var(--status-warning)]',
        isActiveSearchHit && 'ring-1 ring-inset ring-[var(--status-warning)]'
      )}
    >
      {/* 第 1 行：复选框 + 序号 + 时码 + 置信度 + 行操作 */}
      <div className="flex items-center justify-between gap-2">
        <div
          className="flex items-center gap-2"
          onClick={(e) => e.stopPropagation()}
        >
          {!readOnly && (
            <Checkbox
              aria-label={`勾选第 ${index + 1} 条字幕`}
              checked={isSelected}
              onCheckedChange={() => onToggleSelect(cue.local_uid)}
            />
          )}
          <span className="tabular-nums-mono text-[11px] font-semibold text-[var(--text-secondary)]">
            {isActive && (
              <span className="mr-1 text-[var(--sl-highlight)]" aria-hidden="true">
                ▶
              </span>
            )}
            #{index + 1}
          </span>
          <span className="tabular-nums-mono text-[11px] text-[var(--text-secondary)]">
            {formatSrtTimecode(cue.start_ms)} → {formatSrtTimecode(cue.end_ms)}
          </span>
        </div>

        <div className="flex items-center gap-1.5">
          {lowConf ? (
            <span className="inline-flex items-center gap-0.5 rounded-[var(--radius-xs)] bg-[var(--tint-warning)] px-1.5 py-0.5 text-[10px] font-medium text-[var(--status-warning)]">
              <AlertTriangle className="h-2.5 w-2.5" />
              <span>{confPct}%</span>
            </span>
          ) : (
            <span className="tabular-nums-mono text-[10px] text-[var(--text-muted)]">
              {confPct}%
            </span>
          )}

          {!readOnly && !editing && (
            <div
              className="hidden items-center gap-0.5 group-hover:flex group-focus-within:flex"
              onClick={(e) => e.stopPropagation()}
            >
              {canMergeNext && (
                <button
                  type="button"
                  title="与下一条合并"
                  aria-label="与下一条合并"
                  onClick={() => onMergeNext(cue.local_uid)}
                  className="rounded p-1 text-[var(--text-secondary)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-primary)]"
                >
                  <ArrowDownToLine className="h-3 w-3" />
                </button>
              )}
              <button
                type="button"
                title="VLM 重识此条"
                aria-label="VLM 重识此条"
                disabled={!canReRecognize || isReRecognizing}
                onClick={() => onReRecognizeCue(cue.local_uid)}
                className="rounded p-1 text-[var(--text-secondary)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-primary)] disabled:opacity-40"
              >
                <Sparkles className="h-3 w-3" />
              </button>
              <button
                type="button"
                title="删除此条"
                aria-label="删除此条"
                onClick={() => onDeleteCue(cue.local_uid)}
                className="rounded p-1 text-[var(--text-secondary)] hover:bg-[var(--tint-failed)] hover:text-[var(--status-failed)]"
              >
                <Trash2 className="h-3 w-3" />
              </button>
            </div>
          )}
        </div>
      </div>

      {/* 第 2 行：文本展示或行内编辑缓冲区 */}
      {editing ? (
        <div
          className="mt-1.5 space-y-1.5"
          onClick={(e) => e.stopPropagation()}
          onKeyDown={(e) => {
            if (!ime.isImeActive(e) && e.key === 'Escape') {
              e.preventDefault();
              e.stopPropagation();
              cancelEdit();
            }
          }}
        >
          <div className="flex flex-wrap items-center gap-1.5 text-[11px]">
            <input
              aria-label="开始时间"
              type="text"
              value={draftStartText}
              onChange={(e) => {
                const v = e.target.value;
                setDraftStartText(v);
                onDirtyChange(true, true, cue.local_uid);
                const check = validateDraftTimes(v, draftEndText);
                setTimeError(check.valid ? null : check.reason);
              }}
              onKeyDown={handleTimeInputKeyDown}
              className={cn(
                'tabular-nums-mono h-6 w-28 rounded-[var(--radius-xs)] border bg-[var(--bg-app)] px-1.5 text-[11px]',
                timeError
                  ? 'border-[var(--status-failed)]'
                  : 'border-[var(--border-input)]'
              )}
            />
            <button
              type="button"
              aria-label="开始时间 -100ms"
              onClick={() => nudgeDraftTime('start', -100)}
              className="rounded border border-[var(--border-default)] px-1 py-0.5 text-[10px] hover:bg-[var(--bg-hover)]"
            >
              -100
            </button>
            <button
              type="button"
              aria-label="开始时间 +100ms"
              onClick={() => nudgeDraftTime('start', 100)}
              className="rounded border border-[var(--border-default)] px-1 py-0.5 text-[10px] hover:bg-[var(--bg-hover)]"
            >
              +100
            </button>

            <span className="text-[var(--text-muted)]">→</span>

            <input
              aria-label="结束时间"
              type="text"
              value={draftEndText}
              onChange={(e) => {
                const v = e.target.value;
                setDraftEndText(v);
                onDirtyChange(true, true, cue.local_uid);
                const check = validateDraftTimes(draftStartText, v);
                setTimeError(check.valid ? null : check.reason);
              }}
              onKeyDown={handleTimeInputKeyDown}
              className={cn(
                'tabular-nums-mono h-6 w-28 rounded-[var(--radius-xs)] border bg-[var(--bg-app)] px-1.5 text-[11px]',
                timeError
                  ? 'border-[var(--status-failed)]'
                  : 'border-[var(--border-input)]'
              )}
            />
            <button
              type="button"
              aria-label="结束时间 -100ms"
              onClick={() => nudgeDraftTime('end', -100)}
              className="rounded border border-[var(--border-default)] px-1 py-0.5 text-[10px] hover:bg-[var(--bg-hover)]"
            >
              -100
            </button>
            <button
              type="button"
              aria-label="结束时间 +100ms"
              onClick={() => nudgeDraftTime('end', 100)}
              className="rounded border border-[var(--border-default)] px-1 py-0.5 text-[10px] hover:bg-[var(--bg-hover)]"
            >
              +100
            </button>
          </div>

          {timeError && (
            <p className="text-[11px] text-[var(--status-failed)]">{timeError}</p>
          )}
          {splitError && (
            <p className="text-[11px] text-[var(--status-failed)]">{splitError}</p>
          )}

          <textarea
            ref={textareaRef}
            aria-label={`编辑第 ${index + 1} 条字幕文本`}
            rows={2}
            value={draftText}
            onCompositionStart={ime.onCompositionStart}
            onCompositionEnd={ime.onCompositionEnd}
            onChange={(e) => {
              setDraftText(e.target.value);
              setSplitError(null);
              onDirtyChange(true, true, cue.local_uid);
            }}
            onKeyDown={handleTextareaKeyDown}
            className="w-full resize-y rounded-[var(--radius-sm)] border border-[var(--border-focus)] bg-[var(--bg-app)] p-1.5 text-sm leading-6 text-[var(--text-primary)] focus:outline-none"
          />

          <div className="flex items-center justify-between text-[11px]">
            <button
              type="button"
              onClick={() => {
                const offset =
                  textareaRef.current?.selectionStart ?? draftText.length;
                performSplit(offset);
              }}
              className="inline-flex items-center gap-1 rounded-[var(--radius-xs)] border border-[var(--border-default)] px-1.5 py-0.5 text-[11px] text-[var(--text-secondary)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-primary)]"
            >
              <Scissors className="h-3 w-3" />
              <span>在此拆分 (⌘Enter)</span>
            </button>
            <div className="flex items-center gap-1.5">
              <button
                type="button"
                onClick={cancelEdit}
                className="rounded-[var(--radius-xs)] px-2 py-0.5 text-[var(--text-secondary)] hover:bg-[var(--bg-hover)]"
              >
                取消 (Esc)
              </button>
              <button
                type="button"
                disabled={Boolean(timeError)}
                onClick={commitEdit}
                className="rounded-[var(--radius-xs)] bg-[var(--primary)] px-2 py-0.5 text-[var(--primary-foreground)] hover:bg-[var(--primary-hover)] disabled:opacity-45"
              >
                提交 (Enter)
              </button>
            </div>
          </div>
        </div>
      ) : (
        <div
          onClick={(e) => {
            if (!readOnly && isActive) {
              e.stopPropagation();
              enterEdit();
            }
          }}
          onDoubleClick={(e) => {
            if (!readOnly) {
              e.stopPropagation();
              enterEdit();
            }
          }}
          className={cn(
            'mt-1 whitespace-pre-wrap break-words text-sm leading-6 text-[var(--text-primary)]',
            !readOnly && 'cursor-text'
          )}
        >
          {renderHighlightedText(cue.text, searchQuery, caseSensitive)}
        </div>
      )}
    </div>
  );
});
