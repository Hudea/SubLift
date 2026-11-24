import {
  canRedo,
  canUndo,
  pushHistory,
  redoHistory,
  undoHistory,
} from '../core/history';
import {
  adjustCueTime,
  deleteCueById,
  filterLowConfidenceCues,
  findCueMatches,
  findQueueMatches,
  mergeAdjacentCues,
  replaceAllAcrossQueue,
  replaceAllInCues,
  replaceSingleMatch,
  resolveLowConfidenceThreshold,
  splitCueAtCursor,
  updateCueText,
  updateCueTimes,
} from '../core/subtitle';
import { computeCueMidpointMs } from '../core/timecode';
import { parseIpcError } from './bridge';
import { resolveDefaultProviderId } from './providerSlice';
import type { StoreGet, StoreSet } from './providerSlice';
import type {
  FindReplaceState,
  ProofreadCue,
  QueueTaskItem,
  ReRecognizeItemInput,
  ReRecognizeResult,
} from './types';

export interface ProofreadSliceHelpers {
  scheduleDraftSave: (taskId: string) => void;
  stopPlaybackIfPlaying: (taskId: string) => void;
}

function isEditableStatus(item: QueueTaskItem): boolean {
  return item.status === 'ready' || item.status === 'exported';
}

function applyCueMutation(
  item: QueueTaskItem,
  nextCues: ProofreadCue[],
  extra?: Partial<QueueTaskItem>
): QueueTaskItem {
  return {
    ...item,
    cues: nextCues,
    history: pushHistory(item.history, nextCues),
    status: item.status === 'exported' ? 'ready' : item.status,
    ...extra,
  };
}

