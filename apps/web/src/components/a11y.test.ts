import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { FocusTrap } from '../utils/focus_trap';
import fs from 'fs';
import path from 'path';

// Lightweight Mock DOM for headless Node test environment
class MockHTMLElement {
  public id: string;
  public tagName: string;
  public children: MockHTMLElement[] = [];
  public parentElement: MockHTMLElement | null = null;
  public tabIndex = 0;
  public disabled = false;
  public type = '';
  public offsetWidth = 100;
  public offsetHeight = 30;
  public offsetParent: any = {};
  public isFocused = false;

  constructor(tagName: string, id = '') {
    this.tagName = tagName.toUpperCase();
    this.id = id;
  }

  public appendChild(child: MockHTMLElement): MockHTMLElement {
    child.parentElement = this;
    this.children.push(child);
    return child;
  }

  public focus(): void {
    const doc = (globalThis as any).document;
    if (doc) {
      if (doc.activeElement) {
        doc.activeElement.isFocused = false;
      }
      doc.activeElement = this;
      this.isFocused = true;
    }
  }

  public contains(target: any): boolean {
    if (target === this) return true;
    for (const child of this.children) {
      if (child.contains(target)) return true;
    }
    return false;
  }

  public querySelectorAll<T = MockHTMLElement>(_selector: string): T[] {
    const results: MockHTMLElement[] = [];
    function collect(el: MockHTMLElement) {
      for (const c of el.children) {
        if (['BUTTON', 'INPUT', 'SELECT', 'TEXTAREA', 'A'].includes(c.tagName) && !c.disabled) {
          results.push(c);
        }
        collect(c);
      }
    }
    collect(this);
    return results as unknown as T[];
  }

  public querySelector<T = MockHTMLElement>(selector: string): T | null {
    const list = this.querySelectorAll<T>(selector);
    if (selector.startsWith('#')) {
      const targetId = selector.substring(1);
      const found = (list as unknown as MockHTMLElement[]).find((el) => el.id === targetId);
      return (found as unknown as T) || null;
    }
    return list.length > 0 ? list[0] : null;
  }
}

