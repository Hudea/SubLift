import * as React from 'react';
import * as RadixCheckbox from '@radix-ui/react-checkbox';
import { Check } from 'lucide-react';
import { cn } from '../../lib/utils';

export function Checkbox({
  className,
  ...props
}: React.ComponentPropsWithoutRef<typeof RadixCheckbox.Root>) {
  return (
    <RadixCheckbox.Root
      className={cn(
        'inline-flex h-4 w-4 shrink-0 items-center justify-center rounded-[var(--radius-xs)] border border-[var(--border-input)] bg-[var(--bg-panel)] text-[var(--primary-foreground)] transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[var(--border-focus)] data-[state=checked]:border-[var(--primary)] data-[state=checked]:bg-[var(--primary)] disabled:cursor-not-allowed disabled:opacity-45',
        className
      )}
      {...props}
    >
      <RadixCheckbox.Indicator className="flex items-center justify-center">
        <Check className="h-3 w-3" strokeWidth={2.5} />
      </RadixCheckbox.Indicator>
    </RadixCheckbox.Root>
  );
}
