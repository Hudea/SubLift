import * as React from 'react';
import * as RadixTooltip from '@radix-ui/react-tooltip';
import { cn } from '../../lib/utils';

export const TooltipProvider = RadixTooltip.Provider;
export const Tooltip = RadixTooltip.Root;
export const TooltipTrigger = RadixTooltip.Trigger;

export function TooltipContent({
  className,
  sideOffset = 6,
  ...props
}: React.ComponentPropsWithoutRef<typeof RadixTooltip.Content>) {
  return (
    <RadixTooltip.Portal>
      <RadixTooltip.Content
        sideOffset={sideOffset}
        className={cn(
          'z-50 max-w-xs rounded-[var(--radius-sm)] border border-[var(--border-default)] bg-[var(--bg-elevated)] px-2 py-1 text-[11px] text-[var(--text-primary)] shadow-md',
          className
        )}
        {...props}
      />
    </RadixTooltip.Portal>
  );
}