describe('Feature 12516: Web Accessibility (a11y) Verification', () => {
  describe('1. Focus Trapping & Modal Dialog Semantics', () => {
    let container: any;
    let inputEl: any;
    let button1: any;
    let button2: any;
    let triggerBtn: any;
    let eventListeners: Record<string, Function[]> = {};

    beforeEach(() => {
      eventListeners = {};

      const mockDoc = {
        activeElement: null as any,
        addEventListener: (event: string, handler: Function) => {
          eventListeners[event] = eventListeners[event] || [];
          eventListeners[event].push(handler);
        },
        removeEventListener: (event: string, handler: Function) => {
          if (eventListeners[event]) {
            eventListeners[event] = eventListeners[event].filter((h) => h !== handler);
          }
        },
      };

      (globalThis as any).document = mockDoc;
      (globalThis as any).HTMLElement = MockHTMLElement;
      (globalThis as any).window = {
        getComputedStyle: () => ({ display: 'block', visibility: 'visible' }),
      };

      triggerBtn = new MockHTMLElement('button', 'trigger-btn');
      triggerBtn.focus();

      container = new MockHTMLElement('div', 'modal-container');
      inputEl = new MockHTMLElement('input', 'modal-input');
      button1 = new MockHTMLElement('button', 'modal-cancel');
      button2 = new MockHTMLElement('button', 'modal-confirm');

      container.appendChild(inputEl);
      container.appendChild(button1);
      container.appendChild(button2);
    });

    afterEach(() => {
      delete (globalThis as any).document;
      delete (globalThis as any).HTMLElement;
      delete (globalThis as any).window;
    });

    it('FocusTrap 激活时正确记录触发元素并初始聚焦首个可聚焦元素', () => {
      const trap = new FocusTrap(container as unknown as HTMLElement, {
        returnFocusElement: triggerBtn as unknown as HTMLElement,
      });
      trap.activate();

      const focusables = trap.getFocusableElements();
      expect(focusables.length).toBe(3);
      expect((globalThis as any).document.activeElement).toBe(inputEl);

      trap.deactivate();
    });

    it('FocusTrap 支持指定 initialFocus 选择器', () => {
      const trap = new FocusTrap(container as unknown as HTMLElement, {
        initialFocus: '#modal-confirm',
        returnFocusElement: triggerBtn as unknown as HTMLElement,
      });
      trap.activate();

      expect((globalThis as any).document.activeElement).toBe(button2);

      trap.deactivate();
    });

    it('FocusTrap 在最后一个元素按 Tab 键循环聚焦到第一个元素', () => {
      const trap = new FocusTrap(container as unknown as HTMLElement);
      trap.activate();

      // Focus last element
      button2.focus();
      expect((globalThis as any).document.activeElement).toBe(button2);

      // Trigger Tab keydown
      const preventDefaultMock = vi.fn();
      const tabEvent = {
        key: 'Tab',
        shiftKey: false,
        preventDefault: preventDefaultMock,
      } as unknown as KeyboardEvent;

      trap.handleKeyDown(tabEvent);

      expect(preventDefaultMock).toHaveBeenCalled();
      expect((globalThis as any).document.activeElement).toBe(inputEl);

      trap.deactivate();
    });

    it('FocusTrap 在第一个元素按 Shift+Tab 键循环聚焦到最后一个元素', () => {
      const trap = new FocusTrap(container as unknown as HTMLElement);
      trap.activate();

      // Focus first element
      inputEl.focus();
      expect((globalThis as any).document.activeElement).toBe(inputEl);

      // Trigger Shift + Tab keydown
      const preventDefaultMock = vi.fn();
      const shiftTabEvent = {
        key: 'Tab',
        shiftKey: true,
        preventDefault: preventDefaultMock,
      } as unknown as KeyboardEvent;

      trap.handleKeyDown(shiftTabEvent);

      expect(preventDefaultMock).toHaveBeenCalled();
      expect((globalThis as any).document.activeElement).toBe(button2);

      trap.deactivate();
    });

    it('FocusTrap 按 Escape 键触发 onEscape 回调', () => {
      const onEscape = vi.fn();
      const trap = new FocusTrap(container as unknown as HTMLElement, { onEscape });
      trap.activate();

      const preventDefaultMock = vi.fn();
      const stopPropagationMock = vi.fn();
      const escEvent = {
        key: 'Escape',
        preventDefault: preventDefaultMock,
        stopPropagation: stopPropagationMock,
      } as unknown as KeyboardEvent;

      trap.handleKeyDown(escEvent);
      expect(onEscape).toHaveBeenCalledTimes(1);

      trap.deactivate();
    });

    it('FocusTrap 停用时正确恢复焦点至触发元素', () => {
      const trap = new FocusTrap(container as unknown as HTMLElement, {
        returnFocusElement: triggerBtn as unknown as HTMLElement,
      });
      trap.activate();

      button1.focus();
      expect((globalThis as any).document.activeElement).toBe(button1);

      trap.deactivate();
      expect((globalThis as any).document.activeElement).toBe(triggerBtn);
    });
  });

  describe('2. Modal Dialog ARIA Attributes in Vue Components', () => {
    it('Navbar.vue 工作区切换模态弹窗具备完整的 role=dialog 与 aria-modal/aria-labelledby', () => {
      const content = fs.readFileSync(path.resolve(__dirname, 'Navbar.vue'), 'utf-8');

      expect(content).toContain('role="dialog"');
      expect(content).toContain('aria-modal="true"');
      expect(content).toContain('aria-labelledby="switch-ws-title"');
      expect(content).toContain('aria-describedby="switch-ws-desc"');
      expect(content).toContain('id="switch-ws-title"');
      expect(content).toContain('id="switch-ws-desc"');
      expect(content).toContain('aria-label="关闭工作区切换弹窗"');
      expect(content).toContain('aria-label="视频工作区文件夹路径"');
    });

    it('WorkbenchView.vue 重新提取确认弹窗具备 role=alertdialog 与 aria-modal/aria-labelledby', () => {
      const content = fs.readFileSync(path.resolve(__dirname, '../views/WorkbenchView.vue'), 'utf-8');

      expect(content).toContain('role="alertdialog"');
      expect(content).toContain('aria-modal="true"');
      expect(content).toContain('aria-labelledby="reextract-title"');
      expect(content).toContain('aria-describedby="reextract-desc"');
      expect(content).toContain('id="reextract-title"');
      expect(content).toContain('id="reextract-desc"');
      expect(content).toContain('aria-label="取消重新提取"');
      expect(content).toContain('aria-label="确认重新提取"');
    });

    it('TaskCenterView.vue 批量落盘模态弹窗具备 role=dialog 与 aria-modal/aria-labelledby', () => {
      const content = fs.readFileSync(path.resolve(__dirname, '../views/TaskCenterView.vue'), 'utf-8');

      expect(content).toContain('role="dialog"');
      expect(content).toContain('aria-modal="true"');
      expect(content).toContain('aria-labelledby="batch-save-title"');
      expect(content).toContain('aria-describedby="batch-save-desc"');
      expect(content).toContain('id="batch-save-title"');
      expect(content).toContain('id="batch-save-desc"');
      expect(content).toContain('aria-label="关闭批量保存弹窗"');
      expect(content).toContain('role="radiogroup"');
    });
  });

  describe('3. Accessible Live Regions & Progress Indicators', () => {
    it('App.vue 加载屏具备 role=status 和 aria-live=polite', () => {
      const content = fs.readFileSync(path.resolve(__dirname, '../App.vue'), 'utf-8');

      expect(content).toContain('role="status"');
      expect(content).toContain('aria-live="polite"');
    });

    it('WorkbenchView.vue 提取进度条具备 role=progressbar 与 aria-valuenow 语义', () => {
      const content = fs.readFileSync(path.resolve(__dirname, '../views/WorkbenchView.vue'), 'utf-8');

      expect(content).toContain('role="progressbar"');
      expect(content).toContain('aria-label="视频字幕提取进度"');
      expect(content).toContain(':aria-valuenow="workbenchStore.progressPct"');
      expect(content).toContain('aria-valuemin="0"');
      expect(content).toContain('aria-valuemax="100"');
    });

    it('LiveTranscript.vue 草稿状态指示器具备 role=status 与 aria-live=polite', () => {
      const content = fs.readFileSync(path.resolve(__dirname, 'LiveTranscript.vue'), 'utf-8');

      expect(content).toContain('class="sl-draft-status-badge"');
      expect(content).toContain('role="status"');
      expect(content).toContain('aria-live="polite"');
      expect(content).toContain('class="sl-live-tag"');
    });

    it('BatchTaskRow.vue 进度条与状态指示灯具备可访问性属性', () => {
      const content = fs.readFileSync(path.resolve(__dirname, 'BatchTaskRow.vue'), 'utf-8');

      expect(content).toContain('role="progressbar"');
      expect(content).toContain(':aria-valuenow="task.progressPct"');
      expect(content).toContain('class="sl-beacon-light"');
      expect(content).toContain(':aria-label="`状态指示: ${getStageLabel(task)}`"');
    });

    it('BatchTaskInspector.vue 状态与警示区域具备 role=status / role=alert', () => {
      const content = fs.readFileSync(path.resolve(__dirname, 'BatchTaskInspector.vue'), 'utf-8');

      expect(content).toContain('role="status"');
      expect(content).toContain('class="sl-inspector-warning-box" role="alert"');
      expect(content).toContain('class="sl-inspector-error-box" role="alert"');
    });

    it('BatchStatsCards.vue 统计选项卡具备 role=tablist 与 role=tab / aria-selected', () => {
      const content = fs.readFileSync(path.resolve(__dirname, 'BatchStatsCards.vue'), 'utf-8');

      expect(content).toContain('role="tablist"');
      expect(content).toContain('role="tab"');
      expect(content).toContain(':aria-selected="batchStore.statusFilter === \'all\'"');
      expect(content).toContain(':aria-selected="batchStore.statusFilter === \'running\'"');
    });
  });

  describe('4. Accessible Form Inputs & Interactive Controls', () => {
    it('VideoPlayer.vue 播放时间轴具备 role=slider 与 aria-valuetext', () => {
      const content = fs.readFileSync(path.resolve(__dirname, 'VideoPlayer.vue'), 'utf-8');

      expect(content).toContain('role="slider"');
      expect(content).toContain('aria-label="视频时间轴进度"');
      expect(content).toContain('aria-valuemin="0"');
      expect(content).toContain(':aria-valuenow="currentTime"');
      expect(content).toContain(':aria-valuetext');
      expect(content).toContain('role="timer"');
    });

    it('WorkspaceSetupView.vue 具备关联标签与 aria-required', () => {
      const content = fs.readFileSync(path.resolve(__dirname, '../views/WorkspaceSetupView.vue'), 'utf-8');

      expect(content).toContain('for="workspace-path-input"');
      expect(content).toContain('id="workspace-path-input"');
      expect(content).toContain('aria-required="true"');
      expect(content).toContain('aria-label="视频工作区文件夹路径"');
    });

    it('DropZone.vue 单视频输入框具备完整标签与快捷提示', () => {
      const content = fs.readFileSync(path.resolve(__dirname, 'DropZone.vue'), 'utf-8');

      expect(content).toContain('for="manual-video-path-input"');
      expect(content).toContain('id="manual-video-path-input"');
      expect(content).toContain('aria-label="视频物理绝对路径"');
    });
  });

  describe('5. Selectable Text (Elimination of Global user-select: none)', () => {
    it('main.css 中移除了全局 html, body 的 user-select: none', () => {
      const css = fs.readFileSync(path.resolve(__dirname, '../styles/main.css'), 'utf-8');

      // html, body block must not contain user-select: none
      const htmlBodyMatch = css.match(/html,\s*body\s*\{([^}]+)\}/);
      expect(htmlBodyMatch).not.toBeNull();
      expect(htmlBodyMatch![1]).not.toContain('user-select: none');
    });

    it('main.css 明确配置正文、字幕、时间码、路径等为 user-select: text', () => {
      const css = fs.readFileSync(path.resolve(__dirname, '../styles/main.css'), 'utf-8');

      expect(css).toContain('user-select: text');
      expect(css).toContain('.sl-row-text');
      expect(css).toContain('.sl-time-cell');
      expect(css).toContain('.sl-file-title');
      expect(css).toContain('.sl-ins-path');
    });
  });

  describe('6. Motion Sensitivity (prefers-reduced-motion)', () => {
    it('main.css 包含 @media (prefers-reduced-motion: reduce) 降级规则', () => {
      const css = fs.readFileSync(path.resolve(__dirname, '../styles/main.css'), 'utf-8');

      expect(css).toContain('@media (prefers-reduced-motion: reduce)');
      expect(css).toContain('animation-duration: 0.01ms !important');
      expect(css).toContain('animation: none !important');
    });

    it('RoiOverlay.vue 在 prefers-reduced-motion 下直接应用选框位置而不执行 rAF 补间', () => {
      const content = fs.readFileSync(path.resolve(__dirname, 'RoiOverlay.vue'), 'utf-8');

      expect(content).toContain('prefers-reduced-motion');
      expect(content).toContain('localBox.x = target.x');
    });
  });
});
