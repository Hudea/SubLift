import * as React from 'react';
import * as RadixSelect from '@radix-ui/react-select';
import { Check, ChevronDown } from 'lucide-react';
import { cn } from '../../lib/utils';

export const Select = RadixSelect.Root;
export const SelectValue = RadixSelect.Value;

export function SelectTrigger({
  className,
  children,
  ...props
}: React.ComponentPropsWithoutRef<typeof RadixSelect.Trigger>) {
  return (
    <RadixSelect.Trigger
      className={cn(
        'inline-flex h-7 items-center justify-between gap-1.5 rounded-[var(--radius-md)] border border-[var(--border-input)] bg-[var(--bg-panel)] px-2 text-xs text-[var(--text-primary)] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[var(--border-focus)] disabled:opacity-45',
        className
      )}
      {...props}
    >
      {children}
      <RadixSelect.Icon asChild>
        <ChevronDown className="h-3.5 w-3.5 text-[var(--text-secondary)]" strokeWidth={1.75} />
      </RadixSelect.Icon>
    </RadixSelect.Trigger>
  );
}

export function SelectContent({
  className,
  children,
  ...props
}: React.ComponentPropsWithoutRef<typeof RadixSelect.Content>) {
  return (
    <RadixSelect.Portal>
      <RadixSelect.Content
        position="popper"
        sideOffset={4}
        className={cn(
          'z-50 min-w-[8rem] overflow-hidden rounded-[var(--radius-md)] border border-[var(--border-default)] bg-[var(--bg-elevated)] p-1 text-[var(--text-primary)] shadow-lg',
          className
        )}
        {...props}
      >
        <RadixSelect.Viewport>{children}</RadixSelect.Viewport>
      </RadixSelect.Content>
    </RadixSelect.Portal>
  );
}

export function SelectItem({
  className,
  children,
  ...props
}: React.ComponentPropsWithoutRef<typeof RadixSelect.Item>) {
  return (
    <RadixSelect.Item
      className={cn(
        'relative flex cursor-pointer select-none items-center rounded-[var(--radius-sm)] py-1 pl-6 pr-2 text-xs text-[var(--text-primary)] outline-none data-[highlighted]:bg-[var(--bg-hover)] data-[disabled]:opacity-45',
        className
      )}
      {...props}
    >
      <span className="absolute left-1.5 flex h-3.5 w-3.5 items-center justify-center">
        <RadixSelect.ItemIndicator>
          <Check className="h-3 w-3 text-[var(--sl-highlight)]" strokeWidth={2} />
        </RadixSelect.ItemIndicator>
      </span>
      <RadixSelect.ItemText>{children}</RadixSelect.ItemText>
    </RadixSelect.Item>
  );
}
