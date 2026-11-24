import { useEffect, useState } from 'react';
import { AlertTriangle, CheckCircle2, Info, X, XCircle } from 'lucide-react';
import { useWorkbenchStore } from '../../../store';
import type { ToastItem } from '../../../store';
import { Button } from './button';

function resolveToastDurationMs(toast: ToastItem): number {
  if (toast.autoDismissMs !== undefined) {
    return toast.autoDismissMs;
  }
  switch (toast.severity) {
    case 'error':
      return 5000;
    case 'warning':
      return 4500;
    case 'success':
    case 'info':
    default:
      return 3500;
  }
}

interface ToastCardProps {
  toast: ToastItem;
  onDismiss: (id: string) => void;
  onOpenVlmSettings: () => void;
  onRetryActiveTask: () => void;
}

function ToastCard({
  toast,
  onDismiss,
  onOpenVlmSettings,
  onRetryActiveTask,
}: ToastCardProps) {
  const [paused, setPaused] = useState(false);

  useEffect(() => {
    const durationMs = resolveToastDurationMs(toast);
    if (paused || durationMs <= 0) {
      return;
    }
    const timer = setTimeout(() => {
      onDismiss(toast.id);
    }, durationMs);
    return () => clearTimeout(timer);
  }, [toast, paused, onDismiss]);

  const icon =
    toast.severity === 'error' ? (
      <XCircle className="h-4 w-4 shrink-0 text-[var(--status-failed)]" strokeWidth={1.75} />
    ) : toast.severity === 'warning' ? (
      <AlertTriangle className="h-4 w-4 shrink-0 text-[var(--status-warning)]" strokeWidth={1.75} />
    ) : toast.severity === 'success' ? (
      <CheckCircle2 className="h-4 w-4 shrink-0 text-[var(--status-exported)]" strokeWidth={1.75} />
    ) : (
      <Info className="h-4 w-4 shrink-0 text-[var(--status-ready)]" strokeWidth={1.75} />
    );

  return (
    <div
      role="status"
      onMouseEnter={() => setPaused(true)}
      onMouseLeave={() => setPaused(false)}
      className="pointer-events-auto rounded-[var(--radius-md)] border border-[var(--border-default)] bg-[var(--bg-elevated)] p-2.5 text-xs text-[var(--text-primary)] shadow-lg"
    >
      <div className="flex items-start gap-2">
        {icon}
        <div className="min-w-0 flex-1 space-y-0.5">
          <p className="font-medium leading-4">{toast.title}</p>
          {toast.reason && (
            <p className="break-words text-[11px] text-[var(--text-secondary)]">
              {toast.reason}
            </p>
          )}
          {toast.nextStep && (
            <p className="break-words text-[11px] text-[var(--text-muted)]">
              {toast.nextStep}
            </p>
          )}
          {toast.actionLabel && toast.actionType && (
            <div className="pt-1">
              <Button
                size="xs"
                variant="outline"
                onClick={() => {
                  if (toast.actionType === 'open_vlm_settings') {
                    onOpenVlmSettings();
                  } else if (toast.actionType === 'retry_active_task') {
                    onRetryActiveTask();
                  }
                  onDismiss(toast.id);
                }}
              >
                {toast.actionLabel}
              </Button>
            </div>
          )}
        </div>
        <button
          type="button"
          aria-label="关闭通知"
          onClick={() => onDismiss(toast.id)}
          className="rounded-[var(--radius-xs)] p-0.5 text-[var(--text-muted)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-primary)]"
        >
          <X className="h-3.5 w-3.5" strokeWidth={1.75} />
        </button>
      </div>
    </div>
  );
}

export function ToastViewport() {
  const toasts = useWorkbenchStore((s) => s.toasts);
  const dismissToast = useWorkbenchStore((s) => s.dismissToast);
  const setVlmSettingsOpen = useWorkbenchStore((s) => s.setVlmSettingsOpen);
  const startTaskExtraction = useWorkbenchStore((s) => s.startTaskExtraction);

  if (toasts.length === 0) {
    return null;
  }

  return (
    <div
      aria-label="通知消息"
      className="pointer-events-none fixed bottom-3 right-3 z-50 flex w-80 max-w-[calc(100vw-1.5rem)] flex-col gap-2"
    >
      {toasts.map((toast) => (
        <ToastCard
          key={toast.id}
          toast={toast}
          onDismiss={dismissToast}
          onOpenVlmSettings={() => setVlmSettingsOpen(true)}
          onRetryActiveTask={() => void startTaskExtraction()}
        />
      ))}
    </div>
  );
}
