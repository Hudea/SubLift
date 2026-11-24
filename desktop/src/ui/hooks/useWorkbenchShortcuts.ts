import { useEffect } from 'react';
import {
  selectActiveCommands,
  selectActiveTask,
  useWorkbenchStore,
} from '../../store';

function isFormInputNode(node: Element | null): boolean {
  if (!node) {
    return false;
  }
  const tagName = node.tagName;
  if (tagName === 'INPUT' || tagName === 'TEXTAREA' || tagName === 'SELECT') {
    return true;
  }
  return node instanceof HTMLElement && node.isContentEditable;
}

function isFloatingOverlayOpen(node: Element | null): boolean {
  if (node && node.closest('[role="dialog"], [role="menu"], [role="listbox"]')) {
    return true;
  }
  return Boolean(document.querySelector('[role="dialog"]'));
}

function isInteractiveControlNode(node: Element | null): boolean {
  if (!node) {
    return false;
  }
  return Boolean(
    node.closest(
      'button, a, [role="button"], [role="checkbox"], [role="slider"], [role="tab"], [role="region"][aria-label="字幕选区 ROI"]'
    )
  );
}

export function useWorkbenchShortcuts() {
  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.isComposing || event.keyCode === 229) {
        return;
      }

      const state = useWorkbenchStore.getState();
      const cmds = selectActiveCommands(state);
      const activeTask = selectActiveTask(state);
      const focused = document.activeElement;
      const inForm = isFormInputNode(focused);
      const inOverlay = isFloatingOverlayOpen(focused);
      const inInteractiveControl = isInteractiveControlNode(focused);
      const modKey = event.metaKey || event.ctrlKey;
      const lowerKey = event.key.toLowerCase();

      if (inOverlay) {
        return;
      }

      if (modKey && !event.shiftKey && !event.altKey && lowerKey === 'b') {
        event.preventDefault();
        state.toggleQueueSidebar();
        return;
      }

      if (
        (event.metaKey && event.altKey && lowerKey === 'f') ||
        (event.ctrlKey && !event.metaKey && !event.shiftKey && lowerKey === 'h')
      ) {
        event.preventDefault();
        state.openFind(true);
        return;
      }

      if (modKey && !event.shiftKey && !event.altKey && lowerKey === 'f') {
        event.preventDefault();
        state.openFind(false);
        return;
      }

      if (modKey && !event.shiftKey && !event.altKey && lowerKey === 's') {
        if (cmds.exportActiveSrt.enabled) {
          event.preventDefault();
          void state.exportActiveSrt();
        }
        return;
      }

      if (inForm) {
        return;
      }

      if (state.isDirtyInlineEdit) {
        return;
      }

      if (modKey && lowerKey === 'z') {
        if (event.shiftKey) {
          if (cmds.redo.enabled) {
            event.preventDefault();
            state.redo();
          }
        } else if (cmds.undo.enabled) {
          event.preventDefault();
          state.undo();
        }
        return;
      }

      if (inInteractiveControl) {
        return;
      }

      if (event.code === 'Space' || event.key === ' ') {
        if (activeTask && activeTask.probe) {
          event.preventDefault();
          state.setPlaybackMode(
            activeTask.playbackMode === 'playing' ? 'still' : 'playing'
          );
        }
        return;
      }

      if (event.key === 'ArrowLeft' && !modKey && !event.altKey) {
        if (activeTask && activeTask.probe) {
          event.preventDefault();
          state.stepPreview(event.shiftKey ? -5000 : -1000);
        }
        return;
      }

      if (event.key === 'ArrowRight' && !modKey && !event.altKey) {
        if (activeTask && activeTask.probe) {
          event.preventDefault();
          state.stepPreview(event.shiftKey ? 5000 : 1000);
        }
        return;
      }

      if (event.key === 'Delete' || event.key === 'Backspace') {
        if (activeTask?.activeCueId && cmds.editCue.enabled) {
          event.preventDefault();
          state.deleteCue(activeTask.activeCueId);
        }
      }
    };

    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, []);
}
