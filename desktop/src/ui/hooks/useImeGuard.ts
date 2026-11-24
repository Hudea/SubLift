import { useCallback, useEffect, useRef } from 'react';

export interface ImeCompositionState {
  isComposingRef: React.RefObject<boolean>;
  justEndedCompositionRef: React.RefObject<boolean>;
  onCompositionStart: () => void;
  onCompositionEnd: () => void;
  isImeActive: (event: {
    isComposing?: boolean;
    keyCode?: number;
    nativeEvent?: { isComposing?: boolean; keyCode?: number };
  }) => boolean;
}

/**
 * macOS WKWebView 中文/日文输入法防误触守卫：
 * 在 compositionstart/end 及 compositionend 后约一帧（30ms）内拦截裸 Enter 提交。
 */
export function useImeGuard(): ImeCompositionState {
  const isComposingRef = useRef(false);
  const justEndedCompositionRef = useRef(false);
  const clearTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => {
    return () => {
      if (clearTimerRef.current !== null) {
        clearTimeout(clearTimerRef.current);
      }
    };
  }, []);

  const onCompositionStart = useCallback(() => {
    isComposingRef.current = true;
    justEndedCompositionRef.current = false;
    if (clearTimerRef.current !== null) {
      clearTimeout(clearTimerRef.current);
      clearTimerRef.current = null;
    }
  }, []);

  const onCompositionEnd = useCallback(() => {
    isComposingRef.current = false;
    justEndedCompositionRef.current = true;
    if (clearTimerRef.current !== null) {
      clearTimeout(clearTimerRef.current);
    }
    const reset = () => {
      justEndedCompositionRef.current = false;
    };
    if (typeof requestAnimationFrame === 'function') {
      requestAnimationFrame(() => {
        clearTimerRef.current = setTimeout(reset, 30);
      });
    } else {
      clearTimerRef.current = setTimeout(reset, 30);
    }
  }, []);

  const isImeActive = useCallback(
    (event: {
      isComposing?: boolean;
      keyCode?: number;
      nativeEvent?: { isComposing?: boolean; keyCode?: number };
    }) =>
      Boolean(
        event.isComposing ||
          event.nativeEvent?.isComposing ||
          event.keyCode === 229 ||
          event.nativeEvent?.keyCode === 229 ||
          isComposingRef.current ||
          justEndedCompositionRef.current
      ),
    []
  );

  return {
    isComposingRef,
    justEndedCompositionRef,
    onCompositionStart,
    onCompositionEnd,
    isImeActive,
  };
}
