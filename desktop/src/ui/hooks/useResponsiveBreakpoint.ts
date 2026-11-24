import { useEffect, useState } from 'react';

export type WorkbenchBreakpoint = 'wide' | 'compact' | 'drawer' | 'stacked';

export function computeWorkbenchBreakpoint(viewportWidth: number): WorkbenchBreakpoint {
  if (viewportWidth >= 1360) {
    return 'wide';
  }
  if (viewportWidth >= 1160) {
    return 'compact';
  }
  if (viewportWidth >= 960) {
    return 'drawer';
  }
  return 'stacked';
}

export function useResponsiveBreakpoint(): WorkbenchBreakpoint {
  const [breakpoint, setBreakpoint] = useState<WorkbenchBreakpoint>(() => {
    if (typeof window === 'undefined') {
      return 'wide';
    }
    return computeWorkbenchBreakpoint(window.innerWidth);
  });

  useEffect(() => {
    if (typeof window === 'undefined') {
      return;
    }
    const update = () => {
      setBreakpoint(computeWorkbenchBreakpoint(window.innerWidth));
    };
    update();
    window.addEventListener('resize', update);
    return () => window.removeEventListener('resize', update);
  }, []);

  return breakpoint;
}