export function createProofreadActions(
  set: StoreSet,
  get: StoreGet,
  helpers: ProofreadSliceHelpers
) {
  const reRecGenMap = new Map<string, number>();

  const updateTask = (
    taskId: string,
    updater: (item: QueueTaskItem) => QueueTaskItem
  ) => {
    set((state) => ({
      tasks: state.tasks.map((t) => (t.id === taskId ? updater(t) : t)),
    }));
  };

  const getActiveReadyTask = (): QueueTaskItem | null => {
    const state = get();
    const active = state.tasks.find((t) => t.id === state.activeTaskId);
    if (!active) {
      return null;
    }
    return active;
  };

  const computeCurrentMatches = (
    state = get(),
    overrideFind?: Partial<FindReplaceState>
  ) => {
    const fr = { ...state.findReplace, ...overrideFind };
    if (!fr.query) {
      return [];
    }
    if (fr.scope === 'queue') {
      const readyItems = state.tasks
        .filter((t) => isEditableStatus(t))
        .map((t) => ({ taskId: t.id, cues: t.cues }));
      return findQueueMatches(readyItems, fr.query, fr.caseSensitive);
    }
    const active = state.tasks.find((t) => t.id === state.activeTaskId);
    if (!active) {
      return [];
    }
    const sourceCues =
      active.status === 'extracting' ? active.streamingPreviewCues.map((e, i) => ({
        ...e,
        local_uid: `stream_${e.id}_${i}`,
      })) : active.cues;
    return findCueMatches(active.id, sourceCues, fr.query, fr.caseSensitive);
  };

  const jumpToMatch = (taskId: string, localUid: string) => {
    const state = get();
    if (state.isDirtyInlineEdit && state.activeTaskId !== taskId) {
      return;
    }
    const task = state.tasks.find((t) => t.id === taskId);
    if (!task) {
      return;
    }
    const cue =
      task.cues.find((c) => c.local_uid === localUid) ??
      (task.status === 'extracting'
        ? task.streamingPreviewCues.find(
            (e, i) => `stream_${e.id}_${i}` === localUid
          )
        : undefined);
    if (!cue) {
      return;
    }
    const mid = computeCueMidpointMs(cue.start_ms, cue.end_ms);
    if (state.activeTaskId !== taskId) {
      if (state.activeTaskId) {
        helpers.stopPlaybackIfPlaying(state.activeTaskId);
      }
    } else {
      helpers.stopPlaybackIfPlaying(taskId);
    }
    set((s) => ({
      activeTaskId: taskId,
      tasks: s.tasks.map((t) =>
        t.id === taskId
          ? {
              ...t,
              activeCueId: localUid,
              previewTimestampMs: mid,
              playbackMode: 'still',
            }
          : t
      ),
    }));
  };

  return {
    setDirtyInlineEdit(
      dirty: boolean,
      inlineTextFocused?: boolean,
      cueUid?: string | null
    ): void {
      set((state) => {
        if (!dirty) {
          if (
            cueUid !== undefined &&
            state.editingCueUid !== null &&
            state.editingCueUid !== cueUid
          ) {
            return {};
          }
          return {
            isDirtyInlineEdit: false,
            isInlineTextFocused: false,
            editingCueUid: null,
          };
        }
        return {
          isDirtyInlineEdit: true,
          isInlineTextFocused:
            inlineTextFocused !== undefined
              ? inlineTextFocused
              : state.isInlineTextFocused,
          editingCueUid:
            cueUid !== undefined ? cueUid : state.editingCueUid,
        };
      });
    },

    selectCue(localUid: string, opts?: { seek?: boolean }): void {
      const active = getActiveReadyTask();
      if (!active) {
        return;
      }
      const cue =
        active.cues.find((c) => c.local_uid === localUid) ??
        active.streamingPreviewCues.find(
          (e, i) => `stream_${e.id}_${i}` === localUid
        );
      const shouldSeek = opts?.seek ?? true;
      if (shouldSeek && cue) {
        helpers.stopPlaybackIfPlaying(active.id);
      }
      const nextMid =
        shouldSeek && cue
          ? computeCueMidpointMs(cue.start_ms, cue.end_ms)
          : active.previewTimestampMs;

      updateTask(active.id, (item) => ({
        ...item,
        activeCueId: localUid,
        previewTimestampMs: nextMid,
        playbackMode: shouldSeek ? 'still' : item.playbackMode,
      }));
    },

    toggleCueSelection(localUid: string): void {
      const active = getActiveReadyTask();
      if (!active) {
        return;
      }
      updateTask(active.id, (item) => {
        const exists = item.selectedCueIds.includes(localUid);
        const nextSelected = exists
          ? item.selectedCueIds.filter((id) => id !== localUid)
          : [...item.selectedCueIds, localUid];
        return { ...item, selectedCueIds: nextSelected };
      });
    },

    setSelectedCueIds(localUids: string[]): void {
      const active = getActiveReadyTask();
      if (!active) {
        return;
      }
      updateTask(active.id, (item) => ({
        ...item,
        selectedCueIds: Array.from(new Set(localUids)),
      }));
    },

    selectAllLowConfCues(): void {
      const active = getActiveReadyTask();
      if (!active) {
        return;
      }
      const threshold = resolveLowConfidenceThreshold(
        active.activeRunConfig?.engine ?? active.engine
      );
      const lowIds = filterLowConfidenceCues(active.cues, threshold).map(
        (c) => c.local_uid
      );
      updateTask(active.id, (item) => ({
        ...item,
        selectedCueIds: lowIds,
      }));
    },

    clearCueSelection(): void {
      const active = getActiveReadyTask();
      if (!active) {
        return;
      }
      updateTask(active.id, (item) => ({
        ...item,
        selectedCueIds: [],
      }));
    },

    commitCueText(localUid: string, text: string): void {
      const active = getActiveReadyTask();
      if (!active || !isEditableStatus(active) || active.isReRecognizing) {
        return;
      }
      const target = active.cues.find((c) => c.local_uid === localUid);
      if (!target) {
        return;
      }
      if (target.text === text) {
        set({
          isDirtyInlineEdit: false,
          isInlineTextFocused: false,
          editingCueUid: null,
        });
        return;
      }
      const nextCues = updateCueText(active.cues, localUid, text);
      updateTask(active.id, (item) => applyCueMutation(item, nextCues));
      set({
        isDirtyInlineEdit: false,
        isInlineTextFocused: false,
        editingCueUid: null,
      });
      helpers.scheduleDraftSave(active.id);
    },

    commitCueTimes(
      localUid: string,
      startMs: number,
      endMs: number
    ): { ok: boolean; reason?: string } {
      const active = getActiveReadyTask();
      if (!active || !isEditableStatus(active) || active.isReRecognizing) {
        return { ok: false, reason: '当前状态不允许编辑字幕时间码' };
      }
      const res = updateCueTimes(active.cues, localUid, startMs, endMs);
      if (!res.ok) {
        return { ok: false, reason: res.reason };
      }
      updateTask(active.id, (item) => applyCueMutation(item, res.cues));
      set({
        isDirtyInlineEdit: false,
        isInlineTextFocused: false,
        editingCueUid: null,
      });
      helpers.scheduleDraftSave(active.id);
      return { ok: true };
    },

    commitCueEdit(
      localUid: string,
      patch: { text?: string; startMs?: number; endMs?: number }
    ): { ok: boolean; reason?: string } {
      const active = getActiveReadyTask();
      if (!active || !isEditableStatus(active) || active.isReRecognizing) {
        return { ok: false, reason: '当前状态不允许编辑字幕' };
      }
      const target = active.cues.find((c) => c.local_uid === localUid);
      if (!target) {
        return { ok: false, reason: '未找到目标字幕条目' };
      }

      const timesChanged =
        patch.startMs !== undefined &&
        patch.endMs !== undefined &&
        (patch.startMs !== target.start_ms || patch.endMs !== target.end_ms);
      const textChanged = patch.text !== undefined && patch.text !== target.text;

      let nextCues: ProofreadCue[] = [...active.cues];
      if (timesChanged) {
        const res = updateCueTimes(
          nextCues,
          localUid,
          patch.startMs!,
          patch.endMs!
        );
        if (!res.ok) {
          return { ok: false, reason: res.reason };
        }
        nextCues = res.cues;
      }
      if (textChanged) {
        nextCues = updateCueText(nextCues, localUid, patch.text!);
      }

      if (timesChanged || textChanged) {
        updateTask(active.id, (item) => applyCueMutation(item, nextCues));
        helpers.scheduleDraftSave(active.id);
      }

      set({
        isDirtyInlineEdit: false,
        isInlineTextFocused: false,
        editingCueUid: null,
      });
      return { ok: true };
    },

    nudgeCueTime(
      localUid: string,
      field: 'start_ms' | 'end_ms',
      deltaMs: number
    ): { ok: boolean; reason?: string } {
      const active = getActiveReadyTask();
      if (!active || !isEditableStatus(active) || active.isReRecognizing) {
        return { ok: false, reason: '当前状态不允许微调字幕时间码' };
      }
      const res = adjustCueTime(active.cues, localUid, field, deltaMs);
      if (!res.ok) {
        get().pushToast({
          severity: 'warning',
          title: '无法微调时间码',
          reason: res.reason,
        });
        return { ok: false, reason: res.reason };
      }
      updateTask(active.id, (item) => applyCueMutation(item, res.cues));
      helpers.scheduleDraftSave(active.id);
      return { ok: true };
    },

    deleteCue(localUid: string): void {
      const active = getActiveReadyTask();
      if (!active || !isEditableStatus(active) || active.isReRecognizing) {
        return;
      }
      const idx = active.cues.findIndex((c) => c.local_uid === localUid);
      if (idx < 0) {
        return;
      }
      const nextCues = deleteCueById(active.cues, localUid);
      const fallbackActive =
        nextCues[idx]?.local_uid ?? nextCues[idx - 1]?.local_uid ?? null;
      updateTask(active.id, (item) =>
        applyCueMutation(item, nextCues, {
          activeCueId:
            item.activeCueId === localUid ? fallbackActive : item.activeCueId,
          selectedCueIds: item.selectedCueIds.filter((id) => id !== localUid),
        })
      );
      helpers.scheduleDraftSave(active.id);
    },

    mergeCueWithNext(localUid: string): { ok: boolean; reason?: string } {
      const active = getActiveReadyTask();
      if (!active || !isEditableStatus(active) || active.isReRecognizing) {
        return { ok: false, reason: '当前状态不允许合并字幕' };
      }
      const idx = active.cues.findIndex((c) => c.local_uid === localUid);
      const removedUid = active.cues[idx + 1]?.local_uid;
      const res = mergeAdjacentCues(active.cues, idx);
      if (!res.ok) {
        get().pushToast({
          severity: 'warning',
          title: '无法合并字幕',
          reason: res.reason,
        });
        return { ok: false, reason: res.reason };
      }
      updateTask(active.id, (item) =>
        applyCueMutation(item, res.cues, {
          selectedCueIds: item.selectedCueIds.filter((id) => id !== removedUid),
        })
      );
      helpers.scheduleDraftSave(active.id);
      return { ok: true };
    },

    splitCue(
      localUid: string,
      cursorOffset: number,
      pendingText?: string
    ): { ok: boolean; reason?: string; splitLocalUid?: string } {
      const active = getActiveReadyTask();
      if (!active || !isEditableStatus(active) || active.isReRecognizing) {
        return { ok: false, reason: '当前状态不允许拆分字幕' };
      }
      const existing = active.cues.find((c) => c.local_uid === localUid);
      if (!existing) {
        return { ok: false, reason: '未找到目标字幕条目' };
      }
      const baseCues =
        pendingText !== undefined && pendingText !== existing.text
          ? updateCueText(active.cues, localUid, pendingText)
          : active.cues;
      const res = splitCueAtCursor(baseCues, localUid, cursorOffset);
      if (!res.ok) {
        get().pushToast({
          severity: 'warning',
          title: '无法拆分字幕',
          reason: res.reason,
        });
        return { ok: false, reason: res.reason };
      }
      updateTask(active.id, (item) =>
        applyCueMutation(item, res.cues, {
          activeCueId: res.splitLocalUid,
        })
      );
      set({
        isDirtyInlineEdit: false,
        isInlineTextFocused: false,
        editingCueUid: null,
      });
      helpers.scheduleDraftSave(active.id);
      return { ok: true, splitLocalUid: res.splitLocalUid };
    },

    undo(taskId?: string): void {
      const state = get();
      if (state.isDirtyInlineEdit || state.isInlineTextFocused) {
        return;
      }
      const targetId = taskId ?? state.activeTaskId;
      const target = state.tasks.find((t) => t.id === targetId);
      if (
        !target ||
        target.status === 'extracting' ||
        target.isReRecognizing ||
        !canUndo(target.history)
      ) {
        return;
      }
      const nextHistory = undoHistory(target.history);
      updateTask(target.id, (item) => ({
        ...item,
        cues: nextHistory.present,
        history: nextHistory,
        status: item.status === 'exported' ? 'ready' : item.status,
      }));
      helpers.scheduleDraftSave(target.id);
    },

    redo(taskId?: string): void {
      const state = get();
      if (state.isDirtyInlineEdit || state.isInlineTextFocused) {
        return;
      }
      const targetId = taskId ?? state.activeTaskId;
      const target = state.tasks.find((t) => t.id === targetId);
      if (
        !target ||
        target.status === 'extracting' ||
        target.isReRecognizing ||
        !canRedo(target.history)
      ) {
        return;
      }
      const nextHistory = redoHistory(target.history);
      updateTask(target.id, (item) => ({
        ...item,
        cues: nextHistory.present,
        history: nextHistory,
        status: item.status === 'exported' ? 'ready' : item.status,
      }));
      helpers.scheduleDraftSave(target.id);
    },

    openFind(showReplace: boolean = false): void {
      set((state) => ({
        findReplace: {
          ...state.findReplace,
          isOpen: true,
          showReplace: showReplace || state.findReplace.showReplace,
          focusRequestId: (state.findReplace.focusRequestId ?? 0) + 1,
        },
      }));
    },

    closeFind(): void {
      set((state) => ({
        findReplace: {
          ...state.findReplace,
          isOpen: false,
        },
      }));
    },

    setFindState(partial: Partial<FindReplaceState>): void {
      set((state) => ({
        findReplace: {
          ...state.findReplace,
          ...partial,
          activeMatchIndex:
            partial.query !== undefined ||
            partial.scope !== undefined ||
            partial.caseSensitive !== undefined
              ? 0
              : partial.activeMatchIndex ?? state.findReplace.activeMatchIndex,
        },
      }));
    },

    navigateFindMatch(direction: 'next' | 'prev'): void {
      const state = get();
      const matches = computeCurrentMatches(state);
      if (matches.length === 0) {
        return;
      }
      const current = state.findReplace.activeMatchIndex;
      const nextIdx =
        direction === 'next'
          ? (current + 1) % matches.length
          : (current - 1 + matches.length) % matches.length;
      const target = matches[nextIdx];
      if (state.isDirtyInlineEdit && target.taskId !== state.activeTaskId) {
        get().pushToast({
          severity: 'warning',
          title: '存在未提交的行内编辑',
          nextStep: '请先按 Enter 提交或按 Esc 放弃当前修改后再跨视频跳转',
        });
        return;
      }
      set((s) => ({
        findReplace: { ...s.findReplace, activeMatchIndex: nextIdx },
      }));
      jumpToMatch(target.taskId, target.localUid);
    },

    replaceCurrentMatch(): void {
      const state = get();
      if (state.isDirtyInlineEdit) {
        return;
      }
      const matches = computeCurrentMatches(state);
      if (matches.length === 0) {
        return;
      }
      const idx = Math.min(state.findReplace.activeMatchIndex, matches.length - 1);
      const match = matches[idx];
      const task = state.tasks.find((t) => t.id === match.taskId);
      if (!task || !isEditableStatus(task) || task.isReRecognizing) {
        return;
      }
      const nextCues = replaceSingleMatch(
        task.cues,
        match,
        state.findReplace.replacement
      );
      updateTask(task.id, (item) => applyCueMutation(item, nextCues));
      const nextMatches = computeCurrentMatches(get());
      const nextIndex =
        nextMatches.length === 0
          ? 0
          : Math.min(state.findReplace.activeMatchIndex, nextMatches.length - 1);
      set((s) => ({
        findReplace: {
          ...s.findReplace,
          activeMatchIndex: nextIndex,
        },
      }));
      helpers.scheduleDraftSave(task.id);
    },

    replaceAllMatches(): number {
      const state = get();
      if (state.isDirtyInlineEdit) {
        return 0;
      }
      const { query, replacement, scope, caseSensitive } = state.findReplace;
      if (!query) {
        return 0;
      }

      if (scope === 'current') {
        const active = getActiveReadyTask();
        if (!active || !isEditableStatus(active) || active.isReRecognizing) {
          return 0;
        }
        const res = replaceAllInCues(active.cues, query, replacement, caseSensitive);
        if (res.replacedCount > 0) {
          updateTask(active.id, (item) => applyCueMutation(item, res.cues));
          helpers.scheduleDraftSave(active.id);
        }
        return res.replacedCount;
      }

      const readyTasks = state.tasks
        .filter((t) => isEditableStatus(t) && !t.isReRecognizing)
        .map((t) => ({ taskId: t.id, cues: t.cues }));
      const batchRes = replaceAllAcrossQueue(
        readyTasks,
        query,
        replacement,
        caseSensitive
      );
      if (batchRes.totalReplaced > 0) {
        const updatesByTask = new Map(
          batchRes.updatedTasks.map((u) => [u.taskId, u.cues])
        );
        set((s) => ({
          tasks: s.tasks.map((t) => {
            const nextCues = updatesByTask.get(t.id);
            return nextCues ? applyCueMutation(t, nextCues) : t;
          }),
        }));
        for (const u of batchRes.updatedTasks) {
          helpers.scheduleDraftSave(u.taskId);
        }
      }
      return batchRes.totalReplaced;
    },

    async reRecognizeCues(
      localUids: string[]
    ): Promise<{
      ok: boolean;
      succeeded: number;
      failed: number;
      needsProvider?: boolean;
    }> {
      const state = get();
      const active = getActiveReadyTask();
      if (!active || !isEditableStatus(active)) {
        return { ok: false, succeeded: 0, failed: 0 };
      }
      if (state.isDirtyInlineEdit || active.isReRecognizing) {
        return { ok: false, succeeded: 0, failed: 0 };
      }

      const effectiveProviderId =
        active.providerId &&
        state.providers.some((p) => p.id === active.providerId)
          ? active.providerId
          : resolveDefaultProviderId(state.providers);
      if (!effectiveProviderId) {
        get().pushToast({
          severity: 'warning',
          title: '尚未配置 VLM 服务',
          reason: '局部重识需要至少一个有效的 VLM Provider 配置',
          nextStep: '请先在 VLM 设置中添加或从 .env 导入服务档案',
          actionLabel: '打开 VLM 设置',
          actionType: 'open_vlm_settings',
        });
        return { ok: false, succeeded: 0, failed: 0, needsProvider: true };
      }

      const targetSet = new Set(localUids);
      const selectedCues = active.cues.filter((c) => targetSet.has(c.local_uid));
      if (selectedCues.length === 0) {
        return { ok: true, succeeded: 0, failed: 0 };
      }

      const inputs: ReRecognizeItemInput[] = selectedCues.map((c) => ({
        cue_id: c.id,
        start_ms: c.start_ms,
        end_ms: c.end_ms,
      }));

      const snapshotCues = active.cues;
      const reRecGen = (reRecGenMap.get(active.id) ?? 0) + 1;
      reRecGenMap.set(active.id, reRecGen);

      updateTask(active.id, (item) => ({ ...item, isReRecognizing: true }));

      try {
        const results = await state.bridge.reRecognizeEntries(
          active.videoPath,
          active.regionBox ?? undefined,
          inputs,
          effectiveProviderId,
          active.script
        );

        const resultByCueId = new Map<number, ReRecognizeResult>();
        let succeeded = 0;
        let failed = 0;
        let firstError: string | undefined;

        for (const r of results) {
          resultByCueId.set(r.cue_id, r);
          if (r.ok) {
            succeeded += 1;
          } else {
            failed += 1;
            if (!firstError && r.error_message) {
              firstError = r.error_message;
            }
          }
        }

        // 若后端返回缺漏，按未成功计入 failed
        for (const inp of inputs) {
          if (!resultByCueId.has(inp.cue_id)) {
            failed += 1;
          }
        }

        const currentTask = get().tasks.find((t) => t.id === active.id);
        if (!currentTask) {
          return { ok: false, succeeded: 0, failed: inputs.length };
        }
        if (
          reRecGenMap.get(active.id) !== reRecGen ||
          !isEditableStatus(currentTask) ||
          currentTask.cues !== snapshotCues
        ) {
          if (
            reRecGenMap.get(active.id) === reRecGen &&
            currentTask.isReRecognizing
          ) {
            updateTask(active.id, (item) => ({
              ...item,
              isReRecognizing: false,
            }));
          }
          return { ok: false, succeeded: 0, failed: inputs.length };
        }

        if (succeeded > 0) {
          const nextCues = currentTask.cues.map((cue) => {
            if (!targetSet.has(cue.local_uid)) {
              return cue;
            }
            const patch = resultByCueId.get(cue.id);
            if (!patch || !patch.ok) {
              return cue;
            }
            const nextText =
              patch.text !== undefined
                ? patch.text
                : patch.category === 'empty'
                  ? ''
                  : cue.text;
            const nextConfidence =
              patch.confidence !== undefined ? patch.confidence : cue.confidence;
            return {
              ...cue,
              text: nextText,
              confidence: nextConfidence,
              category: patch.category ?? cue.category,
              re_recognized: true,
            };
          });
          updateTask(active.id, (item) =>
            isEditableStatus(item) && item.cues === snapshotCues
              ? applyCueMutation(item, nextCues, { isReRecognizing: false })
              : { ...item, isReRecognizing: false }
          );
          helpers.scheduleDraftSave(active.id);
        } else {
          updateTask(active.id, (item) => ({ ...item, isReRecognizing: false }));
        }

        if (failed > 0) {
          get().pushToast({
            severity: 'warning',
            title: `VLM 局部重识完成：${succeeded} 条成功，${failed} 条失败`,
            reason: firstError ?? '部分条带未返回有效文本，已保留原字幕',
            nextStep: succeeded > 0 ? '成功条目已合并为单次可撤销修改（⌘Z）' : '可检查网络或切换模型后重试',
          });
        } else {
          get().pushToast({
            severity: 'success',
            title: `已完成 ${succeeded} 条字幕的 VLM 局部重识`,
            nextStep: '可使用 ⌘Z 一键撤销本次批量替换',
          });
        }

        return { ok: failed === 0, succeeded, failed };
      } catch (err) {
        const parsed = parseIpcError(err);
        if (reRecGenMap.get(active.id) === reRecGen) {
          updateTask(active.id, (item) =>
            item.isReRecognizing ? { ...item, isReRecognizing: false } : item
          );
        }
        get().pushToast({
          severity: 'error',
          title: 'VLM 局部重识请求失败',
          reason: parsed.message,
          nextStep: '请检查 VLM 服务配置或网络连接后重试',
          actionLabel:
            parsed.code === 'VlmNotImplemented' ? undefined : '打开 VLM 设置',
          actionType:
            parsed.code === 'VlmNotImplemented' ? undefined : 'open_vlm_settings',
        });
        return { ok: false, succeeded: 0, failed: inputs.length };
      }
    },

    async reRecognizeAllLowConf(): Promise<{
      ok: boolean;
      succeeded: number;
      failed: number;
      needsProvider?: boolean;
    }> {
      const active = getActiveReadyTask();
      if (!active) {
        return { ok: false, succeeded: 0, failed: 0 };
      }
      const threshold = resolveLowConfidenceThreshold(
        active.activeRunConfig?.engine ?? active.engine
      );
      const lowUids = filterLowConfidenceCues(active.cues, threshold).map(
        (c) => c.local_uid
      );
      if (lowUids.length === 0) {
        get().pushToast({
          severity: 'info',
          title: '当前剧集没有低置信度或待补录条目',
        });
        return { ok: true, succeeded: 0, failed: 0 };
      }
      return get().reRecognizeCues(lowUids);
    },
  };
}
