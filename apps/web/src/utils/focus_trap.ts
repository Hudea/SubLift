/**
 * Focus Trap Utility for Accessible Modal Dialogs (WCAG 2.1 / WAI-ARIA Authoring Practices)
 */

const FOCUSABLE_SELECTOR = [
  'a[href]',
  'button:not([disabled])',
  'input:not([disabled]):not([type="hidden"])',
  'select:not([disabled])',
  'textarea:not([disabled])',
  '[tabindex]:not([tabindex="-1"])',
].join(', ');

export interface FocusTrapOptions {
  /** Initial element or selector to focus when trap is activated */
  initialFocus?: HTMLElement | string | null;
  /** Callback triggered when Escape key is pressed */
  onEscape?: () => void;
  /** Element to return focus to when trap is deactivated */
  returnFocusElement?: HTMLElement | null;
}

export class FocusTrap {
  private container: HTMLElement;
  private options: FocusTrapOptions;
  private active = false;
  private previousActiveElement: HTMLElement | null = null;
  private boundKeyDownHandler: (e: KeyboardEvent) => void;

  constructor(container: HTMLElement, options: FocusTrapOptions = {}) {
    this.container = container;
    this.options = options;
    this.boundKeyDownHandler = this.handleKeyDown.bind(this);
  }

  public activate(): void {
    if (this.active) return;
    this.active = true;

    // Save current active element for restoration
    this.previousActiveElement =
      this.options.returnFocusElement ||
      (document.activeElement instanceof HTMLElement ? document.activeElement : null);

    // Attach key listener
    document.addEventListener('keydown', this.boundKeyDownHandler, true);

    // Initial focus
    this.focusInitial();
  }

  public deactivate(): void {
    if (!this.active) return;
    this.active = false;

    document.removeEventListener('keydown', this.boundKeyDownHandler, true);

    // Restore focus
    if (this.previousActiveElement && typeof this.previousActiveElement.focus === 'function') {
      try {
        this.previousActiveElement.focus();
      } catch {
        // Ignore errors if element was unmounted
      }
    }
  }

  public getFocusableElements(): HTMLElement[] {
    const elements = Array.from(
      this.container.querySelectorAll<HTMLElement>(FOCUSABLE_SELECTOR)
    );
    return elements.filter((el) => {
      // Check visible and not disabled
      const style = window.getComputedStyle ? window.getComputedStyle(el) : null;
      if (style && (style.display === 'none' || style.visibility === 'hidden')) {
        return false;
      }
      return el.offsetParent !== null || el.offsetWidth > 0 || el.offsetHeight > 0 || el.tabIndex >= 0;
    });
  }

  private focusInitial(): void {
    if (this.options.initialFocus) {
      const target =
        typeof this.options.initialFocus === 'string'
          ? this.container.querySelector<HTMLElement>(this.options.initialFocus)
          : this.options.initialFocus;
      if (target && typeof target.focus === 'function') {
        target.focus();
        return;
      }
    }

    const focusables = this.getFocusableElements();
    if (focusables.length > 0 && typeof focusables[0].focus === 'function') {
      focusables[0].focus();
    } else {
      this.container.focus?.();
    }
  }

  public handleKeyDown(e: KeyboardEvent): void {
    if (!this.active) return;

    if (e.key === 'Escape') {
      if (this.options.onEscape) {
        e.preventDefault();
        e.stopPropagation();
        this.options.onEscape();
      }
      return;
    }

    if (e.key !== 'Tab') return;

    const focusables = this.getFocusableElements();
    if (focusables.length === 0) {
      e.preventDefault();
      return;
    }

    const first = focusables[0];
    const last = focusables[focusables.length - 1];
    const active = document.activeElement;

    if (e.shiftKey) {
      // Shift + Tab: if on first element or outside modal, cycle to last
      if (active === first || !this.container.contains(active)) {
        e.preventDefault();
        last.focus();
      }
    } else {
      // Tab: if on last element or outside modal, cycle to first
      if (active === last || !this.container.contains(active)) {
        e.preventDefault();
        first.focus();
      }
    }
  }
}
