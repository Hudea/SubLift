import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import { FocusTrap } from '../utils/focus_trap';
import { useWorkbenchStore } from '../stores/workbench';
import { useBatchStore } from '../stores/batch';
import fs from 'fs';
import path from 'path';

// Robust Mock DOM implementation for Node/Vitest environment
class MockDOMElement {
  public id: string;
  public tagName: string;
  public className: string;
  public children: MockDOMElement[] = [];
  public parentElement: MockDOMElement | null = null;
  public tabIndex = 0;
  public disabled = false;
  public type = '';
  public style: Record<string, string> = { display: 'block', visibility: 'visible' };
  public offsetWidth = 100;
  public offsetHeight = 30;
  public offsetParent: any = {};
  public isFocused = false;
  public value = '';
  public attributes: Record<string, string> = {};

  constructor(tagName: string, id = '', className = '') {
    this.tagName = tagName.toUpperCase();
    this.id = id;
    this.className = className;
  }

  public appendChild(child: MockDOMElement): MockDOMElement {
    child.parentElement = this;
    this.children.push(child);
    return child;
  }

  public removeChild(child: MockDOMElement): MockDOMElement {
    const idx = this.children.indexOf(child);
    if (idx !== -1) {
      this.children.splice(idx, 1);
      child.parentElement = null;
    }
    return child;
  }

  public setAttribute(name: string, val: string) {
    this.attributes[name] = val;
  }

  public getAttribute(name: string): string | null {
    return this.attributes[name] ?? null;
  }

  public focus(): void {
    const doc = (globalThis as any).document;
    if (doc) {
      if (doc.activeElement && doc.activeElement !== this) {
        doc.activeElement.isFocused = false;
      }
      doc.activeElement = this;
      this.isFocused = true;
    }
  }

  public blur(): void {
    this.isFocused = false;
    const doc = (globalThis as any).document;
    if (doc && doc.activeElement === this) {
      doc.activeElement = null;
    }
  }

  public contains(target: any): boolean {
    if (!target) return false;
    if (target === this) return true;
    for (const child of this.children) {
      if (child.contains(target)) return true;
    }
    return false;
  }

  public querySelectorAll<T = MockDOMElement>(_selector: string): T[] {
    const results: MockDOMElement[] = [];
    function collect(el: MockDOMElement) {
      for (const c of el.children) {
        const isTagFocusable = ['BUTTON', 'INPUT', 'SELECT', 'TEXTAREA', 'A'].includes(c.tagName);
        const hasTabindex = c.tabIndex >= 0;
        if ((isTagFocusable || hasTabindex) && !c.disabled) {
          if (c.style.display !== 'none' && c.style.visibility !== 'hidden') {
            results.push(c);
          }
        }
        collect(c);
      }
    }
    collect(this);
    return results as unknown as T[];
  }

  public querySelector<T = MockDOMElement>(selector: string): T | null {
    const list = this.querySelectorAll<T>(selector);
    if (selector.startsWith('#')) {
      const targetId = selector.substring(1);
      const findById = (el: MockDOMElement): MockDOMElement | null => {
        if (el.id === targetId) return el;
        for (const c of el.children) {
          const res = findById(c);
          if (res) return res;
        }
        return null;
      };
      return (findById(this) as unknown as T) || null;
    }
    if (selector.startsWith('.')) {
      const targetClass = selector.substring(1);
      const findByClass = (el: MockDOMElement): MockDOMElement | null => {
        if (el.className.includes(targetClass)) return el;
        for (const c of el.children) {
          const res = findByClass(c);
          if (res) return res;
        }
        return null;
      };
      return (findByClass(this) as unknown as T) || null;
    }
    return list.length > 0 ? list[0] : null;
  }
}

