import * as React from 'react';
import * as RadixDialog from '@radix-ui/react-dialog';
import { X } from 'lucide-react';
import { cn } from '../../lib/utils';

export const Dialog = RadixDialog.Root;
export const DialogTrigger = RadixDialog.Trigger;
export const DialogClose = RadixDialog.Close;

export function DialogContent({
  className,
  children,
  ...props
}: React.ComponentPropsWithoutRef<typeof RadixDialog.Content>) {
  return (
    <RadixDialog.Portal>
      <RadixDialog.Overlay className="fixed inset-0 z-50 bg-black/65" />
      <RadixDialog.Content
        className={cn(
          'fixed left-1/2 top-1/2 z-50 w-[calc(100%-2rem)] max-w-lg -translate-x-1/2 -translate-y-1/2 rounded-[var(--radius-lg)] border border-[var(--border-default)] bg-[var(--bg-elevated)] p-4 text-[var(--text-primary)] shadow-2xl focus:outline-none',
          className
        )}
        {...props}
      >
        {children}
        <RadixDialog.Close
          aria-label="关闭弹窗"
          className="absolute right-3 top-3 rounded-[var(--radius-sm)] p-1 text-[var(--text-secondary)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-primary)] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[var(--border-focus)]"
        >
          <X className="h-4 w-4" strokeWidth={1.75} />
        </RadixDialog.Close>
      </RadixDialog.Content>
    </RadixDialog.Portal>
  );
}

export function DialogHeader({
  className,
  ...props
}: React.HTMLAttributes<HTMLDivElement>) {
  return <div className={cn('space-y-1 pr-6', className)} {...props} />;
}

export function DialogTitle({
  className,
  ...props
}: React.ComponentPropsWithoutRef<typeof RadixDialog.Title>) {
  return (
    <RadixDialog.Title
      className={cn('text-sm font-semibold text-[var(--text-primary)]', className)}
      {...props}
    />
  );
}

export function DialogDescription({
  className,
  ...props
}: React.ComponentPropsWithoutRef<typeof RadixDialog.Description>) {
  return (
    <RadixDialog.Description
      className={cn('text-xs text-[var(--text-secondary)]', className)}
      {...props}
    />
  );
}

export function DialogFooter({
  className,
  ...props
}: React.HTMLAttributes<HTMLDivElement>) {
  return (
    <div
      className={cn('mt-4 flex items-center justify-end gap-2', className)}
      {...props}
    />
  );
}
