import { canRedo, canUndo } from '../core/history';
import {
  selectWorkbenchCommands,
  type WorkbenchCommandMatrix,
  type WorkbenchCommandsInput,
} from '../core/commands';
import {
  countLowConfidenceCues,
  filterLowConfidenceCues,
  resolveLowConfidenceThreshold,
} from '../core/subtitle';
import { resolveDefaultProviderId } from './providerSlice';
import type {
  ProofreadCue,
  ProviderProfileMaskedDto,
  QueueTaskItem,
  WorkbenchState,
} from './types';

export function selectActiveTask(state: WorkbenchState): QueueTaskItem | null {
  if (!state.activeTaskId) {
    return null;
  }
  return state.tasks.find((t) => t.id === state.activeTaskId) ?? null;
}

export function selectEffectiveProvider(
  state: WorkbenchState,
  item?: QueueTaskItem | null
): ProviderProfileMaskedDto | null {
  const target = item ?? selectActiveTask(state);
  if (target?.providerId) {
    const matched = state.providers.find((p) => p.id === target.providerId);
    if (matched) {
      return matched;
    }
  }
  const defaultId = resolveDefaultProviderId(state.providers);
  if (!defaultId) {
    return null;
  }
  return state.providers.find((p) => p.id === defaultId) ?? null;
}

export function selectHasValidVlmProvider(
  state: WorkbenchState,
  item?: QueueTaskItem | null
): boolean {
  const provider = selectEffectiveProvider(state, item);
  return provider !== null && Boolean(provider.base_url && provider.model);
}

let lastLowConfCuesInput: readonly ProofreadCue[] | null = null;
let lastLowConfThreshold = -1;
let lastLowConfCuesResult: ProofreadCue[] = [];

export function selectLowConfCues(state: WorkbenchState): ProofreadCue[] {
  const active = selectActiveTask(state);
  if (!active) {
    lastLowConfCuesInput = null;
    lastLowConfThreshold = -1;
    return lastLowConfCuesResult.length === 0 ? lastLowConfCuesResult : (lastLowConfCuesResult = []);
  }
  const threshold = resolveLowConfidenceThreshold(
    active.activeRunConfig?.engine ?? active.engine
  );
  if (
    lastLowConfCuesInput === active.cues &&
    lastLowConfThreshold === threshold
  ) {
    return lastLowConfCuesResult;
  }
  lastLowConfCuesInput = active.cues;
  lastLowConfThreshold = threshold;
  lastLowConfCuesResult = filterLowConfidenceCues(active.cues, threshold);
  return lastLowConfCuesResult;
}

export function selectLowConfCount(state: WorkbenchState): number {
  const active = selectActiveTask(state);
  if (!active) {
    return 0;
  }
  const threshold = resolveLowConfidenceThreshold(
    active.activeRunConfig?.engine ?? active.engine
  );
  return countLowConfidenceCues(active.cues, threshold);
}

let lastCommandsInput: WorkbenchCommandsInput | null = null;
let lastCommandsMatrix: WorkbenchCommandMatrix | null = null;

function shallowEqualCommandsInput(
  a: WorkbenchCommandsInput,
  b: WorkbenchCommandsInput
): boolean {
  return (
    a.hasActiveTask === b.hasActiveTask &&
    a.activeStatus === b.activeStatus &&
    a.activeProbe === b.activeProbe &&
    a.activeEngine === b.activeEngine &&
    a.capabilities === b.capabilities &&
    a.hasValidVlmProvider === b.hasValidVlmProvider &&
    a.isAutoDetecting === b.isAutoDetecting &&
    a.isReRecognizing === b.isReRecognizing &&
    a.isDirtyInlineEdit === b.isDirtyInlineEdit &&
    a.isInlineTextFocused === b.isInlineTextFocused &&
    a.activeCueCount === b.activeCueCount &&
    a.queueLength === b.queueLength &&
    a.hasApplyTargets === b.hasApplyTargets &&
    a.exportableQueueCount === b.exportableQueueCount &&
    a.canUndo === b.canUndo &&
    a.canRedo === b.canRedo
  );
}

export function selectActiveCommands(
  state: WorkbenchState
): WorkbenchCommandMatrix {
  const active = selectActiveTask(state);
  const hasApplyTargets =
    active !== null &&
    state.tasks.some(
      (t) =>
        t.id !== active.id && t.status !== 'extracting' && !t.isReRecognizing
    );
  const exportableQueueCount = state.tasks.filter(
    (t) =>
      (t.status === 'ready' || t.status === 'exported') && t.cues.length > 0
  ).length;

  const input: WorkbenchCommandsInput = {
    hasActiveTask: active !== null,
    activeStatus: active?.status ?? null,
    activeProbe: active?.probe ?? null,
    activeEngine: active?.engine ?? null,
    capabilities: state.capabilities,
    hasValidVlmProvider: selectHasValidVlmProvider(state, active),
    isAutoDetecting: active?.isAutoDetecting ?? false,
    isReRecognizing: active?.isReRecognizing ?? false,
    isDirtyInlineEdit: state.isDirtyInlineEdit,
    isInlineTextFocused: state.isInlineTextFocused,
    activeCueCount: active?.cues.length ?? 0,
    queueLength: state.tasks.length,
    hasApplyTargets,
    exportableQueueCount,
    canUndo: active ? canUndo(active.history) : false,
    canRedo: active ? canRedo(active.history) : false,
  };

  if (
    lastCommandsInput &&
    lastCommandsMatrix &&
    shallowEqualCommandsInput(lastCommandsInput, input)
  ) {
    return lastCommandsMatrix;
  }

  lastCommandsInput = input;
  lastCommandsMatrix = selectWorkbenchCommands(input);
  return lastCommandsMatrix;
}