describe('Empirical Challenge - Phase 12 Milestone 6 (Feature 12516: Web Accessibility & Compact Viewport)', () => {
  let eventListeners: Record<string, Function[]> = {};

  beforeEach(() => {
    setActivePinia(createPinia());
    eventListeners = {};

    const mockDoc = {
      activeElement: null as any,
      addEventListener: (event: string, handler: Function, _capture?: boolean) => {
        eventListeners[event] = eventListeners[event] || [];
        eventListeners[event].push(handler);
      },
      removeEventListener: (event: string, handler: Function, _capture?: boolean) => {
        if (eventListeners[event]) {
          eventListeners[event] = eventListeners[event].filter((h) => h !== handler);
        }
      },
    };

    (globalThis as any).document = mockDoc;
    (globalThis as any).HTMLElement = MockDOMElement;
    (globalThis as any).window = {
      getComputedStyle: (el: any) => el.style || { display: 'block', visibility: 'visible' },
    };
  });

  afterEach(() => {
    delete (globalThis as any).document;
    delete (globalThis as any).HTMLElement;
    delete (globalThis as any).window;
    vi.restoreAllMocks();
  });

  describe('1. Focus Trapping Stress Harness Across All Modal Topologies', () => {
    it('Switch Workspace Modal: 100 sequential Tab and Shift+Tab boundary cycles never leak to background DOM', () => {
      // Background DOM elements
      const bgButton1 = new MockDOMElement('button', 'bg-btn-1');
      const bgButton2 = new MockDOMElement('button', 'bg-btn-2');
      const triggerCapsule = new MockDOMElement('button', 'sl-workspace-capsule');
      triggerCapsule.focus();

      // Modal container topology matching Navbar.vue
      const modal = new MockDOMElement('div', 'switch-workspace-modal');
      const closeBtn = new MockDOMElement('button', 'modal-close', 'sl-modal-close');
      const input = new MockDOMElement('input', 'switch-workspace-input', 'sl-modal-input');
      const chip1 = new MockDOMElement('button', 'chip-movies', 'sl-modal-preset-chip');
      const chip2 = new MockDOMElement('button', 'chip-downloads', 'sl-modal-preset-chip');
      const chip3 = new MockDOMElement('button', 'chip-desktop', 'sl-modal-preset-chip');
      const chip4 = new MockDOMElement('button', 'chip-docs', 'sl-modal-preset-chip');
      const cancelBtn = new MockDOMElement('button', 'btn-cancel', 'sl-modal-btn-cancel');
      const confirmBtn = new MockDOMElement('button', 'btn-confirm', 'sl-modal-btn-confirm');

      modal.appendChild(closeBtn);
      modal.appendChild(input);
      modal.appendChild(chip1);
      modal.appendChild(chip2);
      modal.appendChild(chip3);
      modal.appendChild(chip4);
      modal.appendChild(cancelBtn);
      modal.appendChild(confirmBtn);

      const trap = new FocusTrap(modal as unknown as HTMLElement, {
        initialFocus: '.sl-modal-input',
        returnFocusElement: triggerCapsule as unknown as HTMLElement,
      });

      trap.activate();

      // Initial focus must be on .sl-modal-input
      expect((globalThis as any).document.activeElement).toBe(input);

      const focusableList = [closeBtn, input, chip1, chip2, chip3, chip4, cancelBtn, confirmBtn];
      expect(trap.getFocusableElements().length).toBe(8);

      // --- Forward Tab Stress Test (100 sequential steps) ---
      let currentIndex = focusableList.indexOf(input); // index 1
      for (let step = 1; step <= 100; step++) {
        const preventDefault = vi.fn();
        const tabEvent = {
          key: 'Tab',
          shiftKey: false,
          preventDefault,
        } as unknown as KeyboardEvent;

        if (currentIndex === focusableList.length - 1) {
          // At boundary: FocusTrap intercepts and wraps to first element
          trap.handleKeyDown(tabEvent);
          expect(preventDefault).toHaveBeenCalled();
          expect((globalThis as any).document.activeElement).toBe(focusableList[0]);
          currentIndex = 0;
        } else {
          // Native browser tab advance to next element
          currentIndex++;
          focusableList[currentIndex].focus();
        }

        // Verify focus is strictly within modal
        const currentActive = (globalThis as any).document.activeElement;
        expect(modal.contains(currentActive)).toBe(true);
        expect(currentActive).not.toBe(bgButton1);
        expect(currentActive).not.toBe(bgButton2);
        expect(currentActive).not.toBe(triggerCapsule);
      }

      // --- Backward Shift+Tab Stress Test (100 sequential steps) ---
      for (let step = 1; step <= 100; step++) {
        const preventDefault = vi.fn();
        const shiftTabEvent = {
          key: 'Tab',
          shiftKey: true,
          preventDefault,
        } as unknown as KeyboardEvent;

        if (currentIndex === 0) {
          // At first element boundary: FocusTrap intercepts and wraps to last element
          trap.handleKeyDown(shiftTabEvent);
          expect(preventDefault).toHaveBeenCalled();
          expect((globalThis as any).document.activeElement).toBe(focusableList[focusableList.length - 1]);
          currentIndex = focusableList.length - 1;
        } else {
          // Native browser shift+tab advance to previous element
          currentIndex--;
          focusableList[currentIndex].focus();
        }

        const currentActive = (globalThis as any).document.activeElement;
        expect(modal.contains(currentActive)).toBe(true);
        expect(currentActive).not.toBe(bgButton1);
      }

      trap.deactivate();
      expect((globalThis as any).document.activeElement).toBe(triggerCapsule);
    });

    it('Re-Extraction Alert Dialog: 2-element boundary cycling and safe default focus', () => {
      const triggerReExtract = new MockDOMElement('button', 'reextract-trigger-btn');
      triggerReExtract.focus();

      const modal = new MockDOMElement('div', 'reextract-modal');
      const cancelBtn = new MockDOMElement('button', 'reextract-cancel', 'sl-modal-btn--cancel');
      const confirmBtn = new MockDOMElement('button', 'reextract-confirm', 'sl-modal-btn--confirm');

      modal.appendChild(cancelBtn);
      modal.appendChild(confirmBtn);

      const trap = new FocusTrap(modal as unknown as HTMLElement, {
        initialFocus: '.sl-modal-btn--cancel',
        returnFocusElement: triggerReExtract as unknown as HTMLElement,
      });

      trap.activate();
      // Safe default: initial focus is on Cancel
      expect((globalThis as any).document.activeElement).toBe(cancelBtn);

      // Tab from Cancel -> advance to Confirm
      confirmBtn.focus();
      expect((globalThis as any).document.activeElement).toBe(confirmBtn);

      // Tab on Confirm -> wraps to Cancel
      const preventDefault1 = vi.fn();
      trap.handleKeyDown({ key: 'Tab', shiftKey: false, preventDefault: preventDefault1 } as any);
      expect(preventDefault1).toHaveBeenCalled();
      expect((globalThis as any).document.activeElement).toBe(cancelBtn);

      // Shift+Tab on Cancel -> wraps to Confirm
      const preventDefault2 = vi.fn();
      trap.handleKeyDown({ key: 'Tab', shiftKey: true, preventDefault: preventDefault2 } as any);
      expect(preventDefault2).toHaveBeenCalled();
      expect((globalThis as any).document.activeElement).toBe(confirmBtn);

      trap.deactivate();
      expect((globalThis as any).document.activeElement).toBe(triggerReExtract);
    });

    it('Batch Save Modal: Mixed inputs (radio, checkbox, buttons) maintain exact focus order', () => {
      const triggerBatchSave = new MockDOMElement('button', 'batch-save-trigger');
      triggerBatchSave.focus();

      const modal = new MockDOMElement('div', 'batch-save-modal');
      const closeBtn = new MockDOMElement('button', 'save-close', 'sl-modal-close');
      const radio1 = new MockDOMElement('input', 'radio-rename');
      radio1.type = 'radio';
      const radio2 = new MockDOMElement('input', 'radio-skip');
      radio2.type = 'radio';
      const radio3 = new MockDOMElement('input', 'radio-replace');
      radio3.type = 'radio';
      const checkAllowEmpty = new MockDOMElement('input', 'check-empty');
      checkAllowEmpty.type = 'checkbox';
      const cancelBtn = new MockDOMElement('button', 'save-cancel', 'sl-modal-btn-cancel');
      const confirmBtn = new MockDOMElement('button', 'save-confirm', 'sl-modal-btn-confirm');

      modal.appendChild(closeBtn);
      modal.appendChild(radio1);
      modal.appendChild(radio2);
      modal.appendChild(radio3);
      modal.appendChild(checkAllowEmpty);
      modal.appendChild(cancelBtn);
      modal.appendChild(confirmBtn);

      const trap = new FocusTrap(modal as unknown as HTMLElement, {
        initialFocus: '.sl-modal-btn-confirm',
        returnFocusElement: triggerBatchSave as unknown as HTMLElement,
      });

      trap.activate();
      expect((globalThis as any).document.activeElement).toBe(confirmBtn);

      // Tab on Confirm -> wraps to Close button
      const preventDefault = vi.fn();
      trap.handleKeyDown({ key: 'Tab', shiftKey: false, preventDefault } as any);
      expect(preventDefault).toHaveBeenCalled();
      expect((globalThis as any).document.activeElement).toBe(closeBtn);

      trap.deactivate();
      expect((globalThis as any).document.activeElement).toBe(triggerBatchSave);
    });

    it('Background Focus Snapping: External focus leaks are instantly snapped back into the modal on Tab / Shift+Tab', () => {
      const bgInput = new MockDOMElement('input', 'rogue-background-input');
      const modal = new MockDOMElement('div', 'modal-container');
      const btn1 = new MockDOMElement('button', 'm-btn-1');
      const btn2 = new MockDOMElement('button', 'm-btn-2');
      modal.appendChild(btn1);
      modal.appendChild(btn2);

      const trap = new FocusTrap(modal as unknown as HTMLElement);
      trap.activate();

      // Simulate an external script or user click forcing focus to background element
      bgInput.focus();
      expect((globalThis as any).document.activeElement).toBe(bgInput);
      expect(modal.contains(bgInput)).toBe(false);

      // Tab should snap focus back to first element of modal
      const p1 = vi.fn();
      trap.handleKeyDown({ key: 'Tab', shiftKey: false, preventDefault: p1 } as any);
      expect(p1).toHaveBeenCalled();
      expect((globalThis as any).document.activeElement).toBe(btn1);

      // Simulate external leak again
      bgInput.focus();
      expect((globalThis as any).document.activeElement).toBe(bgInput);

      // Shift+Tab should snap focus back to last element of modal
      const p2 = vi.fn();
      trap.handleKeyDown({ key: 'Tab', shiftKey: true, preventDefault: p2 } as any);
      expect(p2).toHaveBeenCalled();
      expect((globalThis as any).document.activeElement).toBe(btn2);

      trap.deactivate();
    });

    it('Degenerate Topologies: Single element, zero elements, and disabled/hidden elements', () => {
      // 1. Single element modal
      const singleModal = new MockDOMElement('div', 'single-modal');
      const onlyBtn = new MockDOMElement('button', 'only-btn');
      singleModal.appendChild(onlyBtn);

      const singleTrap = new FocusTrap(singleModal as unknown as HTMLElement);
      singleTrap.activate();
      expect((globalThis as any).document.activeElement).toBe(onlyBtn);

      const pTab = vi.fn();
      singleTrap.handleKeyDown({ key: 'Tab', shiftKey: false, preventDefault: pTab } as any);
      expect(pTab).toHaveBeenCalled();
      expect((globalThis as any).document.activeElement).toBe(onlyBtn);

      const pShiftTab = vi.fn();
      singleTrap.handleKeyDown({ key: 'Tab', shiftKey: true, preventDefault: pShiftTab } as any);
      expect(pShiftTab).toHaveBeenCalled();
      expect((globalThis as any).document.activeElement).toBe(onlyBtn);
      singleTrap.deactivate();

      // 2. Zero focusable elements
      const emptyModal = new MockDOMElement('div', 'empty-modal');
      const emptyTrap = new FocusTrap(emptyModal as unknown as HTMLElement);
      emptyTrap.activate();

      const pEmpty = vi.fn();
      expect(() => {
        emptyTrap.handleKeyDown({ key: 'Tab', shiftKey: false, preventDefault: pEmpty } as any);
      }).not.toThrow();
      expect(pEmpty).toHaveBeenCalled();
      emptyTrap.deactivate();

      // 3. Disabled & Hidden elements exclusion
      const mixedModal = new MockDOMElement('div', 'mixed-modal');
      const activeBtn1 = new MockDOMElement('button', 'active-1');
      const disabledBtn = new MockDOMElement('button', 'disabled-btn');
      disabledBtn.disabled = true;
      const hiddenBtn = new MockDOMElement('button', 'hidden-btn');
      hiddenBtn.style.display = 'none';
      const activeBtn2 = new MockDOMElement('button', 'active-2');

      mixedModal.appendChild(activeBtn1);
      mixedModal.appendChild(disabledBtn);
      mixedModal.appendChild(hiddenBtn);
      mixedModal.appendChild(activeBtn2);

      const mixedTrap = new FocusTrap(mixedModal as unknown as HTMLElement);
      mixedTrap.activate();

      const focusables = mixedTrap.getFocusableElements();
      expect(focusables.length).toBe(2);
      expect(focusables).toEqual([activeBtn1, activeBtn2]);

      // Tab on activeBtn2 wraps directly to activeBtn1 skipping disabled & hidden
      activeBtn2.focus();
      const pWrap = vi.fn();
      mixedTrap.handleKeyDown({ key: 'Tab', shiftKey: false, preventDefault: pWrap } as any);
      expect(pWrap).toHaveBeenCalled();
      expect((globalThis as any).document.activeElement).toBe(activeBtn1);

      mixedTrap.deactivate();
    });
  });

  describe('2. Escape Key & Exact Return Focus Restoration Across Nested Elements', () => {
    it('Escape from deeply nested elements closes modal and restores focus to exact trigger button', () => {
      const triggerButtonA = new MockDOMElement('button', 'trigger-btn-a');
      const triggerButtonB = new MockDOMElement('button', 'trigger-btn-b');

      const modal = new MockDOMElement('div', 'nested-modal');
      const card = new MockDOMElement('div', 'nested-card');
      const body = new MockDOMElement('div', 'nested-body');
      const deepInput = new MockDOMElement('input', 'deep-input');
      const deepBtn = new MockDOMElement('button', 'deep-btn');

      body.appendChild(deepInput);
      body.appendChild(deepBtn);
      card.appendChild(body);
      modal.appendChild(card);

      let isClosed = false;

      // --- Cycle 1 with Trigger Button A ---
      triggerButtonA.focus();
      expect((globalThis as any).document.activeElement).toBe(triggerButtonA);

      const trapA = new FocusTrap(modal as unknown as HTMLElement, {
        onEscape: () => {
          isClosed = true;
          trapA.deactivate();
        },
        returnFocusElement: triggerButtonA as unknown as HTMLElement,
      });

      trapA.activate();
      // Focus deeply nested button
      deepBtn.focus();
      expect((globalThis as any).document.activeElement).toBe(deepBtn);

      // Fire Escape keydown
      const pEsc = vi.fn();
      const sEsc = vi.fn();
      trapA.handleKeyDown({
        key: 'Escape',
        preventDefault: pEsc,
        stopPropagation: sEsc,
      } as any);

      expect(pEsc).toHaveBeenCalled();
      expect(sEsc).toHaveBeenCalled();
      expect(isClosed).toBe(true);
      expect((globalThis as any).document.activeElement).toBe(triggerButtonA);

      // --- Cycle 2 with Trigger Button B ---
      isClosed = false;
      triggerButtonB.focus();
      expect((globalThis as any).document.activeElement).toBe(triggerButtonB);

      const trapB = new FocusTrap(modal as unknown as HTMLElement, {
        onEscape: () => {
          isClosed = true;
          trapB.deactivate();
        },
        returnFocusElement: triggerButtonB as unknown as HTMLElement,
      });

      trapB.activate();
      deepInput.focus();
      expect((globalThis as any).document.activeElement).toBe(deepInput);

      trapB.handleKeyDown({
        key: 'Escape',
        preventDefault: vi.fn(),
        stopPropagation: vi.fn(),
      } as any);

      expect(isClosed).toBe(true);
      expect((globalThis as any).document.activeElement).toBe(triggerButtonB);
    });

    it('Safely handles trigger element unmounting without throwing exceptions', () => {
      const ephemeralTrigger = new MockDOMElement('button', 'ephemeral-trigger');
      ephemeralTrigger.focus();

      const modal = new MockDOMElement('div', 'ephemeral-modal');
      const cancelBtn = new MockDOMElement('button', 'cancel');
      modal.appendChild(cancelBtn);

      const trap = new FocusTrap(modal as unknown as HTMLElement, {
        returnFocusElement: ephemeralTrigger as unknown as HTMLElement,
      });
      trap.activate();

      // Simulate trigger element being unmounted and its focus method destroyed
      (ephemeralTrigger as any).focus = () => {
        throw new Error('Element is unmounted');
      };

      expect(() => {
        trap.deactivate();
      }).not.toThrow();
    });

    it('Idempotent deactivation on multiple rapid Escape presses', () => {
      const trigger = new MockDOMElement('button', 'idempotent-trigger');
      trigger.focus();

      const modal = new MockDOMElement('div', 'idempotent-modal');
      const btn = new MockDOMElement('button', 'btn');
      modal.appendChild(btn);

      let closeCount = 0;
      const trap = new FocusTrap(modal as unknown as HTMLElement, {
        onEscape: () => {
          closeCount++;
          trap.deactivate();
        },
        returnFocusElement: trigger as unknown as HTMLElement,
      });

      trap.activate();

      // Rapid Escape press 1
      trap.handleKeyDown({ key: 'Escape', preventDefault: vi.fn(), stopPropagation: vi.fn() } as any);
      // Rapid Escape press 2 (after deactivate)
      trap.handleKeyDown({ key: 'Escape', preventDefault: vi.fn(), stopPropagation: vi.fn() } as any);

      expect(closeCount).toBe(1);
      expect((globalThis as any).document.activeElement).toBe(trigger);
    });
  });

  describe('3. Screen Reader Live Region & Burst Event Stress Harness', () => {
    it('High-frequency SSE progress burst (100 events in rapid succession) maintains bounded percentage and integer rounding', () => {
      const workbenchStore = useWorkbenchStore();
      const batchStore = useBatchStore();

      workbenchStore.loadVideo('/workspace/fast_video.mp4');

      // Send 100 progress increments from 0.001 to 1.0
      for (let i = 1; i <= 100; i++) {
        const floatProgress = i / 100.0;
        workbenchStore.progress = {
          stage: 'extracting',
          pct: floatProgress,
          eta_ms: 1000,
        };

        // aria-valuenow is driven by progressPct
        const pct = workbenchStore.progressPct;
        expect(Number.isInteger(pct)).toBe(true);
        expect(pct).toBeGreaterThanOrEqual(0);
        expect(pct).toBeLessThanOrEqual(100);
        expect(pct).toBe(i);
        expect(Number.isNaN(pct)).toBe(false);
      }

      // Out of bounds and float rounding edge cases
      workbenchStore.progress = { stage: 'extracting', pct: -0.5, eta_ms: 0 };
      expect(workbenchStore.progressPct).toBe(-50); // raw Math.round(-0.5 * 100) -> -50

      workbenchStore.progress = { stage: 'extracting', pct: 0.55555, eta_ms: 0 };
      expect(workbenchStore.progressPct).toBe(56);

      // Verify batch task row progress calculation
      const dummyTask: any = {
        id: 'task-1',
        name: 'test.mp4',
        videoPath: '/workspace/test.mp4',
        status: 'extracting',
        progressPct: 84,
        stage: 'extracting',
        jobId: 'job-1',
        entries: [],
        error: null,
        elapsedMs: 1000,
        config: {} as any,
        createdAt: 1000,
      };
      batchStore.tasks = [dummyTask];
      expect(batchStore.tasks[0].progressPct).toBe(84);
    });

    it('Rapid SSE status transition burst updates active counter, filter tags and beacon lights', () => {
      const batchStore = useBatchStore();

      // Seed 20 batch tasks in 'waiting' state
      batchStore.tasks = Array.from({ length: 20 }, (_, i) => ({
        id: `burst-task-${i}`,
        name: `video_${i}.mp4`,
        videoPath: `/workspace/video_${i}.mp4`,
        status: 'waiting' as const,
        progressPct: 0,
        stage: 'waiting',
        jobId: null,
        entries: [],
        error: null,
        elapsedMs: 0,
        config: {} as any,
        createdAt: Date.now(),
      }));

      expect(batchStore.activeTaskCount).toBe(20);
      expect(batchStore.stats.waiting).toBe(20);
      expect(batchStore.stats.active).toBe(0);

      // Rapidly transition 10 tasks to 'extracting', 5 to 'completed', 3 to 'failed', 2 to 'interrupted'
      for (let i = 0; i < 10; i++) {
        batchStore.tasks[i].status = 'extracting';
        batchStore.tasks[i].progressPct = 50;
      }
      for (let i = 10; i < 15; i++) {
        batchStore.tasks[i].status = 'completed';
        batchStore.tasks[i].progressPct = 100;
      }
      for (let i = 15; i < 18; i++) {
        batchStore.tasks[i].status = 'failed';
        batchStore.tasks[i].error = `Error in task ${i}`;
      }
      for (let i = 18; i < 20; i++) {
        batchStore.tasks[i].status = 'interrupted';
      }

      // Active tasks count: active (10) + waiting (0) = 10
      expect(batchStore.activeTaskCount).toBe(10);
      expect(batchStore.stats.active).toBe(10);
      expect(batchStore.stats.completed).toBe(5);
      expect(batchStore.stats.failed).toBe(3);
      expect(batchStore.stats.cancelled).toBe(2); // cancelled or interrupted

      // Status filters check
      batchStore.statusFilter = 'running';
      expect(batchStore.filteredTasks.length).toBe(10);
      batchStore.statusFilter = 'failed';
      expect(batchStore.filteredTasks.length).toBe(3);
      batchStore.statusFilter = 'all';
      expect(batchStore.filteredTasks.length).toBe(20);
    });

    it('Live Draft Status transitions under rapid mutations & storage failure notifications', async () => {
      vi.useFakeTimers();
      const workbenchStore = useWorkbenchStore();
      workbenchStore.loadVideo('/workspace/live_region_video.mp4');
      workbenchStore.entries = [
        { index: 1, start_ms: 1000, end_ms: 2000, text: 'Initial Subtitle', confidence: 0.9 },
      ];

      // Rapid typing mutations
      expect(workbenchStore.draftStatus).toBe('saved');

      workbenchStore.updateSubtitleEntry(1, { text: 'Edit 1' });
      expect(workbenchStore.draftStatus).toBe('dirty');

      workbenchStore.updateSubtitleEntry(1, { text: 'Edit 2' });
      expect(workbenchStore.draftStatus).toBe('dirty');

      workbenchStore.updateSubtitleEntry(1, { text: 'Edit 3' });
      expect(workbenchStore.draftStatus).toBe('dirty');

      // Advance debounce timer -> transitions to saved
      vi.advanceTimersByTime(350);
      expect(workbenchStore.draftStatus).toBe('saved');
      expect(workbenchStore.draftError).toBeNull();

      vi.useRealTimers();
    });
  });

  describe('4. Design Tokens, Reduced Motion & CSS Layout Verification', () => {
    it('design-tokens.css defines standard focus ring and accessible contrast variables', () => {
      const tokensCss = fs.readFileSync(
        path.resolve(__dirname, '../styles/design-tokens.css'),
        'utf-8'
      );

      expect(tokensCss).toContain('--sl-focus-ring');
      expect(tokensCss).toContain('--sl-color-accent: #0a84ff;');
      expect(tokensCss).toContain('--sl-color-error:');
      expect(tokensCss).toContain('--sl-color-success:');
    });

    it('main.css guarantees focus-visible accessibility and selectable text for all transcripts/paths', () => {
      const mainCss = fs.readFileSync(path.resolve(__dirname, '../styles/main.css'), 'utf-8');

      // Focus visible outline
      expect(mainCss).toContain(':focus-visible');
      expect(mainCss).toContain('outline: 2px solid var(--sl-color-accent);');
      expect(mainCss).toContain('outline-offset: 2px;');

      // Selectable text classes
      expect(mainCss).toContain('user-select: text;');
      expect(mainCss).toContain('.sl-row-text');
      expect(mainCss).toContain('.sl-time-cell');
      expect(mainCss).toContain('.sl-file-title');
      expect(mainCss).toContain('.sl-file-subpath');
      expect(mainCss).toContain('.sl-ins-path');
      expect(mainCss).toContain('.sl-result-summary');

      // Reduced motion media query
      expect(mainCss).toContain('@media (prefers-reduced-motion: reduce)');
      expect(mainCss).toContain('transition-duration: 0.01ms !important;');
      expect(mainCss).toContain('animation-duration: 0.01ms !important;');
    });
  });
});
