import { ref, watch, onBeforeUnmount, nextTick, type Ref } from 'vue';
import { FocusTrap } from '../utils/focus_trap';

export interface UseModalA11yOptions {
  /** Selector for the initial element to focus, e.g. 'input' or '.sl-modal-btn-confirm' */
  initialFocusSelector?: string;
  /** Callback when Escape key is pressed */
  onClose?: () => void;
}

export function useModalA11y(
  isOpen: Ref<boolean>,
  modalContainerRef: Ref<HTMLElement | null>,
  options: UseModalA11yOptions = {}
) {
  let trap: FocusTrap | null = null;
  const triggerElement = ref<HTMLElement | null>(null);

  function activateTrap() {
    if (!modalContainerRef.value) return;
    
    trap = new FocusTrap(modalContainerRef.value, {
      initialFocus: options.initialFocusSelector || null,
      returnFocusElement: triggerElement.value,
      onEscape: () => {
        if (options.onClose) {
          options.onClose();
        } else {
          isOpen.value = false;
        }
      },
    });

    trap.activate();
  }

  function deactivateTrap() {
    if (trap) {
      trap.deactivate();
      trap = null;
    }
  }

  watch(
    isOpen,
    async (open) => {
      if (open) {
        if (document.activeElement instanceof HTMLElement) {
          triggerElement.value = document.activeElement;
        }
        await nextTick();
        activateTrap();
      } else {
        deactivateTrap();
      }
    },
    { immediate: true }
  );

  onBeforeUnmount(() => {
    deactivateTrap();
  });

  return {
    triggerElement,
    activateTrap,
    deactivateTrap,
  };
}
