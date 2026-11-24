import * as React from 'react';
import * as RadixSlider from '@radix-ui/react-slider';
import { cn } from '../../lib/utils';

export function Slider({
  className,
  ...props
}: React.ComponentPropsWithoutRef<typeof RadixSlider.Root>) {
  return (
    <RadixSlider.Root
      className={cn(
        'relative flex w-full touch-none select-none items-center py-1',
        className
      )}
      {...props}
    >
      <RadixSlider.Track className="relative h-1.5 w-full grow overflow-hidden rounded-full bg-[var(--border-default)]">
        <RadixSlider.Range className="absolute h-full bg-[var(--sl-highlight)]" />
      </RadixSlider.Track>
      <RadixSlider.Thumb className="block h-3.5 w-3.5 rounded-full border border-[var(--sl-highlight)] bg-[var(--text-primary)] shadow focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[var(--border-focus)]" />
    </RadixSlider.Root>
  );
}
