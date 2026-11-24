import * as React from 'react';
import { cva, type VariantProps } from 'class-variance-authority';
import { cn } from '../../lib/utils';

const buttonVariants = cva(
  'inline-flex items-center justify-center gap-1.5 whitespace-nowrap rounded-[var(--radius-md)] text-xs font-medium transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[var(--border-focus)] disabled:pointer-events-none disabled:opacity-45 cursor-pointer select-none',
  {
    variants: {
      variant: {
        primary:
          'bg-[var(--primary)] text-[var(--primary-foreground)] hover:bg-[var(--primary-hover)]',
        secondary:
          'bg-[var(--bg-elevated)] text-[var(--text-primary)] border border-[var(--border-default)] hover:bg-[var(--bg-hover)]',
        outline:
          'border border-[var(--border-input)] bg-transparent text-[var(--text-primary)] hover:bg-[var(--bg-hover)]',
        ghost:
          'text-[var(--text-secondary)] hover:bg-[var(--bg-hover)] hover:text-[var(--text-primary)]',
        danger:
          'bg-[var(--tint-failed)] text-[var(--status-failed)] border border-[var(--status-failed)]/40 hover:bg-[var(--status-failed)]/20',
      },
      size: {
        xs: 'h-6 px-2 text-[11px]',
        sm: 'h-7 px-2.5 text-xs',
        md: 'h-8 px-3 text-xs',
        icon: 'h-7 w-7 p-0',
      },
    },
    defaultVariants: {
      variant: 'secondary',
      size: 'sm',
    },
  }
);

export interface ButtonProps
  extends React.ButtonHTMLAttributes<HTMLButtonElement>,
    VariantProps<typeof buttonVariants> {}

export const Button = React.forwardRef<HTMLButtonElement, ButtonProps>(
  ({ className, variant, size, type = 'button', ...props }, ref) => (
    <button
      ref={ref}
      type={type}
      className={cn(buttonVariants({ variant, size }), className)}
      {...props}
    />
  )
);
Button.displayName = 'Button';
