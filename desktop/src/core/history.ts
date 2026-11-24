export interface UndoRedoHistory<T> {
  past: readonly T[];
  present: T;
  future: readonly T[];
  maxDepth: number;
}

export function createHistory<T>(
  present: T,
  maxDepth: number = 100
): UndoRedoHistory<T> {
  const safeDepth = Number.isFinite(maxDepth) && maxDepth >= 1 ? Math.floor(maxDepth) : 100;
  return {
    past: [],
    present,
    future: [],
    maxDepth: safeDepth,
  };
}

export function pushHistory<T>(
  history: UndoRedoHistory<T>,
  nextPresent: T
): UndoRedoHistory<T> {
  if (Object.is(history.present, nextPresent)) {
    return history;
  }
  const nextPast = [...history.past, history.present];
  const trimmedPast =
    nextPast.length > history.maxDepth
      ? nextPast.slice(nextPast.length - history.maxDepth)
      : nextPast;
  return {
    past: trimmedPast,
    present: nextPresent,
    future: [],
    maxDepth: history.maxDepth,
  };
}

export function canUndo<T>(history: UndoRedoHistory<T>): boolean {
  return history.past.length > 0;
}

export function canRedo<T>(history: UndoRedoHistory<T>): boolean {
  return history.future.length > 0;
}

export function undoHistory<T>(
  history: UndoRedoHistory<T>
): UndoRedoHistory<T> {
  if (history.past.length === 0) {
    return history;
  }
  const previous = history.past[history.past.length - 1];
  return {
    past: history.past.slice(0, -1),
    present: previous,
    future: [history.present, ...history.future],
    maxDepth: history.maxDepth,
  };
}

export function redoHistory<T>(
  history: UndoRedoHistory<T>
): UndoRedoHistory<T> {
  if (history.future.length === 0) {
    return history;
  }
  const next = history.future[0];
  return {
    past: [...history.past, history.present],
    present: next,
    future: history.future.slice(1),
    maxDepth: history.maxDepth,
  };
}
