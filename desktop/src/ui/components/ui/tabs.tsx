import * as React from 'react';
import * as RadixTabs from '@radix-ui/react-tabs';
import { cn } from '../../lib/utils';

export const Tabs = RadixTabs.Root;

export function TabsList({
  className,
  ...props
}: React.ComponentPropsWithoutRef<typeof RadixTabs.List>) {
  return (
    <RadixTabs.List
      className={cn(
        'inline-flex items-center gap-1 rounded-[var(--radius-md)] border border-[var(--border-subtle)] bg-[var(--bg-panel)] p-0.5',
        className
      )}
      {...props}
    />
  );
}

export function TabsTrigger({
  className,
  ...props
}: React.ComponentPropsWithoutRef<typeof RadixTabs.Trigger>) {
  return (
    <RadixTabs.Trigger
      className={cn(
        'inline-flex items-center justify-center rounded-[var(--radius-sm)] px-2.5 py-1 text-xs font-medium text-[var(--text-secondary)] transition-colors hover:text-[var(--text-primary)] data-[state=active]:bg-[var(--bg-active)] data-[state=active]:text-[var(--text-primary)] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[var(--border-focus)]',
        className
      )}
      {...props}
    />
  );
}

export function TabsContent({
  className,
  ...props
}: React.ComponentPropsWithoutRef<typeof RadixTabs.Content>) {
  return (
    <RadixTabs.Content
      className={cn('focus-visible:outline-none', className)}
      {...props}
    />
  );
}
